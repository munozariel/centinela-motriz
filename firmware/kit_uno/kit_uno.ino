#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

// Pines
const byte PIN_HALL      = 2;
const byte PIN_ONEWIRE   = 4;
const byte PIN_BTN_RECON = 5;
const byte PIN_BTN_PANT  = 6;
const byte PIN_BTN_RES   = 7;
const byte PIN_LED_VERDE = 8;    
const byte PIN_LED_AMAR  = 9;    
const byte PIN_LED_ROJO  = 10;
const byte PIN_BUZZER    = 11;
const byte PIN_ESP_RX    = 12;   // recibe del ESP32 (GPIO17)
const byte PIN_ESP_TX    = 13;   // envía al ESP32 (GPIO16)
const byte PINES_BTN[3]  = {PIN_BTN_RECON, PIN_BTN_PANT, PIN_BTN_RES};

// Constantes
const byte  ADXL_DIR          = 0x53;
const byte  PULSOS_POR_VUELTA = 1;      // un imán: 1 pulso por vuelta
const float G_POR_LSB         = 0.0039; // ADXL345 en resolución completa

// Temperaturas (DS18B20 por dirección)
// Estas direcciones son las de mis sensores de temperatura. Cada DS18B20 tiene la suya:
// buscalas con el ejemplo DallasTemperature > Multiple
const DeviceAddress DIR_CARCASA  = {0x28, 0x4A, 0x41, 0xEB, 0x7C, 0x25, 0x0B, 0x17};  // T1: sonda de acero
const DeviceAddress DIR_AMBIENTE = {0x28, 0xD9, 0x4A, 0xB0, 0xE4, 0x24, 0x02, 0x96};  // T2: ambiente
const float OFFSET_CARCASA  = 0.0;     // °C a sumar (calibración, sin corrección actualmente)
const float OFFSET_AMBIENTE = 0.0;

// Pinza SCT-013
const byte  PIN_PINZA   = A0;
const float VCC         = 5.0;     // 5 V real del UNO medido con tester
const float R_CARGA     = 18.0;    // Ω medidos (21 Ω interna ∥ 110 Ω)
const float RELACION_CT = 2000.0;
const byte  VUELTAS     = 3;       // vueltas del conductor en la pinza
const float VRMS_RUIDO  = 0.0045;  // Vrms con motor apagado
const float CAL         = 1.04;    // ajuste contra pinza comercial (provisorio)
const float I_MINIMA    = 0.2;     // por debajo se informa 0

// Vibración (ADXL345 a 800 Hz con FIFO)
const byte  EJE_RADIAL     = 2;        // 0 = X, 1 = Y, 2 = Z: eje del módulo en dirección radial
const unsigned long VENTANA_VIB_MS = 500;    // ~405 muestras por ventana (el sensor da ~811 por segundo)

// Alarmas
const bool  MODO_DEMO      = false;    // true: umbrales bajos para mostrar la secuencia en la demo
const bool  USAR_DELTA_T   = false;    // false: carcasa absoluta; true: carcasa - ambiente
const float IN_PLACA       = 4.65;     // A, corriente nominal del motor
const float I_ALERTA       = MODO_DEMO ? 2.0  : IN_PLACA * 1.00;
const float I_ALARMA       = MODO_DEMO ? 3.0  : IN_PLACA * 1.15;
const float T_ALERTA       = MODO_DEMO ? 30.0 : (USAR_DELTA_T ? 35.0 : 60.0);
const float T_ALARMA       = MODO_DEMO ? 33.0 : (USAR_DELTA_T ? 50.0 : 75.0);
const byte  SEG_ACTIVAR    = 10;       // s sostenidos para subir de nivel
const byte  SEG_NORMALIZAR = 10;       // s sostenidos para bajar de nivel
const float HISTERESIS     = 0.95;     // para bajar, quedar debajo del 95% del umbral

const char CAUSA_NINGUNA[]   = "-";
const char CAUSA_CORRIENTE[] = "CORRIENTE";
const char CAUSA_TEMP[]      = "TEMP";

// Objetos
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensoresTemp(&oneWire);
LiquidCrystal_I2C* lcd = nullptr;
SoftwareSerial esp(PIN_ESP_RX, PIN_ESP_TX);

