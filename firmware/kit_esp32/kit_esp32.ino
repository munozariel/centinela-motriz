// Kit ESP32 - Gateway v0.6
// Recibe datos del UNO, guarda en SD, reenvía por Bluetooth (HC-05),
// muestra en OLED, indica el estado con el LED RGB y envía un resumen por minuto al servidor.
// - Horómetro y registro de mantenimientos (ver mantenimiento.h).
// - Hora en UTC: RTC DS3231, comando TSET (recibe hora local) y hora por NTP con WiFi.
// - Tramas del UNO: datos $D, vibración $V, alarmas $A, arranque $S, reconocimiento $E.
// - WiFi, resumen por minuto y envío directo al webhook de n8n (ver servidor.h).

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <sys/time.h>
#include <time.h>
#include "mantenimiento.h"
#include "servidor.h" // siempre va después de mantenimiento.h

enum EstadoSistema { FALLA, APP_CONECTADA, SERVIDOR_OK, SIN_INTERNET };

// Pines
const byte PIN_UNO_RX   = 16;   // desde D13 del Arduino UNO (vía conversor)
const byte PIN_UNO_TX   = 17;   // hacia D12 del Arduino UNO (vía conversor)
const byte PIN_BT_RX    = 26;   // TXD del HC-05
const byte PIN_BT_TX    = 27;   // RXD del HC-05
const byte PIN_BT_STATE = 34;   // STATE del HC-05 (alto = celular conectado), con 10k a GND
const byte PIN_SD_CS    = 5;
const byte PIN_RGB_R    = 32;
const byte PIN_RGB_G    = 33;
const byte PIN_RGB_B    = 25;
// I2C: SDA GPIO21, SCL GPIO22 -> OLED (0x3C) y DS3231 (0x68)

// Configuración
const int  ZONA_HORARIA             = -3;       // UTC (Argentina)
const bool RGB_ANODO_COMUN          = true;
const unsigned long TIMEOUT_UNO_MS  = 3000;     // si no recibe datos del Arduino UNO => falla
const unsigned long TIMEOUT_APP_MS  = 30000;    // si no hay mensajes del teléfono => app desconectada
const unsigned long REINTENTO_SD_MS = 10000;
const unsigned long SYNC_OK_MS      = 3600000;  // resincronización del reloj desde el RTC cada 1 hora
const unsigned long SYNC_FALLA_MS   = 60000;    // en caso de no poder sincronizar lo intenta cada 1 min
const unsigned long DEBUG_MS        = 2000;     // diagnóstico por Monitor Serie

// Objetos
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RTC_DS3231 rtc;

// Estado
struct Datos { float t1, t2, ax, ay, az, rpm, irms; };
Datos ultimo = {0, 0, 0, 0, 0, 0, 0};
bool hayDatos = false;
unsigned long msUltimoDato = 0;

struct Vib { float rx, ry, rz, pico; };
Vib  ultimaVib = {0, 0, 0, 0};
bool hayVib = false;
int  estadoMotor = -1;    // semáforo en el Arduino UNO: 0 normal, 1 alerta, 2 alarma, 3 reconocida; -1 sin dato

unsigned long msUltimaApp  = 0;
unsigned long msUltimoIntentoSd = 0;

bool oledOk      = false;
bool sdOk        = false;
bool relojOk     = false;
bool rtcPresente = false;
bool unoAdxl = true;      // se actualizan con la trama S del Arduino UNO
int  unoTemps = -1;       // -1 = todavía no llegó la trama S

unsigned long registros = 0;
unsigned long tramasMal = 0;

char bufUno[100];  byte     posUno = 0;
char bufBt[400];   uint16_t posBt  = 0;   // un MADD ocupa hasta ~380 caracteres

// Utilidades de trama
byte calcularChecksum(const char* s) {
  byte cs = 0;
  while (*s) cs ^= *s++;
  return cs;
}

bool validarTrama(char* l, char*& contenido) {
  if (l[0] != '$') return false;
  char* asterisco = strrchr(l, '*');
  if (!asterisco || strlen(asterisco) < 3) return false;
  *asterisco = '\0';
  byte recibido = (byte)strtol(asterisco + 1, nullptr, 16);
  contenido = l + 1;
  return calcularChecksum(contenido) == recibido;
}

void enviarAlUno(const char* contenido) {
  Serial2.printf("$%s*%02X\n", contenido, calcularChecksum(contenido));
}

