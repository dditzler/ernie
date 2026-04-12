# Vacation Home Temperature Monitor
**ESP32 Project Notes** — March 2026

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| Microcontroller | Seeed Studio XIAO ESP32-C6 | Built-in LiPo charger, ~7µA deep sleep, WiFi 6 |
| Temperature Sensor | DS18B20 on 1m lead (waterproof probe) | 1-Wire, ±0.5°C accuracy, in-hand |
| Pull-up Resistor | 10kΩ (in-hand) | Between 3V3 and Data pin — 4.7kΩ ideal but 10kΩ works at 1m |
| Battery | LiPo 750mAh 1S 3.7V (in-hand) | ~30–35 days normal mode, ~8–10 days alert mode |

### Wiring
```
3V3  → DS18B20 VCC + one end of 10kΩ resistor
GND  → DS18B20 GND
D2   → DS18B20 Data + other end of 10kΩ resistor
```

### Battery Notes
- Board has no JST connector — wiring directly to **B+** and **B-** pads on underside of board
- **Check polarity with multimeter before soldering** — reversed polarity will damage the board
- Keep wire length short between battery and board
- Add hot glue or electrical tape over solder joints to prevent vibration fatigue in unattended install
- Battery life estimate: ~30–35 days normal mode / ~8–10 days sustained alert mode

### XIAO ESP32-C6 Notes
- Uses a **RISC-V core** (not Xtensa) — requires **ESP32 Arduino core 3.0+** in board manager
- Deep sleep ~7µA — roughly 2x better battery life than ESP32-S3
- WiFi 6 (802.11ax) — falls back gracefully to WiFi 4/5 if router doesn't support it
- USB serial doesn't auto-reconnect after deep sleep during development
- Add a button-press wakeup or short active window on boot while testing
- **D2 and GPIO4 are the same pin** — D2 is the label on the board, GPIO4 is what the firmware uses (`#define SENSOR_PIN 4`). Connect your wire to the pin marked D2.

### DS18B20 1m Lead — Placement Tips
- Position probe **low in the room** — cold air settles at floor level near pipes
- Keep away from heating vents and exterior walls with direct sun exposure
- Crawl space or basement is highest-risk area for freeze damage — ideal sensor location
- 1m lead allows board to be mounted near power while sensor is optimally placed

---

## System Design

### Two Operating Modes

**Mode 1 — Normal (temp ≥ 36°F)**
- Sample temperature every 15 minutes (deep sleep between reads)
- WiFi stays OFF — only wake radio to transmit
- Track running daily high/low in RTC memory (survives deep sleep)
- At 6:00 PM: connect WiFi, publish one MQTT packet, disconnect
- Packet format: `h:68,l:41,t:1741993200` (~25 bytes)

**Mode 2 — Alert (temp < 36°F)**
- Switch immediately on threshold crossing
- Connect WiFi and transmit right away with warning flag
- Continue transmitting every 15 minutes
- Packet format: `h:38,l:36,w:1,t:1741993200`
- Optional escalation at 33°F: transmit every 5 minutes with urgent flag
- Revert to normal mode when temp recovers above 38°F (2°F hysteresis)

### Temperature Thresholds
| Threshold | Action |
|---|---|
| ≥ 38°F | Normal mode — daily summary at 6pm |
| < 36°F | Alert mode — transmit every 15 min with `w:1` flag |
| < 33°F | Escalation — transmit every 5 min with urgent flag |
| 40°F | Set point / target for the house |

### Data Usage Estimate
| Mode | Frequency | Approx bytes/day |
|---|---|---|
| Normal | 1x at 6pm | ~60 bytes |
| Alert (36°F) | Every 15 min | ~5,800 bytes |
| Critical (33°F) | Every 5 min | ~17,000 bytes |

---

## Firmware — Core Logic (Arduino)