// Estado
volatile unsigned long pulsosHall = 0;
volatile unsigned long ultimoPulsoUs = 0;
bool  adxlOk = false;
byte  pantalla = 0;
float vrmsPinza = 0;

char bufEsp[32];
byte posEsp = 0;

bool pulsado[3] = {false, false, false};    // flancos registrados por leerPulsadores()

// Vibración
struct Vibracion {
  float rms[3];     // g RMS por eje
  float media[3];   // g, gravedad media por eje
  float pico;       // g, eje radial
  uint16_t n;       // muestras de la ventana
  uint16_t errores;
};
float mediaEje[3] = {0, 0, 0};    // gravedad de la ventana anterior, en cuentas

// Alarmas
struct Vigilancia { byte nivel; byte segSubir; byte segBajar; };
Vigilancia vigI = {0, 0, 0};
Vigilancia vigT = {0, 0, 0};
byte  estadoSem    = 0;           // 0 normal, 1 alerta, 2 alarma sin reconocer, 3 alarma reconocida
byte  nivelGlobal  = 0;
const char* causaActual = CAUSA_NINGUNA;
char  detalleActual[24] = "verde";
float ultimaI   = 0;
float ultimoVt  = 0;
bool  buzzerSonando = false;

// Comunicación con el ESP32
byte calcularChecksum(const char* s) {
  byte cs = 0;
  while (*s) cs ^= *s++;
  return cs;
}

void enviarTrama(const char* contenido) {
  char trama[80];
  snprintf(trama, sizeof(trama), "$%s*%02X", contenido, calcularChecksum(contenido));
  esp.println(trama);
}

