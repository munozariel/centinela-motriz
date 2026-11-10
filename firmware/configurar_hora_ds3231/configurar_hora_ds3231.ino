// Configuración del RTC DS3231 desde el Monitor Serie (ESP32)
// Escribir la HORA LOCAL: AAAA-MM-DD HH:MM:SS (ejemplo: 2026-11-05 04:35:00)
// El RTC guarda UTC (hora local - ZONA_HORARIA).
// Conexión: VCC 3V3, GND, SDA GPIO21, SCL GPIO22. Librería: RTClib (Adafruit).
#include <Wire.h>
#include <RTClib.h>

const int ZONA_HORARIA = -3;       // horas respecto de UTC (Argentina)
RTC_DS3231 rtc;

void mostrarHora() {
  DateTime utc = rtc.now();
  DateTime local(utc.unixtime() + (int32_t)ZONA_HORARIA * 3600);
  Serial.printf("UTC: %04d-%02d-%02d %02d:%02d:%02d  |  Local: %04d-%02d-%02d %02d:%02d:%02d  |  %s\n",
                utc.year(), utc.month(), utc.day(), utc.hour(), utc.minute(), utc.second(),
                local.year(), local.month(), local.day(), local.hour(), local.minute(), local.second(),
                rtc.lostPower() ? "SIN HORA" : "OK");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(21, 22);
  if (!rtc.begin(&Wire)) {
    Serial.println("RTC DS3231 no responde. Revisar conexiones.");
    while (true) delay(1000);
  }
  Serial.println("\nRTC DS3231 OK");
  mostrarHora();
  Serial.println("Escribí la HORA LOCAL como AAAA-MM-DD HH:MM:SS ('Nueva línea').");
}

void loop() {
  if (Serial.available()) {
    String linea = Serial.readStringUntil('\n');
    linea.trim();
    int y, mo, d, h, mi, s;
    if (sscanf(linea.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 &&
        y >= 2025 && y <= 2099) {
      DateTime local(y, mo, d, h, mi, s);
      rtc.adjust(DateTime(local.unixtime() - (int32_t)ZONA_HORARIA * 3600));
      Serial.println("Hora ajustada (guardada en UTC).");
    } else {
      Serial.println("Formato inválido. Ejemplo: 2026-11-05 04:35:00");
    }
  }
  static unsigned long ultimo = 0;
  if (millis() - ultimo >= 2000) {
    ultimo = millis();
    mostrarHora();
  }
}
