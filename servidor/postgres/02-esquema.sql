-- Esquema de datos del kit de monitoreo (base "monitor")
-- Todas las fechas en UTC (timestamptz). Grafana las muestra en hora local.

\connect monitor

SET ROLE monitor_app;

-- ---------- Kits (un kit = un motor) ----------
CREATE TABLE IF NOT EXISTS kit (
  id           text PRIMARY KEY,                  -- ej. MOTOR-001 (mismo nombre que el HC-05)
  token_sha256 text NOT NULL,                     -- hash del token; el token en claro solo lo tiene el kit
  nombre       text,
  potencia_kw  real,
  in_a         real,                              -- corriente nominal de placa
  rpm_nominal  real,
  creado       timestamptz NOT NULL DEFAULT now()
);

-- ---------- Resumen por minuto ----------
CREATE TABLE IF NOT EXISTS minuto (
  kit_id      text        NOT NULL REFERENCES kit(id),
  ts          timestamptz NOT NULL,               -- inicio del minuto (UTC)
  n           smallint,                           -- tramas recibidas en el minuto (60 = completo)
  seg_marcha  smallint,                           -- segundos con el motor en marcha
  t1_avg real, t1_max real,                       -- carcasa (°C)
  t2_avg real,                                    -- ambiente (°C)
  dt_avg real GENERATED ALWAYS AS (t1_avg - t2_avg) STORED,   -- calentamiento sobre ambiente
  i_avg real, i_min real, i_max real,             -- corriente (A)
  rpm_avg real,
  rx_avg real, ry_avg real, rz_avg real,          -- vibración general (RMS) por eje (g); rz = radial
  rz_max real,                                    -- máximo del eje radial (g)
  pico_max real,                                  -- pico del eje radial (g)
  horas       real,                               -- horómetro al cierre del minuto
  arranques   integer,
  recibido    timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (kit_id, ts)
);

-- ---------- Eventos (arranques, alarmas, reconocimientos, ajustes) ----------
CREATE TABLE IF NOT EXISTS evento (
  kit_id   text        NOT NULL REFERENCES kit(id),
  ts       timestamptz NOT NULL,
  tipo     text        NOT NULL,                  -- ej. ALARMA_TEMP, RECONOCIMIENTO, ARRANQUE_ESP32
  origen   text        NOT NULL DEFAULT '',       -- pulsador, app, sistema, uno
  detalle  text,
  recibido timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (kit_id, ts, tipo, origen)
);

-- ---------- Salud del kit ----------
CREATE TABLE IF NOT EXISTS salud (
  kit_id     text        NOT NULL REFERENCES kit(id),
  ts         timestamptz NOT NULL,
  rssi       smallint,                            -- señal WiFi (dBm)
  heap_libre integer,                             -- bytes
  uptime_s   integer,
  tramas_mal integer,
  sd_ok      boolean,
  recibido   timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (kit_id, ts)
);

-- ---------- Mantenimientos (copia del /mantenimientos.csv del kit) ----------
CREATE TABLE IF NOT EXISTS mantenimiento (
  kit_id         text NOT NULL REFERENCES kit(id),
  id             integer NOT NULL,                -- id que asignó el kit
  fecha          timestamp,                       -- hora local, tal como la cargó el celular
  horas          real,
  tipo           text,                            -- PREV / CORR / PRED / INSP
  responsable    text,
  tareas         text,                            -- códigos unidos con +
  aislacion_mohm real,
  corriente_a    text,
  repuestos      text,
  estado         text,                            -- APTO / OBS / NOAPTO
  intervalo_h    real,
  observaciones  text,
  recibido       timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (kit_id, id)
);

CREATE INDEX IF NOT EXISTS minuto_ts_idx ON minuto (ts);
CREATE INDEX IF NOT EXISTS evento_ts_idx ON evento (ts);