// Datos: $D,t1,t2,ax,ay,az,rpm,irms*CS
void enviarDatos(float t1, float t2, float ax, float ay, float az, float rpm, float irms) {
  char v[7][9];                         // el UNO no formatea float con snprintf
  dtostrf(t1,   1, 1, v[0]);
  dtostrf(t2,   1, 1, v[1]);
  dtostrf(ax,   1, 2, v[2]);
  dtostrf(ay,   1, 2, v[3]);
  dtostrf(az,   1, 2, v[4]);
  dtostrf(rpm,  1, 0, v[5]);
  dtostrf(irms, 1, 2, v[6]);
  char contenido[72];
  snprintf(contenido, sizeof(contenido), "D,%s,%s,%s,%s,%s,%s,%s",
           v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
  enviarTrama(contenido);
}

// Vibración: $V,rx,ry,rz,pico,est*CS  (rz = eje radial)
void enviarVibracion(const Vibracion& r) {
  byte o1 = (EJE_RADIAL == 0) ? 1 : 0;              // los dos ejes no radiales, en orden
  byte o2 = (EJE_RADIAL == 2) ? 1 : 2;
  char v[4][8];
  dtostrf(r.rms[o1],         1, 3, v[0]);
  dtostrf(r.rms[o2],         1, 3, v[1]);
  dtostrf(r.rms[EJE_RADIAL], 1, 3, v[2]);
  dtostrf(r.pico,            1, 3, v[3]);
  char contenido[48];
  snprintf(contenido, sizeof(contenido), "V,%s,%s,%s,%s,%d",
           v[0], v[1], v[2], v[3], estadoSem);
  enviarTrama(contenido);
}

// Alarma: $A,est,causa,detalle*CS
void enviarAlarma() {
  char contenido[48];
  snprintf(contenido, sizeof(contenido), "A,%d,%s,%s", estadoSem, causaActual, detalleActual);
  enviarTrama(contenido);
}

// Valida "$contenido*CS"; si es válida deja en 'contenido' el texto sin $ ni *CS
bool validarTrama(char* l, char*& contenido) {
  if (l[0] != '$') return false;
  char* asterisco = strrchr(l, '*');
  if (!asterisco || strlen(asterisco) < 3) return false;
  *asterisco = '\0';
  byte recibido = (byte)strtol(asterisco + 1, nullptr, 16);
  contenido = l + 1;
  return calcularChecksum(contenido) == recibido;
}

bool reconocerAlarma();    // definida más abajo

void procesarTramaEsp(const char* contenido) {
  if (strcmp(contenido, "ACK") == 0) return;            // confirmación: nada que hacer
  if (strcmp(contenido, "C,RECON") == 0) {
    if (reconocerAlarma()) Serial.println(F(">> Alarma reconocida desde la app"));
    return;
  }
  // Cualquier otra trama válida se ignora
}

// Arma líneas desde el ESP32; procesa solo tramas válidas y descarta el resto en silencio
void leerEsp() {
  while (esp.available()) {
    char c = esp.read();
    if (c == '\r') continue;
    if (c == '\n') {
      bufEsp[posEsp] = '\0';
      char* contenido;
      if (posEsp > 0 && validarTrama(bufEsp, contenido)) procesarTramaEsp(contenido);
      posEsp = 0;
    } else if (posEsp < sizeof(bufEsp) - 1) {
      bufEsp[posEsp++] = c;
    } else {
      posEsp = 0;                       // línea demasiado larga: descartar
    }
  }
}

// Sensor Hall
void isrHall() {
  unsigned long ahora = micros();
  if (ahora - ultimoPulsoUs > 500) {    // filtro de rebotes
    pulsosHall++;
    ultimoPulsoUs = ahora;
  }
}

// Semáforo, buzzer y pulsadores (tareas rápidas)
// Registra los flancos de los pulsadores; la acción se hace en atenderPulsadores()
void leerPulsadores() {
  static bool previo[3] = {HIGH, HIGH, HIGH};
  static unsigned long msCambio[3] = {0, 0, 0};
  for (byte i = 0; i < 3; i++) {
    bool actual = digitalRead(PINES_BTN[i]);
    if (actual != previo[i] && millis() - msCambio[i] > 30) {   // antirrebote
      msCambio[i] = millis();
      previo[i] = actual;
      if (actual == LOW) pulsado[i] = true;
    }
  }
}

// Luces y buzzer según el estado del semáforo
void actualizarSalidas() {
  bool parpadeo = (millis() / 500) % 2 == 0;
  digitalWrite(PIN_LED_VERDE, estadoSem == 0);
  digitalWrite(PIN_LED_AMAR,  estadoSem == 1);
  digitalWrite(PIN_LED_ROJO,  estadoSem == 3 || (estadoSem == 2 && parpadeo));

  bool debeSonar = (estadoSem == 2) && (millis() % 1000 < 300);   // 300 ms cada 1 s
  if (debeSonar && !buzzerSonando) { tone(PIN_BUZZER, 2000); buzzerSonando = true; }
  else if (!debeSonar && buzzerSonando) { noTone(PIN_BUZZER); buzzerSonando = false; }
}

// Se llama también dentro de las capturas largas (vibración y corriente)
void tareasRapidas() {
  leerPulsadores();
  actualizarSalidas();
}

void secuenciaLeds() {                  // prueba del semáforo: verde, amarillo, rojo
  const byte leds[3] = {PIN_LED_VERDE, PIN_LED_AMAR, PIN_LED_ROJO};
  for (byte i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_VERDE, LOW);
    digitalWrite(PIN_LED_AMAR, LOW);
    digitalWrite(PIN_LED_ROJO, LOW);
    digitalWrite(leds[i], HIGH);
    delay(300);
  }
  digitalWrite(PIN_LED_ROJO, LOW);
}

// Alarmas
// Nivel 0/1/2 de una variable, con histéresis y temporización
void evaluar(Vigilancia& v, float valor, float alerta, float alarma) {
  byte crudo;
  if (valor > alarma || (v.nivel == 2 && valor > alarma * HISTERESIS))      crudo = 2;
  else if (valor > alerta || (v.nivel >= 1 && valor > alerta * HISTERESIS)) crudo = 1;
  else                                                                        crudo = 0;

  if (crudo > v.nivel) {
    v.segBajar = 0;
    if (++v.segSubir >= SEG_ACTIVAR) { v.nivel = crudo; v.segSubir = 0; }
  } else if (crudo < v.nivel) {
    v.segSubir = 0;
    if (++v.segBajar >= SEG_NORMALIZAR) { v.nivel = crudo; v.segBajar = 0; }
  } else {
    v.segSubir = 0;
    v.segBajar = 0;
  }
}

