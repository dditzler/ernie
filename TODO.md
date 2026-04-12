# Vacation Home Monitor — To-Do List
*Last updated: 2026-04-11*

## 🔗 Project Links
- **Notion tracker (SteadyState Roadmap):** https://www.notion.so/ditz/SteadyState-338e830da7478141a9e1f98b62ab4217
- **Roadmap Tracker DB:** https://www.notion.so/06bda395fb64430d803ae4466fcaa002
- **GitHub repo:** https://github.com/dditzler/vacation-home-temp-monitor
- **Dashboard:** https://dditzler.github.io/ernie/

## ✅ Completed
- Wire up WROOM-32 hub with BMP280 temp sensor
- Wire up XIAO ESP32-C6 with SW420 (paused — switching to ADXL345)
- Establish ESP-NOW link between hub and XIAO
- Build customer-facing dashboard (index.html on GitHub Pages)
- Add outdoor temp + forecast hi/lo from Open-Meteo (ZIP 60631)
- Change T-SIM7000G publish interval to 5 minutes
- Add retain=true to MQTT status + daily publishes (fixes hi/lo on fresh page load)
- Dashboard: grayed-out last reading when sensor is stale

## 🔜 Up Next
- [x] **Remote OTA (WiFi)** — implemented: OTA module checks GitHub version.json, downloads binary via HTTPS, flashes via Update.h
- [ ] **Remote OTA (Cellular)** — wrap TinyGsmClient in SSLClient to enable HTTPS downloads over Hologram. Needs `govorox/SSLClient` library + socket 1 for OTA alongside MQTT on socket 0.
- [ ] **Hologram API: daily data usage** — query usage and include in MQTT status payload
- [ ] **Hologram API: network location** — use as GPS fallback when sats == 0
- [ ] **Hologram API: downlink handler** — receive commands in T-SIM7000G firmware (e.g. trigger OTA, change interval)

## ⏳ Waiting on Hardware
- [ ] **ADXL345 accelerometer** — replace SW420 logic in vibration_xiao_c6 firmware once parts arrive
- [ ] **BME280** — swap out BMP280 on T-SIM7000G for humidity support

## 🔍 To Investigate
- [ ] **GPS zero-satellite issue** — check antenna connection and placement on T-SIM7000G
- [ ] **TLS on cellular MQTT** — implement SSLClient library on cellular path (currently plain TCP port 1883)

## Hardware Reference
- Hub: ESP32 WROOM-32 | SDA=GPIO21, SCL=GPIO22 | port=/dev/cu.usbserial-140
- BMP280: I2C 0x76 (SDO→GND) | chip ID 0x58 (temp+pressure only)
- XIAO ESP32-C6: port=/dev/cu.usbmodem1401 | use BOOT+RESET for bootloader
- Hub MAC: 88:13:BF:04:7D:1C | WiFi Channel 4
- T-SIM7000G: Hologram SIM | MQTT → broker.hivemq.com:1883 (cellular, no TLS yet)
- Dashboard: https://dditzler.github.io/ernie/
- GitHub: https://github.com/dditzler/ernie
