# Vacation Home Sensor System — Wiring Brief for Infographic
*For Nano Banana — Infographic Reference*

---

## System Overview

Two wireless nodes communicate over **ESP-NOW** (peer-to-peer Wi-Fi, no router needed):

- **Hub** — ESP32 WROOM-32 — reads indoor temperature and relays data to the cloud over MQTT
- **Vibration Node** — Seeed XIAO ESP32-C6 — detects HVAC vibration via accelerometer and reports to the Hub

---

## Node 1: Hub (ESP32 WROOM-32)

**Purpose:** Reads indoor temperature from a BMP280 sensor and publishes to MQTT over Wi-Fi. Also receives vibration reports from the XIAO node via ESP-NOW.

### BMP280 Temperature Sensor → WROOM-32 (I2C)

| BMP280 Pin | WROOM-32 Pin | Notes |
|------------|--------------|-------|
| VCC | 3.3V | |
| GND | GND | |
| SDA | GPIO 21 | I2C Data |
| SCL | GPIO 22 | I2C Clock |
| SDO | GND | Sets I2C address to 0x76 |
| CSB | 3.3V | Selects I2C mode |

**Power:** USB or 5V barrel jack into WROOM-32 `VIN`

---

## Node 2: Vibration Node (Seeed XIAO ESP32-C6)

**Purpose:** Mounts on the HVAC unit. Detects whether the furnace/AC is running by sensing vibration via an ADXL345 accelerometer. Wakes from deep sleep every 30 seconds, samples, and sends an ESP-NOW packet to the Hub.

### ADXL345 Accelerometer → XIAO ESP32-C6 (I2C)

| ADXL345 Pin | XIAO Pin | Notes |
|-------------|----------|-------|
| VCC / VS | 3.3V | |
| GND | GND | |
| SDA | D4 (GPIO 6) | I2C Data |
| SCL | D5 (GPIO 7) | I2C Clock |
| SDO | GND | Sets I2C address to 0x53 |
| CS | 3.3V | Selects I2C mode |
| INT1 | D1 (GPIO 3) | Optional — activity interrupt |

**Power:** Small USB power bank or USB-C wall adapter → XIAO USB-C port

---

## Wireless Link: ESP-NOW

- **Protocol:** ESP-NOW (Espressif peer-to-peer, 2.4 GHz)
- **No Wi-Fi router required** between the two nodes
- Hub MAC Address: `88:13:BF:04:7D:1C`
- Wi-Fi Channel: 4
- The XIAO sends a small packet every 30 seconds: HVAC on/off status + duration

---

## Data Flow Diagram (for infographic)

```
[ADXL345]
    │  I2C
    ▼
[XIAO ESP32-C6]  ──── ESP-NOW (2.4GHz) ────▶  [WROOM-32 Hub]  ──── Wi-Fi/MQTT ────▶  ☁ Cloud
                                                      ▲
                                                 [BMP280]
                                                  I2C │
                                              (temp sensor)
```

---

## Key Callouts for the Infographic

- The XIAO runs on **deep sleep** between readings to save power — can run weeks on a battery pack
- The BMP280 measures **temperature only** (not humidity) — chip ID 0x58
- ESP-NOW works even if the home Wi-Fi goes down — the two devices talk directly
- The ADXL345 detects the subtle **vibration signature** of the furnace blower motor or compressor
- The Hub is the only device that needs a Wi-Fi password

---

*Hardware versions: WROOM-32 rev1, XIAO ESP32-C6, BMP280 breakout (I2C), ADXL345 breakout (I2C)*
