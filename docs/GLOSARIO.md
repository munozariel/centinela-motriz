# Glosario

## Vibración

| Término | Qué significa |
|---|---|
| **Vibración** | Movimiento de ida y vuelta de la carcasa alrededor de su posición de reposo. Un motor sano vibra siempre de manera parecida; si algo empieza a fallar, la vibración cambia |
| **g** | Unidad de aceleración: 1 g = 9,81 m/s², la aceleración de la gravedad. El ADXL345 mide en g. Para valores chicos se usa mg (milésima de g) |
| **Gravedad (resta)** | El acelerómetro siempre mide 1 g hacia abajo. El kit resta el promedio de cada eje, que es esa gravedad, para quedarse solo con el movimiento del motor |
| **RMS (valor eficaz)** | Raíz del promedio de los cuadrados. Resume en un solo número cuánto vibró el motor en una ventana de tiempo. Es la "vibración general" del kit |
| **Pico** | La mayor sacudida dentro de la ventana de medición |
| **Pico a pico** | La diferencia entre el valor más alto y el más bajo |
| **rx, ry, rz** | Vibración general (RMS) de cada eje, en g. `rz` es el eje radial y es la principal |
| **Eje radial** | Dirección perpendicular al eje del motor. Es donde mejor se ve la vibración de un motor apoyado sobre una base |
| **Eje axial** | Dirección paralela al eje del motor |
| **Frecuencia de muestreo** | Cuántas lecturas por segundo toma el sensor. En el kit, unas 811 |
| **Ventana** | El tiempo durante el que se toman muestras para calcular un valor. En el kit, medio segundo por cada segundo |
| **FIFO** | Memoria interna del ADXL345 donde se acumulan las muestras hasta que el UNO las lee. Así no se pierde ninguna |
| **Montaje rígido** | El sensor tiene que estar firme sobre la carcasa (tornillo y epoxi). Si está flojo, mide su propio movimiento y no el del motor |
| **Línea base** | Los valores del motor medidos cuando está sano. Sirven de referencia para detectar cambios |
| **Puntaje z** | Cuánto se aleja un valor de la línea base, en desvíos estándar. A partir de 3 se considera un cambio |
| **Tendencia** | Cómo evoluciona un valor con las horas de marcha. Permite estimar cuándo llegaría a un umbral |
| **Monitoreo de condición** | Medir el estado de una máquina en funcionamiento para detectar problemas antes de que se conviertan en fallas |

## Mantenimiento

### Tipos

| Código | Tipo | Cuándo se hace |
|---|---|---|
| `PREV` | Preventivo | Por calendario o por horas de marcha, esté o no fallando la máquina |
| `CORR` | Correctivo | Después de una falla, para repararla |
| `PRED` | Predictivo | Porque las mediciones mostraron un cambio que anticipa una falla |
| `INSP` | Inspección | Revisión sin intervención |

### Estado en que queda el motor

| Código | Significado |
|---|---|
| `APTO` | Puede seguir funcionando sin restricciones |
| `OBS` | Apto con observaciones: funciona, pero hay algo para seguir |
| `NOAPTO` | No debe seguir funcionando hasta repararse |

### Tareas

| Código | Tarea |
|---|---|
| `INSP` | Inspección visual general |
| `LIMP` | Limpieza de carcasa, aletas y ventilación |
| `LUB` | Lubricación de rodamientos |
| `ROD` | Cambio de rodamientos |
| `AISL` | Medición de resistencia de aislación con megóhmetro |
| `CONX` | Ajuste de bornera y conexiones |
| `PAT` | Verificación de puesta a tierra |
| `CAP` | Verificación o cambio de capacitor |
| `ALIN` | Verificación de alineación y acople |
| `ANCL` | Verificación de anclajes y base |
| `VENT` | Ventilador y tapa de ventilación |
| `PROT` | Verificación de protecciones (térmico, guardamotor) |
| `CORR` | Medición de corriente con pinza |
| `VIB` | Medición de vibración con instrumento |

En el registro, las tareas se unen con `+`. Por ejemplo, `LIMP+AISL+CONX`.

### Otros términos

| Término | Qué significa |
|---|---|
| **Horómetro** | Contador de horas reales de marcha. Solo suma cuando el motor está girando |
| **Arranque** | Cada vez que el motor pasa de detenido a en marcha. Muchos arranques calientan el bobinado |
| **Intervalo** | Horas de marcha hasta el próximo mantenimiento. Se carga en cada registro |
| **Aislación (MΩ)** | Resistencia entre el bobinado y la carcasa, medida con megóhmetro. Si baja, el aislante se está deteriorando |

## Indicadores

### Semáforo: estado del motor

| Color | Estado | Qué hacer |
|---|---|---|
| Verde | Normal | Nada |
| Amarillo | Alerta: corriente o temperatura sobre el primer umbral durante 10 s. Suena un pitido corto | Revisar la carga y la ventilación |
| Rojo parpadeando + buzzer | Alarma sin reconocer: se superó el segundo umbral | Revisar el motor y reconocer la alarma con el pulsador D5 |
| Rojo fijo | Alarma reconocida: alguien ya la vio, pero la condición sigue | Planificar la intervención |

El kit nunca detiene el motor: el semáforo solo avisa.

### LED RGB: estado del kit

| Color | Estado |
|---|---|
| Rojo parpadeando | Falla del kit: el UNO no responde, falta la SD o falta un sensor |
| Violeta | App conectada por Bluetooth |
| Azul fijo | Conectado al servidor |
| Azul parpadeando | Sin internet: los datos se guardan solo en la SD |

El semáforo indica el estado del **motor** y el LED RGB, el estado del **kit**. Un sensor desconectado es una falla del kit, no del motor.
