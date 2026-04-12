This is an ESP32-based vacation home temperature and vibration monitor. The project has multiple hardware variants with separate firmware. Most active firmware is in the "sensor_test" folder (src/, boards/, lib/, platformio.ini).

**Hardware variants:**
- hub_wroom32 — ESP32 WROOM-32 hub, BMP280 temp sensor (I2C 0x76), SDA=GPIO21, SCL=GPIO22, port=/dev/cu.usbserial-140, MAC: 88:13:BF:04:7D:1C, WiFi Channel 4
- vibration_xiao_c6 — XIAO ESP32-C6 vibration sensor, currently SW420, switching to ADXL345 (parts pending), port=/dev/cu.usbmodem1401
- ernie-eink — e-ink display board
- T-SIM7000G — cellular node, Hologram SIM, MQTT → broker.hivemq.com:1883 (no TLS yet), 5-min publish interval
- ADXL345_ESP32-C3 Super Mini — new vibration board variant

**Active TODOs:**
- Remote OTA (Cellular) — SSLClient over TinyGsmClient, socket 1 for OTA + socket 0 for MQTT
- Hologram API: daily data usage, network location (GPS fallback), downlink handler
- ADXL345 accelerometer swap in vibration_xiao_c6 (waiting on hardware)
- BME280 swap on T-SIM7000G for humidity
- GPS zero-satellite issue — antenna check
- TLS on cellular MQTT (SSLClient)

**Key links:**
- GitHub: https://github.com/dditzler/ernie
- Dashboard: https://dditzler.github.io/ernie/
- Notion roadmap: https://www.notion.so/ditz/SteadyState-338e830da7478141a9e1f98b62ab4217
- TODO.md and vacation-home-temp-monitor.md are in the project root

**Stack:** PlatformIO, Arduino framework, ESP-NOW, MQTT, OTA via GitHub version.json + Update.h