// Reloj (UTC)
// El RTC guarda UTC. Una lectura alcanza, con control de rango y de pérdida de energía
bool leerRtcConfiable(uint32_t &unix) {
  if (!rtcPresente || rtc.lostPower()) return false;
  DateTime a = rtc.now();
  if (a.year() < 2025 || a.year() > 2099) return false;
  unix = a.unixtime();
  return true;
}

// Pone en hora el reloj interno del ESP32 (UTC) a partir del RTC
void sincronizarReloj() {
  uint32_t unix;
  if (leerRtcConfiable(unix)) {
    timeval tv = { (time_t)unix, 0 };
    settimeofday(&tv, nullptr);
    relojOk = true;
    Serial.println("Reloj sincronizado desde el RTC (UTC)");
  } else {
    Serial.println("RTC sin hora valida: se mantiene el reloj interno");
  }
}

// Epoch UTC (segundos desde 1970). 0 si el reloj no tiene hora válida
uint32_t epochUtc() {
  return relojOk ? (uint32_t)time(nullptr) : 0;
}

// Hora local (UTC + ZONA_HORARIA) para pantalla y nombres de archivo
bool obtenerHora(struct tm &t) {
  if (!relojOk) return false;
  time_t local = time(nullptr) + (time_t)ZONA_HORARIA * 3600;
  gmtime_r(&local, &t);
  return true;
}

void fechaHora(char* destino, size_t tam) {
  struct tm t;
  if (obtenerHora(t)) strftime(destino, tam, "%Y-%m-%d %H:%M:%S", &t);
  else                snprintf(destino, tam, "sin-hora");
}

// prefijo: "datos" o "vib" -> /datos_AAAAMMDD.csv, /vib_AAAAMMDD.csv
void nombreArchivoDia(const char* prefijo, char* destino, size_t tam) {
  struct tm t;
  if (obtenerHora(t)) {
    char fecha[12];
    strftime(fecha, sizeof(fecha), "%Y%m%d", &t);
    snprintf(destino, tam, "/%s_%s.csv", prefijo, fecha);
  } else {
    snprintf(destino, tam, "/%s_sinfecha.csv", prefijo);
  }
}

// SD
void montarSd() {
  msUltimoIntentoSd = millis();
  sdOk = SD.begin(PIN_SD_CS);
  Serial.println(sdOk ? "SD: OK" : "SD: no se pudo montar");
}

void guardarEnSd(const char* ruta, const char* encabezado, const char* linea) {
  if (!sdOk) return;
  bool nuevo = !SD.exists(ruta);
  File f = SD.open(ruta, FILE_APPEND);
  if (!f) {
    sdOk = false;                      // se reintentará montar
    Serial.println("SD: error al escribir");
    return;
  }
  if (nuevo) f.println(encabezado);
  f.println(linea);
  f.close();                           // cerrar siempre: evita corrupción por cortes
}

// Guarda el evento en la SD y lo deja pendiente para el servidor
void registrarEvento(const char* tipo, const char* origen, const char* detalle) {
  char fh[24], linea[140];
  uint32_t ts = epochUtc();
  fechaHora(fh, sizeof(fh));
  snprintf(linea, sizeof(linea), "%lu,%s,%s,%s,%s",
           (unsigned long)ts, fh, tipo, origen, detalle);
  guardarEnSd("/eventos.csv", "ts,fecha_hora,evento,origen,detalle", linea);
  servidorEvento(ts, tipo, origen, detalle);
  Serial.printf("Evento: %s\n", linea);
}

// Estado del sistema
bool unoVivo() {
  return hayDatos && (millis() - msUltimoDato < TIMEOUT_UNO_MS);
}

bool sensoresOk() {
  if (unoTemps < 0) return true;       // sin trama S todavía: no se juzga
  return unoAdxl && unoTemps >= 1;
}

bool appConectada() {
  if (digitalRead(PIN_BT_STATE) == HIGH) return true;   // HC-05 informa conexión
  return msUltimaApp != 0 && (millis() - msUltimaApp < TIMEOUT_APP_MS);
}

EstadoSistema calcularEstado() {
  if (!unoVivo() || !sdOk || !sensoresOk()) return FALLA;
  if (appConectada()) return APP_CONECTADA;
  if (servidorConectado()) return SERVIDOR_OK;
  return SIN_INTERNET;
}