const char* causaDeNivel() {
  if (nivelGlobal == 0) return CAUSA_NINGUNA;
  return (vigI.nivel >= vigT.nivel) ? CAUSA_CORRIENTE : CAUSA_TEMP;
}

void armarDetalle() {
  char num[8];
  if (estadoSem == 0)      { strcpy(detalleActual, "verde"); return; }
  if (estadoSem == 3)      { strcpy(detalleActual, "reconocida"); return; }
  const char* color = (estadoSem == 1) ? "amarillo" : "rojo";
  if (causaActual == CAUSA_CORRIENTE) {
    dtostrf(ultimaI, 1, 2, num);
    snprintf(detalleActual, sizeof(detalleActual), "%s I=%s", color, num);
  } else {
    dtostrf(ultimoVt, 1, 1, num);
    snprintf(detalleActual, sizeof(detalleActual), "%s %s=%s", color, USAR_DELTA_T ? "dT" : "T1", num);
  }
}

void cambiarEstado(byte nuevo) {
  estadoSem = nuevo;
  if (nuevo == 0)      causaActual = CAUSA_NINGUNA;
  else if (nuevo != 3) causaActual = causaDeNivel();      // en 3 se conserva la causa
  armarDetalle();
  if (nuevo == 1) tone(PIN_BUZZER, 2000, 150);            // pitido corto de alerta
  enviarAlarma();
  Serial.print(F(">> Semaforo: "));
  Serial.print(nuevo);
  Serial.print(' ');
  Serial.print(causaActual);
  Serial.print(' ');
  Serial.println(detalleActual);
}

// Se llama una vez por segundo
void actualizarAlarmas(float irms, float t1, float t2) {
  ultimaI = irms;
  evaluar(vigI, irms, I_ALERTA, I_ALARMA);

  bool tValida = (t1 != DEVICE_DISCONNECTED_C) &&
                 (!USAR_DELTA_T || t2 != DEVICE_DISCONNECTED_C);
  if (tValida) {
    ultimoVt = USAR_DELTA_T ? t1 - t2 : t1;
    evaluar(vigT, ultimoVt, T_ALERTA, T_ALARMA);
  }
  nivelGlobal = max(vigI.nivel, vigT.nivel);

  switch (estadoSem) {
    case 0:
    case 1: if (nivelGlobal != estadoSem) cambiarEstado(nivelGlobal); break;   // 2 = alarma nueva
    case 2: break;                                                              // retenida
    case 3: if (nivelGlobal < 2) cambiarEstado(nivelGlobal); break;
  }
}

// Reconoce una alarma sin reconocer. Devuelve false si no había
bool reconocerAlarma() {
  if (estadoSem != 2) return false;
  noTone(PIN_BUZZER);
  buzzerSonando = false;
  cambiarEstado(nivelGlobal == 2 ? 3 : nivelGlobal);
  return true;
}

void atenderPulsadores() {
  if (pulsado[0]) {                                // D5: reconocer alarma
    pulsado[0] = false;
    if (reconocerAlarma()) {
      Serial.println(F(">> Alarma reconocida con el pulsador"));
      enviarTrama("E,RECON");
      tone(PIN_BUZZER, 2000, 100);
    } else {
      tone(PIN_BUZZER, 2000, 30);                  // sin alarma activa: solo un pitido cortito
    }
  }
  if (pulsado[1]) {                                // D6: cambia la pantalla del LCD
    pulsado[1] = false;
    pantalla = (pantalla + 1) % 2;
  }
  if (pulsado[2]) {                                // D7: prueba del semáforo
    pulsado[2] = false;
    secuenciaLeds();
  }
}

// I2C
byte escanearI2C() {
  byte dirLcd = 0;
  Serial.println(F("--- Escaneo I2C ---"));
  for (byte dir = 1; dir < 127; dir++) {
    Wire.beginTransmission(dir);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("Dispositivo en 0x"));
      Serial.println(dir, HEX);
      if (dir == 0x27 || dir == 0x3F) dirLcd = dir;
    }
  }
  return dirLcd;
}