-- ---------- Ingesta ----------
-- n8n llama: SELECT ingesta(<cuerpo JSON>, <token del header>)
-- Verifica el token y las fechas, inserta todo e ignora duplicados (reintentos del kit).
-- Un mantenimiento con el mismo id actualiza el registro: el firmware no debe reutilizar ids.
CREATE OR REPLACE FUNCTION ingesta(p jsonb, token text)
RETURNS jsonb
LANGUAGE plpgsql
AS $$
DECLARE
  v_kit text := p->>'kit';
  n_min int := 0;
  n_ev  int := 0;
  n_sal int := 0;
  n_man int := 0;
BEGIN
  IF v_kit IS NULL OR token IS NULL OR NOT EXISTS (
       SELECT 1 FROM kit
       WHERE id = v_kit
         AND token_sha256 = encode(sha256(convert_to(token, 'UTF8')), 'hex')) THEN
    RETURN jsonb_build_object('ok', false, 'error', 'NO_AUTORIZADO');
  END IF;

  IF coalesce((p->>'v')::int, 0) <> 1 THEN
    RETURN jsonb_build_object('ok', false, 'error', 'VERSION');
  END IF;

  -- Un kit sin hora sincronizada manda fechas de 1970: se rechaza el envío en vez de guardarlas.
  IF EXISTS (
       SELECT 1 FROM (
         SELECT (x->>'ts')::bigint AS ts FROM jsonb_array_elements(coalesce(p->'minutos', '[]')) AS x
         UNION ALL
         SELECT (x->>'ts')::bigint FROM jsonb_array_elements(coalesce(p->'eventos', '[]')) AS x
         UNION ALL
         SELECT (p->'salud'->>'ts')::bigint
       ) AS f
       WHERE f.ts < 1704067200                               -- antes de 2024
          OR f.ts > extract(epoch FROM now())::bigint + 86400) THEN  -- más de un día en el futuro
    RETURN jsonb_build_object('ok', false, 'error', 'FECHA');
  END IF;

  INSERT INTO minuto (kit_id, ts, n, seg_marcha, t1_avg, t1_max, t2_avg,
                      i_avg, i_min, i_max, rpm_avg, rx_avg, ry_avg, rz_avg, rz_max,
                      pico_max, horas, arranques)
  SELECT v_kit, to_timestamp(r.ts), r.n, r.marcha, r.t1, r.t1max, r.t2,
         r.i, r.imin, r.imax, r.rpm, r.rx, r.ry, r.rz, r.rzmax,
         r.pico, r.horas, r.arr
  FROM jsonb_to_recordset(coalesce(p->'minutos', '[]')) AS r(
         ts bigint, n smallint, marcha smallint, t1 real, t1max real, t2 real,
         i real, imin real, imax real, rpm real, rx real, ry real, rz real, rzmax real,
         pico real, horas real, arr int)
  WHERE r.ts IS NOT NULL
  ON CONFLICT DO NOTHING;
  GET DIAGNOSTICS n_min = ROW_COUNT;

  INSERT INTO evento (kit_id, ts, tipo, origen, detalle)
  SELECT v_kit, to_timestamp(e.ts), e.tipo, coalesce(e.origen, ''), e.detalle
  FROM jsonb_to_recordset(coalesce(p->'eventos', '[]')) AS e(
         ts bigint, tipo text, origen text, detalle text)
  WHERE e.ts IS NOT NULL AND e.tipo IS NOT NULL
  ON CONFLICT DO NOTHING;
  GET DIAGNOSTICS n_ev = ROW_COUNT;

  IF p ? 'salud' THEN
    INSERT INTO salud (kit_id, ts, rssi, heap_libre, uptime_s, tramas_mal, sd_ok)
    SELECT v_kit, to_timestamp(s.ts), s.rssi, s.heap, s.uptime, s.err, s.sd
    FROM jsonb_to_record(p->'salud') AS s(
           ts bigint, rssi smallint, heap int, uptime int, err int, sd boolean)
    WHERE s.ts IS NOT NULL
    ON CONFLICT DO NOTHING;
    GET DIAGNOSTICS n_sal = ROW_COUNT;
  END IF;

  INSERT INTO mantenimiento (kit_id, id, fecha, horas, tipo, responsable, tareas,
                             aislacion_mohm, corriente_a, repuestos, estado,
                             intervalo_h, observaciones)
  SELECT v_kit, m.id, m.fecha::timestamp, m.horas, m.tipo, m.responsable, m.tareas,
         m.aislacion_mohm, m.corriente_a, m.repuestos, m.estado,
         m.intervalo_h, m.observaciones
  FROM jsonb_to_recordset(coalesce(p->'mantenimientos', '[]')) AS m(
         id int, fecha text, horas real, tipo text, responsable text, tareas text,
         aislacion_mohm real, corriente_a text, repuestos text, estado text,
         intervalo_h real, observaciones text)
  WHERE m.id IS NOT NULL
  ON CONFLICT (kit_id, id) DO UPDATE
    SET fecha = EXCLUDED.fecha, horas = EXCLUDED.horas, tipo = EXCLUDED.tipo,
        responsable = EXCLUDED.responsable, tareas = EXCLUDED.tareas,
        aislacion_mohm = EXCLUDED.aislacion_mohm, corriente_a = EXCLUDED.corriente_a,
        repuestos = EXCLUDED.repuestos, estado = EXCLUDED.estado,
        intervalo_h = EXCLUDED.intervalo_h, observaciones = EXCLUDED.observaciones,
        recibido = now()
    WHERE (mantenimiento.fecha, mantenimiento.horas, mantenimiento.tipo, mantenimiento.responsable,
           mantenimiento.tareas, mantenimiento.aislacion_mohm, mantenimiento.corriente_a,
           mantenimiento.repuestos, mantenimiento.estado, mantenimiento.intervalo_h,
           mantenimiento.observaciones)
          IS DISTINCT FROM
          (EXCLUDED.fecha, EXCLUDED.horas, EXCLUDED.tipo, EXCLUDED.responsable,
           EXCLUDED.tareas, EXCLUDED.aislacion_mohm, EXCLUDED.corriente_a,
           EXCLUDED.repuestos, EXCLUDED.estado, EXCLUDED.intervalo_h,
           EXCLUDED.observaciones);
  GET DIAGNOSTICS n_man = ROW_COUNT;

  RETURN jsonb_build_object('ok', true, 'minutos', n_min, 'eventos', n_ev,
                            'salud', n_sal, 'mantenimientos', n_man);
