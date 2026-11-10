// servidor.h — Kit ESP32 v0.6
// WiFi, hora por NTP, resumen por minuto y envío directo al webhook de n8n.
// Contrato de datos: docs/MANUAL.md, sección "Contrato de datos".
// Incluir DESPUÉS de mantenimiento.h (usa mantCampo, horasActuales y arranques).

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <time.h>
#include "esp_sntp.h"
#include "secretos.h" //(no se sube al repo)

// Configuración
// Reemplazar por la URL del webhook de tu servidor.
const char*         SERVIDOR_URL      = "https://n8n.ejemplo.com/webhook/kit";
const char*         KIT_ID            = "MOTOR-001";
const char*         USER_AGENT        = "monitor-kit/0.6";
const char*         ARCH_RECHAZOS     = "/envios_rechazados.csv";
const uint16_t      HTTP_TIMEOUT_MS   = 5000;
const unsigned long ENVIO_CADA_MS     = 60000;    // un envío por minuto
const unsigned long REINTENTO_WIFI_MS = 30000;
const unsigned long SERVIDOR_OK_MS    = 180000;   // "conectado" si el último envío OK fue hace < 3 min
const uint32_t      EPOCH_MINIMO      = 1704067200;   // 2024-01-01: antes de esto la hora no es válida
const byte          MAX_MINUTOS       = 15;
const byte          MAX_EVENTOS       = 20;
const byte          MAX_MANT          = 5;

// Estructuras
struct Minuto {
  uint32_t ts;
  uint16_t n, marcha;
  float t1, t1max, t2, i, imin, imax, rpm, horas;
  float rx, ry, rz, rzmax, pico;
  uint32_t arr;
};

struct Evento {
  uint32_t ts;
  char tipo[24];
  char origen[12];
  char detalle[48];
};

struct Acumulador {
  bool activo;
  uint32_t ts;
  uint16_t n, marcha, nT1, nT2, nV;
  float sT1, maxT1, sT2, sI, minI, maxI, sRpm;
  float sRx2, sRy2, sRz2, maxRz, maxPico;
};

// Estado
Acumulador acum = {};
Minuto     minutosPend[MAX_MINUTOS];  byte cantMinPend  = 0;
Evento     eventosPend[MAX_EVENTOS];  byte cantEvPend   = 0;
char       mantPend[MAX_MANT][420];   byte cantMantPend = 0;

WiFiClientSecure clienteTls;
volatile bool ntpRecibido = false;        // lo levanta el callback de SNTP
unsigned long msUltimoEnvio       = 0;
unsigned long msUltimoIntentoWifi = 0;
unsigned long msUltimoEnvioOk     = 0;
int           ultimoCodigoHttp    = 0;
unsigned long tramasMalEnviadas   = 0;
bool          wifiAvisado         = false;

// Utilidades
bool horaValidaServidor() {
  return (uint32_t)time(nullptr) >= EPOCH_MINIMO;
}

bool wifiConectado() {
  return WiFi.status() == WL_CONNECTED;
}

bool servidorConectado() {
  return msUltimoEnvioOk != 0 && (millis() - msUltimoEnvioOk < SERVIDOR_OK_MS);
}

void alSincronizarNtp(struct timeval* t) {
  ntpRecibido = true;
}

// Convierte texto a número aceptando coma decimal. false si no es un número completo
bool aNumero(const char* txt, float& v) {
  char b[24];
  size_t n = strlen(txt);
  if (n == 0 || n >= sizeof(b)) return false;
  for (size_t k = 0; k <= n; k++) b[k] = (txt[k] == ',') ? '.' : txt[k];
  char* fin;
  v = strtof(b, &fin);
  return fin != b && *fin == '\0';
}

void jsonTexto(String& s, const char* v) {
  s += '"';
  for (const char* p = v; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { s += '\\'; s += c; }
    else if ((uint8_t)c < 0x20) s += ' ';
    else s += c;
  }
  s += '"';
}

void jsonNum(String& s, float v, byte decimales) {
  if (isnan(v) || isinf(v)) { s += "null"; return; }
  char b[24];
  snprintf(b, sizeof(b), "%.*f", decimales, v);
  s += b;
}