// ADXL345
void adxlEscribir(byte reg, byte valor) {
  Wire.beginTransmission(ADXL_DIR);
  Wire.write(reg);
  Wire.write(valor);
  Wire.endTransmission();
}

byte adxlLeerRegistro(byte reg) {
  Wire.beginTransmission(ADXL_DIR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL_DIR, (byte)1);
  return Wire.available() ? Wire.read() : 0;
}

byte adxlEntradasFifo() {
  return adxlLeerRegistro(0x39) & 0x3F;          // FIFO_STATUS
}

// Cada muestra del FIFO se lee con una lectura propia de 6 bytes desde 0x32
bool adxlLeerMuestra(int16_t v[3]) {
  Wire.beginTransmission(ADXL_DIR);
  Wire.write(0x32);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADXL_DIR, (byte)6) != 6) return false;
  for (byte e = 0; e < 3; e++) {
    byte bajo = Wire.read();
    byte alto = Wire.read();
    v[e] = (int16_t)((alto << 8) | bajo);
  }
  return true;
}

void adxlConfigurar() {
  adxlEscribir(0x2D, 0x00);           // standby para configurar
  adxlEscribir(0x31, 0x0B);           // ±16 g, resolución completa
  adxlEscribir(0x2C, 0x0D);           // BW_RATE: 800 Hz
  adxlEscribir(0x38, 0x80);           // FIFO en modo stream
  adxlEscribir(0x2D, 0x08);           // modo medición
}

// Gravedad inicial, para que la primera ventana ya reste bien
void adxlMedirGravedad() {
  Wire.setClock(400000);
  delay(60);
  long suma[3] = {0, 0, 0};
  byte n = 0;
  int16_t v[3];
  for (byte k = adxlEntradasFifo(); k > 0; k--) {
    if (adxlLeerMuestra(v)) {
      for (byte e = 0; e < 3; e++) suma[e] += v[e];
      n++;
    }
    delayMicroseconds(5);
  }
  Wire.setClock(100000);
  if (n) for (byte e = 0; e < 3; e++) mediaEje[e] = (float)suma[e] / n;
}

// Captura de 0,5 s: vibración general (RMS) de cada eje y pico del eje radial
void capturarVibracion(Vibracion& r) {
  float suma[3] = {0, 0, 0}, sumaCuad[3] = {0, 0, 0};
  float minR = 1e9, maxR = -1e9;
  uint16_t n = 0, errores = 0;
  int16_t v[3];

  Wire.setClock(400000);
  for (byte k = adxlEntradasFifo(); k > 0; k--) {   // descarta lo acumulado antes de la ventana
    adxlLeerMuestra(v);
    delayMicroseconds(5);
  }

  unsigned long inicio = millis();
  while (millis() - inicio < VENTANA_VIB_MS) {
    tareasRapidas();
    byte entradas = adxlEntradasFifo();
    for (byte k = 0; k < entradas; k++) {
      if (!adxlLeerMuestra(v)) { errores++; continue; }
      for (byte e = 0; e < 3; e++) {
        float d = v[e] - mediaEje[e];            // se resta la gravedad
        suma[e] += d;
        sumaCuad[e] += d * d;
      }
      float x = v[EJE_RADIAL] - mediaEje[EJE_RADIAL];
      if (x < minR) minR = x;
      if (x > maxR) maxR = x;
      n++;
      delayMicroseconds(5);                      // pausa mínima entre lecturas del FIFO
    }
  }
  Wire.setClock(100000);                         // el LCD es de 100 kHz

  r.n = n;
  r.errores = errores;
  if (n < 50) {                                  // ventana inválida
    for (byte e = 0; e < 3; e++) { r.rms[e] = 0; r.media[e] = mediaEje[e] * G_POR_LSB; }
    r.pico = 0;
    return;
  }

  // RMS por eje y actualización de la gravedad para la ventana siguiente
  float mediaR = 0;
  for (byte e = 0; e < 3; e++) {
    float m = suma[e] / n;
    float var = sumaCuad[e] / n - m * m;
    if (var < 0) var = 0;
    r.rms[e] = sqrt(var) * G_POR_LSB;
    mediaEje[e] += m;
    r.media[e] = mediaEje[e] * G_POR_LSB;
    if (e == EJE_RADIAL) mediaR = m;
  }
  r.pico = max(maxR - mediaR, mediaR - minR) * G_POR_LSB;
}

