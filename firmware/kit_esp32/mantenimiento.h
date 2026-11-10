// mantenimiento.h — Kit ESP32 v0.6
// Horómetro (horas de marcha y arranques) y registro de mantenimientos en la SD.
// Comandos Bluetooth: INFO, HSET,<horas>, MLIST[,n], MADD;...
// - La marcha se detecta por RPM o por corriente.
// - El último id de mantenimiento se guarda en la flash (ids únicos aunque cambie la SD)
//   y cada mantenimiento guardado se avisa al módulo del servidor (mantAlGuardar).

#pragma once
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>

// ---------- Configuración ----------
const float         RPM_MARCHA        = 300.0;  // por encima, el motor está en marcha
const float         I_MARCHA          = 0.5;    // A: por encima, el motor está en marcha (en vacío ~2,3 A)
const byte          TRAMAS_CONFIRMAR  = 3;      // tramas seguidas para confirmar arranque o parada
const uint32_t      GUARDAR_CADA_S    = 300;    // guarda el horómetro cada 5 min de marcha
const unsigned long HUECO_MAX_MS      = 3000;   // si no llegaron datos en este lapso, no suma tiempo
const unsigned long MLIST_PASO_MS     = 250;    // una línea de historial cada 250 ms (9600 baudios)
const int           MLIST_DEFECTO     = 20;
const int           MLIST_MAXIMO      = 50;
const char*         ARCH_MANT         = "/mantenimientos.csv";
const char*         ENCABEZADO_MANT   = "id;fecha;horas;tipo;responsable;tareas;aislacion_mohm;"
                                        "corriente_a;repuestos;estado;intervalo_h;observaciones";

// ---------- Estado ----------
Preferences   prefsHoro;
uint32_t      segMarcha        = 0;     // segundos de marcha acumulados
uint32_t      arranques        = 0;
uint32_t      segSinGuardar    = 0;
uint32_t      msAcumulados     = 0;
bool          enMarcha         = false;
byte          tramasDistintas  = 0;
unsigned long msUltimoDatoHoro = 0;

bool          mantEscaneado    = false;
uint32_t      cantMant         = 0;
uint32_t      proximoId        = 1;
uint32_t      ultIdNvs         = 0;     // último id guardado en la flash
char          ultFecha[20]     = "-";
float         ultHoras         = -1;
float         ultIntervalo     = -1;

File          archMlist;
bool          mlistActivo      = false;
unsigned long msMlist          = 0;
int           mlistEnviados    = 0;

// Se llama con la línea completa cada vez que se guarda un mantenimiento
void (*mantAlGuardar)(const char* lineaSd) = nullptr;

// ---------- Utilidades ----------
float horasActuales() { return segMarcha / 3600.0; }

void guardarHorometro() {
  prefsHoro.putUInt("seg", segMarcha);
  prefsHoro.putUInt("arr", arranques);
  segSinGuardar = 0;
}

// Copia el campo n (desde 0) de una línea separada por ';'
bool mantCampo(const char* linea, int n, char* dest, size_t tam) {
  const char* p = linea;
  for (int i = 0; i < n; i++) {
    p = strchr(p, ';');
    if (!p) { dest[0] = '\0'; return false; }
    p++;
  }
  size_t len = strcspn(p, ";\r\n");
  if (len >= tam) len = tam - 1;
  memcpy(dest, p, len);
  dest[len] = '\0';
  return true;
}

int mantContarCampos(const char* l) {
  int n = 1;
  while (*l) if (*l++ == ';') n++;
  return n;
}

int mantLeerLinea(File& f, char* buf, size_t tam) {
  int n = f.readBytesUntil('\n', buf, tam - 1);
  buf[n] = '\0';
  if (n > 0 && buf[n - 1] == '\r') buf[--n] = '\0';
  return n;
}