// Resumen por minuto
void cerrarMinuto() {
  if (!acum.activo) return;
  acum.activo = false;
  if (acum.n == 0 && acum.nV == 0) return;

  Minuto m;
  m.ts     = acum.ts;
  m.n      = acum.n;
  m.marcha = acum.marcha;
  m.t1     = acum.nT1 ? acum.sT1 / acum.nT1 : NAN;
  m.t1max  = acum.nT1 ? acum.maxT1 : NAN;
  m.t2     = acum.nT2 ? acum.sT2 / acum.nT2 : NAN;
  m.i      = acum.n ? acum.sI / acum.n : NAN;
  m.imin   = acum.n ? acum.minI : NAN;
  m.imax   = acum.n ? acum.maxI : NAN;
  m.rpm    = acum.n ? acum.sRpm / acum.n : NAN;
  m.rx     = acum.nV ? sqrt(acum.sRx2 / acum.nV) : NAN;     // RMS del minuto
  m.ry     = acum.nV ? sqrt(acum.sRy2 / acum.nV) : NAN;
  m.rz     = acum.nV ? sqrt(acum.sRz2 / acum.nV) : NAN;
  m.rzmax  = acum.nV ? acum.maxRz : NAN;
  m.pico   = acum.nV ? acum.maxPico : NAN;
  m.horas  = horasActuales();
  m.arr    = arranques;

  if (cantMinPend == MAX_MINUTOS) {                 // lleno: descarta el más viejo
    memmove(&minutosPend[0], &minutosPend[1], sizeof(Minuto) * (MAX_MINUTOS - 1));
    cantMinPend--;
  }
  minutosPend[cantMinPend++] = m;
}

// Abre el minuto actual (cerrando el anterior si cambió). false si no hay hora válida
bool asegurarMinuto() {
  if (!horaValidaServidor()) return false;
  uint32_t ts = ((uint32_t)time(nullptr) / 60) * 60;     // inicio del minuto UTC
  if (acum.activo && ts != acum.ts) cerrarMinuto();
  if (!acum.activo) {
    acum = Acumulador{};
    acum.activo = true;
    acum.ts = ts;
    acum.minI = 1e9;
    acum.maxI = -1e9;
    acum.maxT1 = -1e9;
  }
  return true;
}

// Llamar con cada trama D válida del UNO
void servidorDato(float t1, float t2, float i, float rpm, bool marcha) {
  if (!asegurarMinuto()) return;
  acum.n++;
  if (marcha) acum.marcha++;
  if (t1 > -100) { acum.sT1 += t1; acum.nT1++; if (t1 > acum.maxT1) acum.maxT1 = t1; }
  if (t2 > -100) { acum.sT2 += t2; acum.nT2++; }
  acum.sI += i;
  if (i < acum.minI) acum.minI = i;
  if (i > acum.maxI) acum.maxI = i;
  acum.sRpm += rpm;
}

// Llamar con cada trama V válida del UNO
void servidorVib(float rx, float ry, float rz, float pico) {
  if (!asegurarMinuto()) return;
  acum.nV++;
  acum.sRx2 += rx * rx;
  acum.sRy2 += ry * ry;
  acum.sRz2 += rz * rz;
  if (rz > acum.maxRz) acum.maxRz = rz;
  if (pico > acum.maxPico) acum.maxPico = pico;
}

void servidorEvento(uint32_t ts, const char* tipo, const char* origen, const char* detalle) {
  if (ts < EPOCH_MINIMO) return;                      // sin hora válida: solo queda en la SD
  if (cantEvPend == MAX_EVENTOS) {
    memmove(&eventosPend[0], &eventosPend[1], sizeof(Evento) * (MAX_EVENTOS - 1));
    cantEvPend--;
  }
  Evento& e = eventosPend[cantEvPend++];
  e.ts = ts;
  strlcpy(e.tipo, tipo, sizeof(e.tipo));
  strlcpy(e.origen, origen, sizeof(e.origen));
  strlcpy(e.detalle, detalle, sizeof(e.detalle));
}

// Recibe la línea tal como quedó en /mantenimientos.csv
void servidorMantenimiento(const char* lineaSd) {
  if (cantMantPend == MAX_MANT) {
    memmove(&mantPend[0], &mantPend[1], sizeof(mantPend[0]) * (MAX_MANT - 1));
    cantMantPend--;
  }
  strlcpy(mantPend[cantMantPend++], lineaSd, sizeof(mantPend[0]));
}

