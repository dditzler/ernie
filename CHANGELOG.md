# SteadyState Firmware Changelog

All notable changes to each device's firmware are documented here.
Format: `[version] — date — summary`

---

## hub_wroom32

### [1.0.0] — 2026-04-12
- Initial production firmware
- BMP280 temp/pressure reading (I2C 0x76, confirmed BMP280 not BME280)
- ESP-NOW receiver for VibrationMsg from XIAO C6
- MQTT over TLS to HiveMQ Cloud (port 8883)
- Publishes: vacation/dev/hub/status (realtime), /cycle (HVAC events), /daily (hi/lo summary), /sensor_raw (vibration)
- HVAC cycle tracking with start time and duration
- Daily hi/lo temperature summary at 6 PM Central
- RSSI included in sensor_raw payload
- Firmware version in every status payload (`"fw":"1.0.0"`)
- **OTA via GitHub** — checks hub-version.json hourly, flashes over WiFi

---

## vibration_xiao_c6

### [1.0.0] — 2026-04-12
- Initial production firmware
- ADXL345 vibration detection (I2C 0x53, SDO=GND, CS=3.3V, SDA=D4, SCL=D5)
- Range-based detection: max(axis_max - axis_min) across 5 samples per wake
- Calibrated threshold ON_RANGE_THRESHOLD=12 (idle=2-4, blower=20-64)
- Hysteresis: ON_THRESHOLD=3/5 readings, OFF_THRESHOLD=4/5 readings
- Deep sleep 30s between samples (SLEEP_DURATION_S=30)
- ESP-NOW send to hub MAC 3C:8A:1F:90:D4:30 on channel 6
- External U.FL antenna (GPIO3 LOW, GPIO14 HIGH)
- Heartbeat every 2 wake cycles (~1 min)
- State persists across deep sleep via RTC_DATA_ATTR
- **OTA via GitHub** — checks sensor-c6-version.json every 120 wake cycles (~1hr), connects to WiFi for check

---

## T-SIM7000G (sensor_test)

### [1.1.0] — 2026-04-12
- Fixed OTA repo name (vacation-home-temp-monitor → ernie)
- Fixed SSLClient library reference (registry → GitHub URL)
- Fixed SSLClient API (not a template class — takes Client* pointer)
- OTA now confirmed working on cellular path (SSLClient on TinyGSM socket 1)
- socket 0 = MQTT plain TCP, socket 1 = OTA TLS

### [1.0.0] — 2026-04-07
- Initial production firmware
- BMP280 temp/pressure (I2C 0x76)
- Cellular via Hologram SIM (APN=hologram), MQTT → broker.hivemq.com:1883
- WiFi fallback (WookieBear / airport networks)
- GPS fix with Welford running average for stationary baseline
- Daily hi/lo summary at 6 PM Central
- NTP sync on WiFi path; GPS UTC sync on cellular path
- **OTA via GitHub** — checks sensor-version.json hourly, WiFi and cellular paths

---

## Release workflow (all devices)

1. Bump `FW_VERSION_MAJOR/MINOR/PATCH` and `FW_VERSION_STR` in `src/version.h`
2. Add entry to this CHANGELOG
3. Commit and push
4. Push git tag — GitHub Actions builds binary, publishes Release, updates version manifest

| Device | Tag format | Manifest updated |
|---|---|---|
| hub_wroom32 | `hub-vX.Y.Z` | `hub-version.json` |
| vibration_xiao_c6 | `sensor-c6-vX.Y.Z` | `sensor-c6-version.json` |
| T-SIM7000G | `sensor-vX.Y.Z` | `sensor-version.json` |