// Recorre el archivo: cuenta registros, próximo id y datos del último mantenimiento.
// El próximo id es el mayor entre la SD y la flash.
void escanearMant() {
  mantEscaneado = true;
  cantMant = 0;
  proximoId = ultIdNvs + 1;
  if (!SD.exists(ARCH_MANT)) return;
  File f = SD.open(ARCH_MANT, FILE_READ);
  if (!f) return;
  char l[400], c[24];
  mantLeerLinea(f, l, sizeof(l));                       // encabezado
  while (f.available()) {
    if (mantLeerLinea(f, l, sizeof(l)) == 0) continue;
    cantMant++;
    if (mantCampo(l, 0, c, sizeof(c))) {
      uint32_t id = atol(c);
      if (id >= proximoId) proximoId = id + 1;
    }
    mantCampo(l, 1, ultFecha, sizeof(ultFecha));
    ultHoras     = mantCampo(l, 2, c, sizeof(c))  ? atof(c) : -1;
    ultIntervalo = mantCampo(l, 10, c, sizeof(c)) ? atof(c) : -1;
  }
  f.close();
}

// ---------- Horómetro ----------
// Llamar cada vez que llega una trama D válida del UNO
void horometroDato(float rpm, float irms) {
  unsigned long ahora = millis();
  unsigned long dt = ahora - msUltimoDatoHoro;
  msUltimoDatoHoro = ahora;

  bool lectura = (rpm > RPM_MARCHA) || (irms > I_MARCHA);
  if (lectura != enMarcha) {
    if (++tramasDistintas >= TRAMAS_CONFIRMAR) {
      enMarcha = lectura;
      tramasDistintas = 0;
      if (enMarcha) arranques++;
      guardarHorometro();
    }
  } else {
    tramasDistintas = 0;
  }

  if (enMarcha && dt < HUECO_MAX_MS) {
    msAcumulados += dt;
    while (msAcumulados >= 1000) {
      msAcumulados -= 1000;
      segMarcha++;
      segSinGuardar++;
    }
    if (segSinGuardar >= GUARDAR_CADA_S) guardarHorometro();
  }
}

// Comandos
void enviarInfo(HardwareSerial& bt) {
  float prox = (ultIntervalo > 0 && ultHoras >= 0) ? ultHoras + ultIntervalo : -1;
  bt.printf("I,%.1f,%lu,%d,%lu,%s,%.1f,%.1f\n",
            horasActuales(), (unsigned long)arranques, enMarcha ? 1 : 0,
            (unsigned long)cantMant, ultFecha, ultHoras, prox);
}

bool tipoValido(const char* t) {
  return !strcmp(t, "PREV") || !strcmp(t, "CORR") || !strcmp(t, "PRED") || !strcmp(t, "INSP");
}

bool estadoValido(const char* e) {
  return !strcmp(e, "APTO") || !strcmp(e, "OBS") || !strcmp(e, "NOAPTO");
}

// MADD;fecha;tipo;responsable;tareas;aisl;corriente;repuestos;estado;intervalo;obs
void comandoMadd(char* l, HardwareSerial& bt, bool sdOk) {
  if (!sdOk) { bt.print("ERROR,MADD,SD\n"); return; }
  if (mantContarCampos(l) != 11) { bt.print("ERROR,MADD,CAMPOS\n"); return; }

  char fecha[20], tipo[8], resp[40], estado[8], c[24];
  mantCampo(l, 1, fecha, sizeof(fecha));
  mantCampo(l, 2, tipo, sizeof(tipo));
  mantCampo(l, 3, resp, sizeof(resp));
  mantCampo(l, 8, estado, sizeof(estado));
  if (strlen(fecha) < 10 || !tipoValido(tipo) || !estadoValido(estado) || resp[0] == '\0') {
    bt.print("ERROR,MADD,DATOS\n");
    return;
  }

  if (!mantEscaneado) escanearMant();
  bool nuevo = !SD.exists(ARCH_MANT);
  File f = SD.open(ARCH_MANT, FILE_APPEND);
  if (!f) { bt.print("ERROR,MADD,ARCHIVO\n"); return; }
  if (nuevo) f.printf("%s\n", ENCABEZADO_MANT);

  const char* resto = strchr(l + 5, ';') + 1;   // desde "tipo" hasta el final
  uint32_t id = proximoId;
  float horas = horasActuales();
  char lineaSd[420];
  snprintf(lineaSd, sizeof(lineaSd), "%lu;%s;%.1f;%s", (unsigned long)id, fecha, horas, resto);
  f.printf("%s\n", lineaSd);
  f.close();

  prefsHoro.putUInt("ultId", id);               // el id no se reutiliza aunque cambie la SD
  ultIdNvs = id;
  proximoId++;
  cantMant++;
  strlcpy(ultFecha, fecha, sizeof(ultFecha));
  ultHoras = roundf(horas * 10) / 10.0;
  ultIntervalo = mantCampo(l, 9, c, sizeof(c)) ? atof(c) : -1;

  if (mantAlGuardar) mantAlGuardar(lineaSd);    // al servidor
  bt.printf("OK,MADD,%lu\n", (unsigned long)id);
}