```cpp
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// --- Config ---
#define SENSOR_PIN    4         // D2 on XIAO ESP32-C6
#define WIFI_SSID     "YourSSID"
#define WIFI_PASS     "YourPassword"
#define MQTT_BROKER   "broker.hivemq.com"
#define MQTT_PORT     1883
#define MQTT_TOPIC    "vacation/temp/status"

#define ALERT_TEMP    36.0
#define RECOVER_TEMP  38.0
#define URGENT_TEMP   33.0
#define SUMMARY_HOUR  18

// --- RTC Memory (survives deep sleep) ---
RTC_DATA_ATTR float dailyHigh = -999;
RTC_DATA_ATTR float dailyLow  =  999;
RTC_DATA_ATTR int   lastPublishHour = -1;
RTC_DATA_ATTR bool  inAlertMode = false;

OneWire oneWire(SENSOR_PIN);
DallasTemperature sensors(&oneWire);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

float readTemp() {
  sensors.begin();
  sensors.requestTemperatures();
  float c = sensors.getTempCByIndex(0);
  return c * 9.0 / 5.0 + 32.0;  // convert to Fahrenheit
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); tries++;
  }
  // Sync NTP time
  configTime(-5 * 3600, 0, "pool.ntp.org");  // adjust UTC offset for your timezone
}

void publishMQTT(float temp, float high, float low, bool alert, bool urgent) {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.connect("xiao-vacationhome");

  char payload[48];
  if (urgent)
    snprintf(payload, sizeof(payload), "h:%.1f,l:%.1f,c:%.1f,u:1", high, low, temp);
  else if (alert)
    snprintf(payload, sizeof(payload), "h:%.1f,l:%.1f,c:%.1f,w:1", high, low, temp);
  else
    snprintf(payload, sizeof(payload), "h:%.1f,l:%.1f", high, low);

  mqtt.publish(MQTT_TOPIC, payload);
  mqtt.disconnect();
}

void setup() {
  float temp = readTemp();

  // Update daily high/low
  if (temp > dailyHigh) dailyHigh = temp;
  if (temp < dailyLow)  dailyLow  = temp;

  // Get current time
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  // Reset high/low at midnight
  if (timeinfo.tm_hour == 0 && timeinfo.tm_min < 15) {
    dailyHigh = temp; dailyLow = temp;
  }

  // Determine mode
  bool urgent = (temp <= URGENT_TEMP);
  bool alert  = (temp <= ALERT_TEMP);
  bool recovering = (inAlertMode && temp >= RECOVER_TEMP);
  bool summaryDue = (timeinfo.tm_hour == SUMMARY_HOUR && lastPublishHour != SUMMARY_HOUR);

  if (recovering) inAlertMode = false;
  if (alert)      inAlertMode = true;

  // Transmit if needed
  if (inAlertMode || summaryDue) {
    connectWiFi();
    publishMQTT(temp, dailyHigh, dailyLow, inAlertMode, urgent);
    if (summaryDue) lastPublishHour = SUMMARY_HOUR;
    WiFi.disconnect(true);
  }

  // Sleep: 5 min if urgent, 15 min otherwise
  int sleepSec = urgent ? 300 : 900;
  esp_deep_sleep(sleepSec * 1000000ULL);
}

void loop() {} // never reached
```

---

## Backend Stack

### MQTT Broker
- **HiveMQ Cloud** — free tier, no server needed, good starting point
- **Mosquitto** on a $5/month VPS — more control

### Logic / Routing
- **Node-RED** — subscribes to `vacation/temp/status`, routes to notifications and database
- Free hosting: Railway.app or Render.com

### Data Storage
- **InfluxDB** (time-series) + **Grafana** dashboard for historical charts
- Or simpler: Google Sheets via Node-RED HTTP node

### Notifications to User
| Alert Level | Method |
|---|---|
| Daily summary | Pushover push notification (app, $5 one-time) |
| Warning (36°F) | Pushover + optional email via SendGrid |
| Urgent (33°F) | Twilio SMS |

---

## Network Notes

- System designed for home WiFi (e.g. "Holograph" network)
- **Important edge case:** If home router/internet goes down, no alerts will send
- Mitigation options:
  - Track `lastSuccessfulSend` in RTC memory; trigger local LED/buzzer after 24hr silence
  - Consider **Hologram.io cellular SIM** as backup or primary — independent of home internet

---

## PlatformIO Setup

**IDE:** VS Code + PlatformIO extension

**New Project Settings:**
- Board: `Seeed Studio XIAO ESP32C6`
- Framework: `Arduino`
- Code goes in `src/main.cpp` (not `.ino`) — add `#include <Arduino.h>` at the top

**platformio.ini** (replaces generated file):
```ini
[env:seeed_xiao_esp32c6]
platform = espressif32
board = seeed_xiao_esp32c6
framework = arduino
monitor_speed = 115200

lib_deps =
    milesburton/DallasTemperature @ ^3.11.0
    paulstoffregen/OneWire @ ^2.3.7
    knolleary/PubSubClient @ ^2.8.0
```
Libraries download automatically on first build — no manual installs needed.

---

## Next Steps
- [ ] Flash basic DS18B20 read to serial — verify sensor wiring
- [ ] Add MQTT publish to HiveMQ Cloud free broker
- [ ] Set up Node-RED and wire a Pushover test notification
- [ ] Test alert threshold logic (cool sensor manually)
- [ ] Add LiPo battery and verify deep sleep current draw
- [ ] Consider cellular backup if home internet reliability is a concern