// Temperaturas
float leerTemp(const uint8_t* dir, float offset) {
  float t = sensoresTemp.getTempC(dir);
  return (t == DEVICE_DISCONNECTED_C) ? t : t + offset;
}

// Pinza SCT-013
// Bloquea 200 ms (10 ciclos de 50 Hz). Atiende semáforo y pulsadores mientras mide
float leerCorriente() {
  const unsigned long VENTANA_MS = 200;
  long suma = 0, sumaCuad = 0, n = 0;
  unsigned long inicio = millis();
  while (millis() - inicio < VENTANA_MS) {
    int x = analogRead(PIN_PINZA) - 512;
    suma += x;
    sumaCuad += (long)x * x;
    n++;
    if ((n & 31) == 0) tareasRapidas();
  }
  float media = (float)suma / n;              // punto medio real, no 512 fijo
  float var = (float)sumaCuad / n - media * media;
  if (var < 0) var = 0;
  vrmsPinza = sqrt(var) * VCC / 1024.0;

  float v2 = vrmsPinza * vrmsPinza - VRMS_RUIDO * VRMS_RUIDO;
  if (v2 < 0) v2 = 0;
  float i = sqrt(v2) * RELACION_CT / (R_CARGA * VUELTAS) * CAL;
  return (i < I_MINIMA) ? 0.0 : i;
}

