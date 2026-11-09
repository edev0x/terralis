# Estación agrometeorológica y controlador de riego por parcela

Firmware para ESP32 (simulable en Wokwi) de una estación agrometeorológica y controlador de riego por parcela.

## Estructura del proyecto

```text
terralis/
├── firmware/                  # código del ESP32 y circuito de Wokwi
│   ├── sketch.ino             # punto de entrada: setup() y loop()
│   ├── *.h                    # lógica, clases de configuración y utilidades
│   ├── diagram.json           # circuito de Wokwi
│   └── libraries.txt          # bibliotecas de Wokwi
├── tests/                     # pruebas unitarias con GoogleTest
```


## Firmware

**Wokwi:** Cree un proyecto ESP32, copie el contenido de `firmware/` y dar inicio a la simulación. Ajuste los potenciómetros de suelo, el botón de lluvia y los sensores HC-SR04 (lámina de agua de unos 35 cm, tanque de unos 60 cm) para
ejercitar los escenarios. 

## Roadmap

### Medición
- [ ] Lluvia con pluviómetro de balancín (0,2 mm por pulso) y antirrebote de 50 ms
- [ ] Temperatura y humedad del aire (DHT22) y presión (BMP180)
- [ ] Humedad de suelo, hoja húmeda y luz
- [ ] Lámina del arrozal y nivel del reservorio (2 × HC-SR04)
- [ ] Reloj con DS1307 y reloj simulado acelerable

### Modelo
- [ ] ET0 de Hargreaves con radiación extraterrestre FAO-56
- [ ] Balance hídrico del suelo con corrección por sensor
- [ ] Kc por cultivo y etapa fenológica
- [ ] Riesgo fúngico en ventana de 48 h (café, frijol, arroz)

### Control
- [ ] Riego de café y frijol: estados T1 a T3
- [ ] Lámina de arroz: estados T4 a T7
- [ ] Una sola línea de bomba, con prioridad al mayor déficit
- [ ] Bloqueos por lluvia, caída de presión, reservorio bajo (con histéresis) y fallas de sensor
- [ ] Salidas por 74HC595 y relés (bomba y válvulas)

### Comunicación e interfaz
- [ ] Registro CSV en tarjeta SD
- [ ] Telemetría y alertas por MQTT
- [ ] Reenvío de filas pendientes desde la SD, con desplazamiento guardado en NVS
- [ ] OLED de 8 páginas, 2 botones y buzzer
- [ ] Cambio de etapa y silencio de alarma desde los botones

### Robustez
- [ ] Vigilante por latidos que cierra las salidas y reinicia
- [ ] Comandos serie de prueba: `x` (colgar control) y `w` (perder red)

### Calidad y herramientas
- [ ] Diagrama de Wokwi validado (pines y redes)
- [ ] Código y mensajes en inglés, con valores de registro en español

## Pendiente

### Validación
- [ ] Compilar el firmware con las bibliotecas reales de ESP32
- [ ] Simular todos los escenarios en Wokwi y registrar los resultados
- [ ] Probar el reenvío desde la SD con una caída de red real

### Calibración
- [ ] Ajustar umbrales y Kc con datos reales de la finca
- [ ] Calibrar sensores de suelo y del pluviómetro
- [ ] Ajustar las horas de riesgo fúngico por variedad

### Código
- [ ] Separar las funciones largas en `.h` y `.cc`
- [ ] Reducir los falsos positivos de `cpplint` o documentarlos en el CI

### Funcionalidades futuras
- [ ] Panel remoto con histórico (suscriptor MQTT)
- [ ] Alertas por mensajería (SMS o chat)
- [ ] Configuración de umbrales sin recompilar
- [ ] Actualización de firmware por red (OTA)
- [ ] Más parcelas o líneas de riego

### Documentación
- [ ] Actualizar el documento 2 a BMP180 y DS1307
- [ ] Incluir el texto de introducción mejorado en el resumen ejecutivo

