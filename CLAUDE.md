This is an ESP32-based vacation home temperature and vibration monitor (SteadyState). Multiple hardware variants with separate firmware folders.

**Hardware variants:**
- hub_wroom32 — ESP32 WROOM-32U hub, BMP280 temp sensor (I2C 0x76, chip ID 0x58 = BMP280 not BME280), SDA=GPIO21, SCL=GPIO22, port=/dev/cu.usbserial-0001, MAC: 3C:8A:1F:90:D4:30, WiFi Channel 6 (WookieBear)
- vibration_xiao_c6 — XIAO ESP32-C6 vibration sensor, ADXL345 (I2C 0x53, SDO=GND, CS=3.3V), SDA=D4(GPIO22), SCL=D5(GPIO23), port=/dev/cu.usbmodem1301, external U.FL antenna (GPIO3 LOW, GPIO14 HIGH), deployed on furnace
- T-SIM7000G — cellular node, Hologram SIM (APN=hologram), MQTT → broker.hivemq.com:1883 (plain TCP, no TLS yet), port=/dev/cu.usbserial-59680320951, 5-min publish interval, fw=1.1.0
- ernie-eink — e-ink display board (inactive)
- ADXL345_ESP32-C3 Super Mini — future vibration board variant (waiting on U.FL units)

**Firmware versions (as of 2026-04-12):**
- hub_wroom32: fw=1.0.0
- vibration_xiao_c6: fw=1.0.0
- T-SIM7000G: fw=1.1.0

**OTA — fully implemented and live:**
- Hub: WiFiClientSecure, checks hourly via otaLoop() in main loop
- C6: WiFi association on wake, checks every 120 cycles (~1hr), tracked in RTC_DATA_ATTR lastOtaCycle
- T-SIM7000G: SSLClient over TinyGsmClient socket 1, socket 0 = MQTT, checks hourly
- Manifests: hub-version.json, sensor-c6-version.json, sensor-version.json (all in repo root, currently 0.0.0 placeholders)
- GitHub Actions: release.yml triggers on hub-v*, sensor-c6-v*, sensor-v* tags → builds binary → publishes Release → updates manifest
- DEPLOY_TOKEN set in GitHub repo secrets
- To release: bump version.h, commit, git tag <device>-vX.Y.Z && git push origin <device>-vX.Y.Z

**Vibration calibration (ADXL345, calibrated 2026-04-12):**
- idle: range 2-4 counts
- ignition transient: 5-8
- blower ramp: 10-18
- full blower run: 20-64 (peak 64)
- ON_RANGE_THRESHOLD = 12 (between noise floor and blower onset)
- HEARTBEAT_CYCLES = 2 (one heartbeat per minute at 30s sleep)
- SLEEP_DURATION_S = 30

**ESP-NOW link:**
- Hub MAC: 3C:8A:1F:90:D4:30, channel 6
- RSSI confirmed -55 to -75 dBm throughout property (basement, garage, furnace room)
- VibrationMsg struct: hvacOn, reason, onDurationS, rawMagnitude (must match exactly hub ↔ C6)

**Active TODOs:**
- Provisioning/onboarding: QR code → captive portal WiFi setup → cloud account link → C6 pairing (two separate phases per Notion notes)
- Interrupt-driven wakeup for C6: ADXL345 INT1/INT2 pins for instant furnace detection (pre-V1)
- TLS on cellular MQTT (SSLClient wrapping MQTT broker connection)
- Hologram API: daily data usage, network location, downlink handler
- BME280 swap on T-SIM7000G for humidity
- GPS zero-satellite issue — antenna check on T-SIM7000G
- Fair C3 vs C6 antenna range test (waiting on C3 Super Minis with U.FL connector)
- 3D printed mounting boards with serial numbers

**WiFi credentials (in secrets.h, gitignored):**
- WookieBear / 7737443630 (home)
- airport / HitekPrinting (vacation home)

**MQTT (HiveMQ Cloud TLS):**
- Host: ad21434501c84aad995bc5621bf77f15.s1.eu.hivemq.cloud:8883
- Topics: vacation/dev/hub/status, /cycle, /daily, /sensor_raw
- User: vacation-sensor

**Key links:**
- GitHub: https://github.com/dditzler/ernie
- Dashboard: https://dditzler.github.io/ernie/
- Notion roadmap: https://www.notion.so/ditz/SteadyState-338e830da7478141a9e1f98b62ab4217
- Notion quick ref: https://www.notion.so/340e830da74781358c3fcdde811b819c

**Stack:** PlatformIO, Arduino framework, ESP-NOW, MQTT over TLS (WiFi) / plain TCP (cellular), OTA via GitHub raw + ArduinoHttpClient + Update.h, SSLClient (govorox) for cellular TLS