// LCD
void actualizarLcd(float t1, float irms, float rpm, const Vibracion& vib) {
  if (!lcd) return;
  lcd->clear();
  if (pantalla == 0) {
    lcd->print(F("T1:")); lcd->print(t1, 1);
    lcd->print(F(" I:")); lcd->print(irms, 2);
    lcd->setCursor(0, 1);
    switch (estadoSem) {
      case 0:
        lcd->print(F("RPM:")); lcd->print(rpm, 0);
        lcd->print(F(" NORMAL"));
        break;
      case 1:
        lcd->print(F("ALERTA "));
        lcd->print(causaActual);
        break;
      case 2:
        lcd->print(F("ALARMA "));
        lcd->print(causaActual);
        break;
      case 3:
        lcd->print(F("ALARMA RECONOC."));
        break;
    }
  } else {
    // Vibración en g, con los mismos nombres que la trama V (rz = eje radial)
    byte o1 = (EJE_RADIAL == 0) ? 1 : 0;
    byte o2 = (EJE_RADIAL == 2) ? 1 : 2;
    lcd->print(F("rx")); lcd->print(vib.rms[o1], 3);
    lcd->print(F(" ry")); lcd->print(vib.rms[o2], 3);
    lcd->setCursor(0, 1);
    lcd->print(F("rz")); lcd->print(vib.rms[EJE_RADIAL], 3);
    lcd->print(F(" p")); lcd->print(vib.pico, 3);
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  esp.begin(57600);
  Serial.println(F("\n=== KIT UNO v0.6 ==="));

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMAR, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  for (byte i = 0; i < 3; i++) pinMode(PINES_BTN[i], INPUT_PULLUP);
  pinMode(PIN_HALL, INPUT);             // pull-up externo de 10k
  attachInterrupt(digitalPinToInterrupt(PIN_HALL), isrHall, FALLING);

  Serial.println(F("Prueba del semaforo y buzzer..."));
  secuenciaLeds();
  tone(PIN_BUZZER, 2000, 200);

  Wire.begin();
  Wire.setClock(100000);
  byte dirLcd = escanearI2C();
  if (dirLcd) {
    lcd = new LiquidCrystal_I2C(dirLcd, 16, 2);
    lcd->init();
    lcd->backlight();
    lcd->print(F("Kit monitoreo"));
    Serial.println(F("LCD: OK"));
  } else {
    Serial.println(F("LCD: NO encontrado (0x27 / 0x3F)"));
  }

  byte idAdxl = adxlLeerRegistro(0x00);
  adxlOk = (idAdxl == 0xE5);
  Serial.print(F("ADXL345 ID: 0x"));
  Serial.print(idAdxl, HEX);
  Serial.println(adxlOk ? F(" -> OK") : F(" -> FALLA (esperado 0xE5)"));
  if (adxlOk) {
    adxlConfigurar();
    adxlMedirGravedad();
    Serial.print(F("BW_RATE: 0x"));
    Serial.println(adxlLeerRegistro(0x2C), HEX);   // esperado D (800 Hz)
  }

  sensoresTemp.begin();
  sensoresTemp.setWaitForConversion(false);
  bool hayCarcasa  = sensoresTemp.isConnected(DIR_CARCASA);
  bool hayAmbiente = sensoresTemp.isConnected(DIR_AMBIENTE);
  Serial.print(F("Carcasa (T1): "));  Serial.println(hayCarcasa  ? F("OK") : F("NO encontrado"));
  Serial.print(F("Ambiente (T2): ")); Serial.println(hayAmbiente ? F("OK") : F("NO encontrado"));
  byte nTemp = (hayCarcasa ? 1 : 0) + (hayAmbiente ? 1 : 0);
  sensoresTemp.requestTemperatures();

  if (MODO_DEMO) Serial.println(F("*** MODO DEMO: umbrales de alarma bajos ***"));

  // Estado de arranque al ESP32: $S,lcd,adxl,nTemp*CS, y estado inicial del semáforo
  char estado[24];
  snprintf(estado, sizeof(estado), "S,%d,%d,%d", dirLcd ? 1 : 0, adxlOk ? 1 : 0, nTemp);
  enviarTrama(estado);
  enviarAlarma();

  Serial.println(F("=== Lecturas cada 1 s ==="));
  delay(1000);
}

// Loop
void loop() {
  static unsigned long ultimo = 0;
  tareasRapidas();
  atenderPulsadores();
  leerEsp();

  if (millis() - ultimo < 1000) return;
  unsigned long ahora = millis();
  unsigned long dt = ahora - ultimo;
  ultimo = ahora;

  // RPM del Hall (pulsos contados por interrupción durante dt)
  noInterrupts();
  unsigned long pulsos = pulsosHall;
  pulsosHall = 0;
  interrupts();
  float rpm = pulsos * 60000.0 / (dt * PULSOS_POR_VUELTA);

  // Vibración (0,5 s)
  Vibracion vib;
  if (adxlOk) capturarVibracion(vib);
  else        memset(&vib, 0, sizeof(vib));

  // Corriente (0,2 s)
  float irms = leerCorriente();

  // Temperaturas por dirección (la conversión se pidió en la vuelta anterior)
  float t1 = leerTemp(DIR_CARCASA,  OFFSET_CARCASA);    // carcasa
  float t2 = leerTemp(DIR_AMBIENTE, OFFSET_AMBIENTE);   // ambiente
  sensoresTemp.requestTemperatures();

  // Alarmas (puede mandar una trama A)
  actualizarAlarmas(irms, t1, t2);

  // ---- Envío al ESP32 ----
  enviarDatos(t1, t2, vib.media[0], vib.media[1], vib.media[2], rpm, irms);
  if (adxlOk) enviarVibracion(vib);

  // ---- Monitor Serie ----
  Serial.print(F("Carc:"));
  if (t1 == DEVICE_DISCONNECTED_C) Serial.print(F("--")); else Serial.print(t1, 1);
  Serial.print(F(" Amb:"));
  if (t2 == DEVICE_DISCONNECTED_C) Serial.print(F("--")); else Serial.print(t2, 1);
  Serial.print(F(" | I:"));   Serial.print(irms, 2);
  Serial.print(F(" Vrms:"));  Serial.print(vrmsPinza, 4);
  Serial.print(F(" | RPM:")); Serial.print(rpm, 0);
  Serial.print(F(" | rz:"));  Serial.print(vib.rms[EJE_RADIAL], 3);
  Serial.print(F(" p:"));     Serial.print(vib.pico, 3);
  Serial.print(F(" | n:"));   Serial.print(vib.n);
  Serial.print(F(" err:"));   Serial.print(vib.errores);
  Serial.print(F(" | Sem:")); Serial.println(estadoSem);

  // ---- LCD ----
  actualizarLcd(t1, irms, rpm, vib);
}