// LED RGB
void ponerColor(byte r, byte g, byte b) {
  if (RGB_ANODO_COMUN) { r = 255 - r; g = 255 - g; b = 255 - b; }
  analogWrite(PIN_RGB_R, r);
  analogWrite(PIN_RGB_G, g);
  analogWrite(PIN_RGB_B, b);
}

void actualizarRgb() {
  bool parpadeo = (millis() / 500) % 2 == 0;
  switch (calcularEstado()) {
    case FALLA:         parpadeo ? ponerColor(255, 0, 0) : ponerColor(0, 0, 0); break;
    case APP_CONECTADA: ponerColor(160, 0, 255); break;
    case SERVIDOR_OK:   ponerColor(0, 0, 255); break;                              // azul fijo
    case SIN_INTERNET:  parpadeo ? ponerColor(0, 0, 255) : ponerColor(0, 0, 0); break;
  }
}

// OLED
const char* textoEstadoMotor() {
  switch (estadoMotor) {
    case 0:  return "NORMAL";
    case 1:  return "ALERTA";
    case 2:  return "ALARMA";
    case 3:  return "RECONOC.";
    default: return "--";
  }
}

void actualizarOled() {
  if (!oledOk) return;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);

  struct tm t;
  if (obtenerHora(t)) oled.printf("%02d/%02d %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1,
                                  t.tm_hour, t.tm_min, t.tm_sec);
  else                oled.print("--/-- --:--:--");
  oled.setCursor(88, 0);
  oled.print(wifiConectado() ? "W" : "-");
  oled.setCursor(98, 0);
  oled.print(sdOk ? "SD" : "--");
  oled.setCursor(116, 0);
  oled.print(appConectada() ? "BT" : "--");

  if (unoVivo()) {
    oled.setCursor(0, 11);
    oled.printf("Carc %.1fC Amb %.1fC", ultimo.t1, ultimo.t2);
    oled.setCursor(0, 21);
    oled.printf("I %.2fA  RPM %.0f", ultimo.irms, ultimo.rpm);
    oled.setCursor(0, 31);
    if (hayVib) oled.printf("Vib %.3fg P %.3fg", ultimaVib.rz, ultimaVib.pico);
    else        oled.print("Vib --");
  } else {
    oled.setCursor(0, 21);
    oled.print("Sin datos del UNO");
  }

  oled.setCursor(0, 42);
  oled.printf("Motor:%s H%.1f", textoEstadoMotor(), horasActuales());
  oled.setCursor(0, 54);
  switch (calcularEstado()) {
    case FALLA:         oled.print("Estado: FALLA KIT"); break;
    case APP_CONECTADA: oled.print("Estado: app conectada"); break;
    case SERVIDOR_OK:   oled.print("Estado: servidor OK"); break;
    case SIN_INTERNET:  oled.print("Estado: guardando SD"); break;
  }
  oled.display();
}