// MLIST o MLIST,n: prepara el envío de los últimos n registros
void comandoMlist(char* l, HardwareSerial& bt, bool sdOk) {
  if (mlistActivo) { bt.print("ERROR,MLIST,OCUPADO\n"); return; }
  if (!sdOk) { bt.print("ERROR,MLIST,SD\n"); return; }
  int n = MLIST_DEFECTO;
  if (l[5] == ',') n = constrain(atoi(l + 6), 1, MLIST_MAXIMO);

  if (!mantEscaneado) escanearMant();
  archMlist = SD.open(ARCH_MANT, FILE_READ);
  if (!archMlist) { bt.print("MFIN,0\n"); return; }

  char tmp[400];
  mantLeerLinea(archMlist, tmp, sizeof(tmp));           // encabezado
  long saltear = (long)cantMant - n;
  while (saltear-- > 0 && archMlist.available()) mantLeerLinea(archMlist, tmp, sizeof(tmp));

  mlistEnviados = 0;
  msMlist = 0;
  mlistActivo = true;
}

// Devuelve true si el comando era de este módulo
bool mantProcesarComando(char* l, HardwareSerial& bt, bool sdOk) {
  size_t n = strlen(l);
  while (n && (l[n - 1] == '\r' || l[n - 1] == ' ')) l[--n] = '\0';

  if (!strcmp(l, "INFO")) {
    if (!mantEscaneado && sdOk) escanearMant();
    enviarInfo(bt);
    return true;
  }
  if (!strncmp(l, "MLIST", 5) && (l[5] == '\0' || l[5] == ',')) {
    comandoMlist(l, bt, sdOk);
    return true;
  }
  if (!strncmp(l, "MADD;", 5)) {
    comandoMadd(l, bt, sdOk);
    return true;
  }
  if (!strncmp(l, "HSET,", 5)) {
    float h = atof(l + 5);
    if (h < 0 || h > 200000) {
      bt.print("ERROR,HSET\n");
    } else {
      segMarcha = (uint32_t)(h * 3600);
      guardarHorometro();
      bt.print("OK,HSET\n");
      enviarInfo(bt);
    }
    return true;
  }
  return false;
}

// ---------- setup y loop ----------
void mantSetup() {
  prefsHoro.begin("horometro", false);
  segMarcha = prefsHoro.getUInt("seg", 0);
  arranques = prefsHoro.getUInt("arr", 0);
  ultIdNvs  = prefsHoro.getUInt("ultId", 0);
}

void mantLoop(HardwareSerial& bt, bool sdOk) {
  if (!mantEscaneado && sdOk) escanearMant();

  if (mlistActivo && millis() - msMlist >= MLIST_PASO_MS) {
    msMlist = millis();
    char l[400];
    while (archMlist.available()) {
      if (mantLeerLinea(archMlist, l, sizeof(l)) == 0) continue;
      bt.print("M;");
      bt.print(l);
      bt.print('\n');
      mlistEnviados++;
      return;                       // una línea por vez
    }
    archMlist.close();
    mlistActivo = false;
    bt.printf("MFIN,%d\n", mlistEnviados);
  }
}