// Armado del JSON
void jsonMinuto(String& s, const Minuto& m) {
  s += "{\"ts\":";      s += m.ts;
  s += ",\"n\":";       s += m.n;
  s += ",\"marcha\":";  s += m.marcha;
  s += ",\"t1\":";      jsonNum(s, m.t1, 1);
  s += ",\"t1max\":";   jsonNum(s, m.t1max, 1);
  s += ",\"t2\":";      jsonNum(s, m.t2, 1);
  s += ",\"i\":";       jsonNum(s, m.i, 2);
  s += ",\"imin\":";    jsonNum(s, m.imin, 2);
  s += ",\"imax\":";    jsonNum(s, m.imax, 2);
  s += ",\"rpm\":";     jsonNum(s, m.rpm, 0);
  s += ",\"rx\":";      jsonNum(s, m.rx, 3);
  s += ",\"ry\":";      jsonNum(s, m.ry, 3);
  s += ",\"rz\":";      jsonNum(s, m.rz, 3);
  s += ",\"rzmax\":";   jsonNum(s, m.rzmax, 3);
  s += ",\"pico\":";    jsonNum(s, m.pico, 3);
  s += ",\"horas\":";   jsonNum(s, m.horas, 1);
  s += ",\"arr\":";     s += m.arr;
  s += '}';
}

void jsonEvento(String& s, const Evento& e) {
  s += "{\"ts\":";       s += e.ts;
  s += ",\"tipo\":";     jsonTexto(s, e.tipo);
  s += ",\"origen\":";   jsonTexto(s, e.origen);
  s += ",\"detalle\":";  jsonTexto(s, e.detalle);
  s += '}';
}