// Procesamiento de tramas del Arduino UNO
void procesarTramaUno(char* contenido) {
  switch (contenido[0]) {
    case 'D': {
      Datos d;
      int n = sscanf(contenido, "D,%f,%f,%f,%f,%f,%f,%f",
                     &d.t1, &d.t2, &d.ax, &d.ay, &d.az, &d.rpm, &d.irms);
      if (n != 7) { tramasMal++; return; }
      ultimo = d;
      hayDatos = true;
      msUltimoDato = millis();
      horometroDato(ultimo.rpm, ultimo.irms);                          // marcha por RPM o corriente
      servidorDato(ultimo.t1, ultimo.t2, ultimo.irms, ultimo.rpm, enMarcha);   // resumen por minuto

      char fh[24], linea[140], archivo[32];
      fechaHora(fh, sizeof(fh));
      snprintf(linea, sizeof(linea), "%lu,%s,%s",
               (unsigned long)epochUtc(), fh, contenido + 2);
      nombreArchivoDia("datos", archivo, sizeof(archivo));
      guardarEnSd(archivo, "ts,fecha_hora,t1,t2,ax,ay,az,rpm,irms", linea);
      registros++;

      Serial1.println(contenido);      // al celular
      break;
    }
    case 'V': {
      Vib v;
      int est = 0;
      int n = sscanf(contenido, "V,%f,%f,%f,%f,%d", &v.rx, &v.ry, &v.rz, &v.pico, &est);
      if (n != 5) { tramasMal++; return; }
      ultimaVib = v;
      hayVib = true;
      estadoMotor = est;
      servidorVib(v.rx, v.ry, v.rz, v.pico);

      char fh[24], linea[140], archivo[32];
      fechaHora(fh, sizeof(fh));
      snprintf(linea, sizeof(linea), "%lu,%s,%s",
               (unsigned long)epochUtc(), fh, contenido + 2);
      nombreArchivoDia("vib", archivo, sizeof(archivo));
      guardarEnSd(archivo, "ts,fecha_hora,rx,ry,rz,pico,est", linea);

      Serial1.println(contenido);      // al celular
      break;
    }
    case 'A': {
      int est = 0;
      char causa[16] = "", detalle[48] = "";
      int n = sscanf(contenido, "A,%d,%15[^,],%47[^\n]", &est, causa, detalle);
      if (n < 2) { tramasMal++; return; }
      estadoMotor = est;
      if (est == 1 || est == 2) {
        char tipo[24];
        snprintf(tipo, sizeof(tipo), "ALARMA_%s", causa);
        registrarEvento(tipo, "uno", detalle);
      } else if (est == 0) {
        registrarEvento("ALARMA_NORMAL", "uno", detalle);
      }
      // est == 3 (reconocida): el evento RECONOCIMIENTO ya lo registra quien reconoció
      Serial1.println(contenido);      // al celular
      break;
    }
    case 'S': {
      int lcd = 0, adxl = 0, temps = 0;
      if (sscanf(contenido, "S,%d,%d,%d", &lcd, &adxl, &temps) == 3) {
        unoAdxl = adxl == 1;
        unoTemps = temps;
        char det[40];
        snprintf(det, sizeof(det), "lcd=%d adxl=%d temps=%d", lcd, adxl, temps);
        registrarEvento("ARRANQUE_UNO", "uno", det);
        enviarAlUno("ACK");
      }
      break;
    }
    case 'E': {
      if (strcmp(contenido, "E,RECON") == 0) {
        registrarEvento("RECONOCIMIENTO", "pulsador", "");
        Serial1.println("E,RECON");
        enviarAlUno("ACK");
      }
      break;
    }
    default:
      tramasMal++;
  }
}

void leerUno() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      bufUno[posUno] = '\0';
      if (posUno > 0) {
        char* contenido;
        if (validarTrama(bufUno, contenido)) procesarTramaUno(contenido);
        else tramasMal++;
      }
      posUno = 0;
    } else if (posUno < sizeof(bufUno) - 1) {
      bufUno[posUno++] = c;
    } else {
      posUno = 0; // si la línea muy larga la descartamos
    }
  }
}

// Comandos desde el celular
void procesarComandoBt(char* cmd) {
  msUltimaApp = millis();
  if (mantProcesarComando(cmd, Serial1, sdOk)) return;   // INFO, MADD, MLIST, HSET

  if (strcmp(cmd, "PING") == 0) {
    Serial1.println("PONG");
  } else if (strcmp(cmd, "RECON") == 0) {
    registrarEvento("RECONOCIMIENTO", "app", "");
    enviarAlUno("C,RECON");
    Serial1.println("OK,RECON");
  } else if (strncmp(cmd, "TSET,", 5) == 0) {   // TSET,AAAA-MM-DD HH:MM:SS (hora local)
    int y, mo, d, h, mi, s;
    if (rtcPresente &&
        sscanf(cmd + 5, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 &&
        y >= 2025 && y <= 2099) {
      DateTime local(y, mo, d, h, mi, s);
      rtc.adjust(DateTime(local.unixtime() - (int32_t)ZONA_HORARIA * 3600));   // guarda en UTC
      sincronizarReloj();
      registrarEvento("AJUSTE_HORA", "app", "");
      Serial1.println("OK,TSET");
    } else {
      Serial1.println("ERROR,TSET");
    }
  } else {
    Serial1.println("ERROR,COMANDO");
  }
}

void leerBluetooth() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      bufBt[posBt] = '\0';
      if (posBt > 0) procesarComandoBt(bufBt);
      posBt = 0;
    } else if (posBt < sizeof(bufBt) - 1) {
      bufBt[posBt++] = c;
    } else {
      posBt = 0; // línea muy larga: a descartar
    }
  }
}