EXCEPTION WHEN others THEN
  RETURN jsonb_build_object('ok', false, 'error', 'DATOS', 'detalle', SQLERRM);
END;
$$;

-- ---------- Alta de un kit ----------
-- Uso: SELECT alta_kit('MOTOR-001', 'token-largo-y-secreto', 'Cortadora WEG 1 CV', 0.75, 4.65, 2850);
CREATE OR REPLACE FUNCTION alta_kit(p_id text, p_token text, p_nombre text,
                                    p_kw real, p_in real, p_rpm real)
RETURNS void LANGUAGE sql AS $$
  INSERT INTO kit (id, token_sha256, nombre, potencia_kw, in_a, rpm_nominal)
  VALUES (p_id, encode(sha256(convert_to(p_token, 'UTF8')), 'hex'), p_nombre, p_kw, p_in, p_rpm)
  ON CONFLICT (id) DO UPDATE
    SET token_sha256 = EXCLUDED.token_sha256, nombre = EXCLUDED.nombre,
        potencia_kw = EXCLUDED.potencia_kw, in_a = EXCLUDED.in_a,
        rpm_nominal = EXCLUDED.rpm_nominal;
$$;

RESET ROLE;

-- Grafana solo lee (y no ve los hashes de los tokens)
GRANT USAGE ON SCHEMA public TO grafana_lector;
GRANT SELECT ON minuto, evento, salud, mantenimiento TO grafana_lector;
GRANT SELECT (id, nombre, potencia_kw, in_a, rpm_nominal, creado) ON kit TO grafana_lector;