// id;fecha;horas;tipo;responsable;tareas;aisl;corriente;repuestos;estado;intervalo;obs
void jsonMantenimiento(String& s, const char* l) {
  char c[260];
  float v;
  s += "{\"id\":";                mantCampo(l, 0, c, sizeof(c));  s += (uint32_t)atol(c);
  s += ",\"fecha\":";             mantCampo(l, 1, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"horas\":";             mantCampo(l, 2, c, sizeof(c));  jsonNum(s, aNumero(c, v) ? v : NAN, 1);
  s += ",\"tipo\":";              mantCampo(l, 3, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"responsable\":";       mantCampo(l, 4, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"tareas\":";            mantCampo(l, 5, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"aislacion_mohm\":";    mantCampo(l, 6, c, sizeof(c));  jsonNum(s, aNumero(c, v) ? v : NAN, 1);
  s += ",\"corriente_a\":";       mantCampo(l, 7, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"repuestos\":";         mantCampo(l, 8, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"estado\":";            mantCampo(l, 9, c, sizeof(c));  jsonTexto(s, c);
  s += ",\"intervalo_h\":";       mantCampo(l, 10, c, sizeof(c)); jsonNum(s, aNumero(c, v) ? v : NAN, 0);
  s += ",\"observaciones\":";     mantCampo(l, 11, c, sizeof(c)); jsonTexto(s, c);
  s += '}';
}

void armarCuerpo(String& s, bool sdOk, unsigned long errores) {
  s.reserve(12288);
  s = "{\"v\":1,\"kit\":";
  jsonTexto(s, KIT_ID);

  if (cantMinPend) {
    s += ",\"minutos\":[";
    for (byte k = 0; k < cantMinPend; k++) { if (k) s += ','; jsonMinuto(s, minutosPend[k]); }
    s += ']';
  }
  if (cantEvPend) {
    s += ",\"eventos\":[";
    for (byte k = 0; k < cantEvPend; k++) { if (k) s += ','; jsonEvento(s, eventosPend[k]); }
    s += ']';
  }
  if (cantMantPend) {
    s += ",\"mantenimientos\":[";
    for (byte k = 0; k < cantMantPend; k++) { if (k) s += ','; jsonMantenimiento(s, mantPend[k]); }
    s += ']';
  }
  s += ",\"salud\":{\"ts\":";  s += (uint32_t)time(nullptr);
  s += ",\"rssi\":";           s += WiFi.RSSI();
  s += ",\"heap\":";           s += ESP.getFreeHeap();
  s += ",\"uptime\":";         s += (uint32_t)(millis() / 1000);
  s += ",\"err\":";            s += errores;
  s += ",\"sd\":";             s += sdOk ? "true" : "false";
  s += "}}";
}

// Envío
void vaciarPendientes() {
  cantMinPend = 0;
  cantEvPend = 0;
  cantMantPend = 0;
}

void registrarRechazo(bool sdOk, int codigo, const String& resp) {
  Serial.printf("Envio rechazado (HTTP %d): %s\n", codigo, resp.substring(0, 200).c_str());
  if (resp.startsWith("<")) Serial.println("La respuesta es HTML: probablemente la frena Cloudflare (Bot Fight Mode)");
  if (!sdOk) return;
  String r = resp.substring(0, 150);
  r.replace(',', ' ');
  r.replace('\n', ' ');
  r.replace('\r', ' ');
  bool nuevo = !SD.exists(ARCH_RECHAZOS);
  File f = SD.open(ARCH_RECHAZOS, FILE_APPEND);
  if (!f) return;
  if (nuevo) f.println("ts,codigo,respuesta,minutos,eventos,mantenimientos");
  f.printf("%lu,%d,%s,%d,%d,%d\n", (unsigned long)time(nullptr), codigo, r.c_str(),
           cantMinPend, cantEvPend, cantMantPend);
  f.close();
}

void enviarAlServidor(bool sdOk, unsigned long tramasMal) {
  String cuerpo;
  armarCuerpo(cuerpo, sdOk, tramasMal - tramasMalEnviadas);

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(clienteTls, SERVIDOR_URL)) {
    ultimoCodigoHttp = -1;
    Serial.println("Envio: no se pudo iniciar la conexion");
    return;
  }
  http.setUserAgent(USER_AGENT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Kit-Token", KIT_TOKEN);

  int codigo = http.POST(cuerpo);
  String resp = (codigo > 0) ? http.getString() : HTTPClient::errorToString(codigo);
  http.end();
  ultimoCodigoHttp = codigo;

  if (codigo == 200) {
    Serial.printf("Envio: HTTP 200 %s\n", resp.c_str());
    vaciarPendientes();
    tramasMalEnviadas = tramasMal;
    msUltimoEnvioOk = millis();
  } else if (codigo <= 0 || codigo == 429 || codigo >= 500) {
    // Timeout, red, límite de pedidos o error del servidor: se conserva y se reintenta
    Serial.printf("Envio: sin conexion o error temporal (%d: %s). Pendientes: %d min, %d ev, %d mant\n",
                  codigo, resp.substring(0, 80).c_str(), cantMinPend, cantEvPend, cantMantPend);
  } else {
    // 400, 401, 403...: no se reintenta. Se registra y se descarta (los datos siguen en la SD)
    registrarRechazo(sdOk, codigo, resp);
    vaciarPendientes();
    tramasMalEnviadas = tramasMal;
  }
}

// setup y loop
void servidorSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  sntp_set_time_sync_notification_cb(alSincronizarNtp);
  configTime(0, 0, "pool.ntp.org", "time.google.com");    // UTC
  clienteTls.useBuiltinCACertBundle();                    // certificados raíz del core
  Serial.printf("WiFi: conectando a %s\n", WIFI_SSID);
}

void servidorLoop(bool sdOk, unsigned long tramasMal) {
  // Cierra el minuto aunque el UNO haya dejado de mandar
  uint32_t ahora = (uint32_t)time(nullptr);
  if (acum.activo && ahora >= EPOCH_MINIMO && ahora >= acum.ts + 65) cerrarMinuto();

  // WiFi
  if (wifiConectado()) {
    if (!wifiAvisado) {
      Serial.printf("WiFi: conectado (IP %s, RSSI %d)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      wifiAvisado = true;
    }
  } else {
    if (wifiAvisado) { Serial.println("WiFi: desconectado"); wifiAvisado = false; }
    if (millis() - msUltimoIntentoWifi > REINTENTO_WIFI_MS) {
      msUltimoIntentoWifi = millis();
      WiFi.reconnect();
    }
  }

  // Envío cada minuto, solo con WiFi y hora válida
  if (millis() - msUltimoEnvio >= ENVIO_CADA_MS) {
    msUltimoEnvio = millis();
    if (wifiConectado() && horaValidaServidor()) enviarAlServidor(sdOk, tramasMal);
  }
}