// Hora por NTP
// Cuando llega la hora de internet, corrige el DS3231 y marca el reloj como válido
void revisarNtp() {
  static bool primeraVez = true;
  if (!ntpRecibido) return;
  ntpRecibido = false;
  uint32_t ahora = (uint32_t)time(nullptr);
  if (ahora < EPOCH_MINIMO) return;
  if (rtcPresente) rtc.adjust(DateTime(ahora));
  relojOk = true;
  Serial.println("Hora NTP recibida: RTC actualizado (UTC)");
  if (primeraVez) {
    registrarEvento("SYNC_NTP", "sistema", "");
    primeraVez = false;
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== KIT ESP32 v0.6 ===");

  pinMode(PIN_BT_STATE, INPUT);
  pinMode(PIN_RGB_R, OUTPUT);
  pinMode(PIN_RGB_G, OUTPUT);
  pinMode(PIN_RGB_B, OUTPUT);
  ponerColor(255, 0, 0); delay(300);
  ponerColor(0, 255, 0); delay(300);
  ponerColor(0, 0, 255); delay(300);
  ponerColor(0, 0, 0);

  Serial2.setRxBufferSize(1024);       // no perder tramas del Arduino UNO mientras se envía al servidor
  Serial2.begin(57600, SERIAL_8N1, PIN_UNO_RX, PIN_UNO_TX);
  Serial1.setRxBufferSize(512);        // un MADD completo entra sin perder caracteres
  Serial1.setTxBufferSize(1024);       // las líneas M; largas no bloquean el loop
  Serial1.begin(9600, SERIAL_8N1, PIN_BT_RX, PIN_BT_TX);

  Wire.begin(21, 22);

  // Primero el RTC, sobre el bus ya configurado; después el OLED
  rtcPresente = rtc.begin(&Wire);
  Serial.println(rtcPresente ? "RTC DS3231: OK" : "RTC DS3231: no responde");
  if (rtcPresente && rtc.lostPower()) {
    Serial.println("RTC sin hora: se tomara de internet (NTP) o enviar TSET desde la app");
  }

  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.println(oledOk ? "OLED: OK" : "OLED: no responde");
  if (oledOk) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.print("Kit monitoreo v0.6");
    oled.display();
  }

  sincronizarReloj();

  montarSd();
  mantSetup();                         // carga horas, arranques y último id de la flash
  mantAlGuardar = servidorMantenimiento;
  servidorSetup();                     // WiFi y NTP (no bloquea)
  registrarEvento("ARRANQUE_ESP32", "sistema", "");

  Serial.printf("Horometro: %.1f h, %lu arranques, ultimo id mant: %lu\n",
                horasActuales(), (unsigned long)arranques, (unsigned long)ultIdNvs);
}

// Loop
void loop() {
  static unsigned long msOled  = 0;
  static unsigned long msDebug = 0;
  static unsigned long msSync  = 0;

  leerUno();
  leerBluetooth();
  mantLoop(Serial1, sdOk);             // envío escalonado de MLIST
  revisarNtp();
  servidorLoop(sdOk, tramasMal);  // WiFi, cierre de minuto y envío (bloquea hasta 5 s al enviar)

  // Reintento de montaje de la SD
  if (!sdOk && millis() - msUltimoIntentoSd > REINTENTO_SD_MS) montarSd();

  // Resincronización del reloj interno desde el RTC
  unsigned long intervaloSync = relojOk ? SYNC_OK_MS : SYNC_FALLA_MS;
  if (millis() - msSync >= intervaloSync) {
    msSync = millis();
    sincronizarReloj();
  }

  // Diagnóstico por Monitor Serie
  if (millis() - msDebug >= DEBUG_MS) {
    msDebug = millis();
    char fh[24];
    fechaHora(fh, sizeof(fh));
    Serial.printf("[%s | ts %lu] UNO:%s Reg:%lu Err:%lu SD:%d BT:%d WiFi:%d HTTP:%d "
                  "Pend:%d/%d/%d H:%.3f %s Motor:%s rz:%.3f\n",
                  fh, (unsigned long)epochUtc(), unoVivo() ? "OK" : "sin datos",
                  registros, tramasMal, sdOk, digitalRead(PIN_BT_STATE), wifiConectado(),
                  ultimoCodigoHttp, cantMinPend, cantEvPend, cantMantPend,
                  horasActuales(), enMarcha ? "MARCHA" : "PARADO",
                  textoEstadoMotor(), ultimaVib.rz);
  }

  // Pantalla (actualización)
  if (millis() - msOled >= 500) {
    msOled = millis();
    actualizarOled();
  }

  actualizarRgb();
}
