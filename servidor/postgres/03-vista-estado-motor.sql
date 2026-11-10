-- Vista con el estado de cada motor, para el bot de Telegram (n8n) y para Grafana.
-- Corre una sola vez, después de 02-esquema.sql, cuando la carpeta de datos está vacía.
-- Principio: el servidor calcula todo; la IA solo interpreta y redacta.
--
-- Ventanas:
--   * actual     = últimas 24 h
--   * línea base = los 7 días anteriores (de -8 d a -24 h)  -> detecta CAMBIOS (puntaje z)
--   * tendencia  = últimos 14 días, calentamiento contra el horómetro -> "llega a alarma en ~N h de marcha"
-- Temperatura, corriente y vibración: solo minutos en régimen (seg_marcha >= 50).
-- La vibración (rz, en g) no tiene umbral fijo: se evalúa por cambio respecto de la línea base.

\connect monitor

SET ROLE monitor_app;

CREATE OR REPLACE VIEW v_estado_motor AS
WITH
umbral AS (                     -- EDITAR ACÁ los umbrales
  SELECT 100.0 AS i_alerta_pct,  115.0 AS i_alarma_pct,   -- corriente, % de la In de placa
         60.0  AS t1_alerta,     75.0  AS t1_alarma,      -- carcasa, °C
         35.0  AS dt_alerta,     50.0  AS dt_alarma,      -- calentamiento sobre ambiente, °C
         3.0   AS z_alerta,      4.0   AS z_alarma,       -- desvíos respecto de la línea base
         15    AS min_sin_datos,                          -- minutos sin datos para considerar el kit caído
         60    AS min_base,                               -- minutos en régimen para que la línea base valga
         200.0 AS h_tendencia_alerta,                     -- aviso si la tendencia llega a alarma antes de N h
         -80   AS rssi_debil                              -- dBm
),
ult AS (                        -- último minuto recibido
  SELECT DISTINCT ON (kit_id) kit_id, ts, horas, arranques
  FROM minuto
  ORDER BY kit_id, ts DESC
),
d24 AS (                        -- últimas 24 h
  SELECT kit_id,
         count(*)                                                AS minutos,
         sum(seg_marcha) / 3600.0                                AS horas_marcha,
         max(arranques) - min(arranques)                         AS arranques,
         avg(t1_avg)  FILTER (WHERE seg_marcha >= 50)            AS t1,
         max(t1_max)                                             AS t1_max,
         avg(t2_avg)                                             AS t2,
         avg(dt_avg)  FILTER (WHERE seg_marcha >= 50)            AS dt,
         percentile_cont(0.95) WITHIN GROUP (ORDER BY dt_avg) FILTER (WHERE seg_marcha >= 50) AS dt_p95,
         avg(i_avg)   FILTER (WHERE seg_marcha >= 50)            AS i,
         percentile_cont(0.95) WITHIN GROUP (ORDER BY i_avg)  FILTER (WHERE seg_marcha >= 50) AS i_p95,
         avg(rz_avg)  FILTER (WHERE seg_marcha >= 50)            AS rz,
         max(pico_max) FILTER (WHERE seg_marcha >= 50)           AS pico,
         avg(rpm_avg) FILTER (WHERE seg_marcha >= 50 AND rpm_avg > 0) AS rpm
  FROM minuto
  WHERE ts >= now() - interval '24 hours'
  GROUP BY kit_id
),
base AS (                       -- línea base: 7 días anteriores
  SELECT kit_id,
         count(*)                                  AS minutos,
         avg(dt_avg) AS dt_mu, stddev_samp(dt_avg) AS dt_sd,
         avg(i_avg)  AS i_mu,  stddev_samp(i_avg)  AS i_sd,
         avg(rz_avg) AS rz_mu, stddev_samp(rz_avg) AS rz_sd
  FROM minuto
  WHERE ts >= now() - interval '8 days'
    AND ts <  now() - interval '24 hours'
    AND seg_marcha >= 50
  GROUP BY kit_id
),
tend AS (                       -- tendencia: 14 días, calentamiento contra el horómetro
  SELECT kit_id,
         count(*)                    AS minutos,
         max(horas) - min(horas)     AS horas_rango,
         regr_slope(dt_avg, horas)   AS dt_b, regr_intercept(dt_avg, horas) AS dt_a, regr_r2(dt_avg, horas) AS dt_r2
  FROM minuto
  WHERE ts >= now() - interval '14 days'
    AND seg_marcha >= 50
    AND horas IS NOT NULL
  GROUP BY kit_id
),
uso AS (                        -- horas de marcha por día (promedio de 7 días)
  SELECT kit_id, sum(seg_marcha) / 3600.0 / 7 AS horas_dia
  FROM minuto
  WHERE ts >= now() - interval '7 days'
  GROUP BY kit_id
),
ev AS (                         -- eventos de las últimas 24 h
  SELECT kit_id,
         count(*) FILTER (WHERE tipo LIKE 'ALARMA%' AND tipo <> 'ALARMA_NORMAL') AS alarmas,
         count(*) FILTER (WHERE tipo = 'RECONOCIMIENTO')                       AS reconocimientos,
         (array_agg(tipo || coalesce(' ' || detalle, '') ORDER BY ts DESC)
            FILTER (WHERE tipo LIKE 'ALARMA%' AND tipo <> 'ALARMA_NORMAL'))[1] AS ultima_alarma
  FROM evento
  WHERE ts >= now() - interval '24 hours'
  GROUP BY kit_id
),
mant AS (                       -- último mantenimiento registrado
  SELECT DISTINCT ON (kit_id) kit_id, fecha, horas, tipo, estado, intervalo_h, observaciones
  FROM mantenimiento
  ORDER BY kit_id, horas DESC NULLS LAST, fecha DESC NULLS LAST, id DESC
),
sal AS (                        -- última salud del kit
  SELECT DISTINCT ON (kit_id) kit_id, rssi, sd_ok, tramas_mal
  FROM salud
  ORDER BY kit_id, ts DESC
),
calc AS (
  SELECT
    k.id                                                          AS kit_id,
    k.nombre,
    k.in_a                                                        AS in_placa_a,
    k.rpm_nominal,
    -- conexión
    to_char(ult.ts AT TIME ZONE 'America/Argentina/Buenos_Aires', 'DD/MM/YYYY HH24:MI') AS ultimo_dato,
    round((extract(epoch FROM now() - ult.ts) / 60)::numeric)     AS min_sin_datos,
    round(d.minutos * 100.0 / 1440)                               AS cobertura_24h_pct,
    s.rssi, s.sd_ok, s.tramas_mal,
    -- uso
    round(ult.horas::numeric, 1)                                  AS horometro_h,
    ult.arranques                                                 AS arranques_total,
    round(d.horas_marcha::numeric, 2)                             AS horas_marcha_24h,
    d.arranques                                                   AS arranques_24h,
    round(uso.horas_dia::numeric, 2)                              AS horas_marcha_dia_prom7d,
    -- valores de las últimas 24 h (en régimen)
    round(d.t1::numeric, 1)                                       AS t1_carcasa_c,
    round(d.t1_max::numeric, 1)                                   AS t1_max_c,
    round(d.t2::numeric, 1)                                       AS t2_ambiente_c,
    round(d.dt::numeric, 1)                                       AS dt_c,
    round(d.dt_p95::numeric, 1)                                   AS dt_p95_c,
    round(d.i::numeric, 2)                                        AS i_a,
    round((100 * d.i_p95 / nullif(k.in_a, 0))::numeric)           AS i_p95_pct_in,
    round(d.rz::numeric, 3)                                       AS rz_g,
    round(d.pico::numeric, 3)                                     AS pico_max_g,
    round(d.rpm::numeric)                                         AS rpm,
    -- cambio respecto de la línea base (puntaje z; null si no hay base suficiente)
    base.minutos                                                  AS base_minutos,
    round(base.rz_mu::numeric, 3)                                 AS rz_base_g,
    CASE WHEN base.minutos >= u.min_base THEN round(((d.dt - base.dt_mu) / greatest(base.dt_sd, 0.5))::numeric, 1) END                 AS z_dt,
    CASE WHEN base.minutos >= u.min_base THEN round(((d.i  - base.i_mu)  / greatest(base.i_sd, 0.02 * coalesce(k.in_a, 1)))::numeric, 1) END AS z_i,
    CASE WHEN base.minutos >= u.min_base THEN round(((d.rz - base.rz_mu) / greatest(base.rz_sd, 0.005))::numeric, 1) END               AS z_rz,
    -- tendencia: horas de marcha hasta llegar al umbral de alarma (solo si la tendencia es clara)
    CASE WHEN t.minutos >= 120 AND t.horas_rango >= 5 AND t.dt_b > 0 AND t.dt_r2 >= 0.5
         THEN round(greatest(0, (u.dt_alarma - (t.dt_a + t.dt_b * ult.horas)) / t.dt_b)::numeric) END AS h_hasta_alarma_dt,
    -- eventos
    coalesce(ev.alarmas, 0)                                       AS alarmas_24h,
    coalesce(ev.reconocimientos, 0)                               AS reconocimientos_24h,
    ev.ultima_alarma,
    -- mantenimiento
    to_char(m.fecha, 'DD/MM/YYYY')                                AS ult_mant_fecha,
    m.tipo                                                        AS ult_mant_tipo,
    m.estado                                                      AS ult_mant_estado,
    m.observaciones                                               AS ult_mant_obs,
    m.intervalo_h                                                 AS intervalo_mant_h,
    round((ult.horas - m.horas)::numeric, 1)                      AS horas_desde_mant,
    round((m.intervalo_h - (ult.horas - m.horas))::numeric, 1)    AS horas_hasta_mant,
    round(((m.intervalo_h - (ult.horas - m.horas)) / nullif(uso.horas_dia, 0))::numeric) AS dias_hasta_mant_estimado
  FROM kit k
  CROSS JOIN umbral u
  LEFT JOIN ult  ON ult.kit_id  = k.id
  LEFT JOIN d24 d ON d.kit_id   = k.id
  LEFT JOIN base ON base.kit_id = k.id
  LEFT JOIN tend t ON t.kit_id  = k.id
  LEFT JOIN uso  ON uso.kit_id  = k.id
  LEFT JOIN ev   ON ev.kit_id   = k.id
  LEFT JOIN mant m ON m.kit_id  = k.id
  LEFT JOIN sal s  ON s.kit_id  = k.id
),
mot AS (
  SELECT c.*,
    array_remove(ARRAY[
      CASE WHEN c.i_p95_pct_in >= u.i_alarma_pct THEN format('Corriente sostenida %s%% de In (sobrecarga)', c.i_p95_pct_in) END,
      CASE WHEN c.t1_max_c     >= u.t1_alarma    THEN format('Carcasa llegó a %s °C', c.t1_max_c) END,
      CASE WHEN c.dt_p95_c     >= u.dt_alarma    THEN format('Calentamiento sobre ambiente %s °C', c.dt_p95_c) END,
      CASE WHEN c.ult_mant_estado = 'NOAPTO'     THEN 'Último mantenimiento: NO APTO' END
    ], NULL) AS motivos_rojo,
    array_remove(ARRAY[
      CASE WHEN c.i_p95_pct_in >= u.i_alerta_pct AND c.i_p95_pct_in < u.i_alarma_pct THEN format('Corriente sostenida %s%% de In', c.i_p95_pct_in) END,
      CASE WHEN c.t1_max_c     >= u.t1_alerta    AND c.t1_max_c     < u.t1_alarma    THEN format('Carcasa llegó a %s °C', c.t1_max_c) END,
      CASE WHEN c.dt_p95_c     >= u.dt_alerta    AND c.dt_p95_c     < u.dt_alarma    THEN format('Calentamiento sobre ambiente %s °C', c.dt_p95_c) END,
      -- El cambio respecto de la base nunca da rojo solo: el rojo queda para umbrales absolutos.
      CASE WHEN greatest(c.z_dt, c.z_i, c.z_rz) >= u.z_alerta
           THEN format('%s respecto de la semana anterior: %s',
                       CASE WHEN greatest(c.z_dt, c.z_i, c.z_rz) >= u.z_alarma THEN 'Cambio fuerte' ELSE 'Cambio' END,
                       concat_ws(', ',
                         CASE WHEN c.z_dt >= u.z_alerta THEN format('calentamiento z=%s', c.z_dt) END,
                         CASE WHEN c.z_i  >= u.z_alerta THEN format('corriente z=%s', c.z_i) END,
                         CASE WHEN c.z_rz >= u.z_alerta THEN format('vibración z=%s (%s g, antes %s g)', c.z_rz, c.rz_g, c.rz_base_g) END)) END,
      CASE WHEN c.h_hasta_alarma_dt < u.h_tendencia_alerta
           THEN format('El calentamiento llega a alarma en ~%s h de marcha', c.h_hasta_alarma_dt) END,
      CASE WHEN c.alarmas_24h > 0                THEN format('%s alarma(s) en el kit en 24 h', c.alarmas_24h) END,
      CASE WHEN c.horas_hasta_mant < 0           THEN format('Mantenimiento vencido hace %s h', -c.horas_hasta_mant)
           WHEN c.horas_hasta_mant <= 0.1 * c.intervalo_mant_h THEN format('Mantenimiento próximo: faltan %s h', c.horas_hasta_mant) END,
      CASE WHEN c.ult_mant_estado = 'OBS'        THEN 'Último mantenimiento con observaciones' END,
      CASE WHEN c.sd_ok = false                  THEN 'La SD del kit no responde' END,
      CASE WHEN c.rssi < u.rssi_debil            THEN format('Señal WiFi débil (%s dBm)', c.rssi) END
    ], NULL) AS motivos_amarillo
  FROM calc c CROSS JOIN umbral u
)
SELECT
  CASE WHEN mot.min_sin_datos IS NULL OR mot.min_sin_datos > u.min_sin_datos THEN 'SIN_DATOS'
       WHEN cardinality(mot.motivos_rojo) > 0     THEN 'ROJO'
       WHEN cardinality(mot.motivos_amarillo) > 0 THEN 'AMARILLO'
       ELSE 'VERDE'
  END AS estado,
  mot.*
FROM mot CROSS JOIN umbral u;

COMMENT ON VIEW v_estado_motor IS
  'Estado por motor: 24 h vs línea base de 7 días, tendencia de 14 días, mantenimiento y salud del kit. La usa el bot de Telegram (n8n).';

RESET ROLE;

-- n8n (bot) y Grafana leen con el usuario de solo lectura
GRANT SELECT ON v_estado_motor TO grafana_lector;
