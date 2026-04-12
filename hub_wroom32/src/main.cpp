// ============================================================
//  Vacation Home — WROOM-32 Hub
//
//  Responsibilities:
//    - BME280 reads indoor temperature + humidity every 30s
//    - Receives HVAC on/off state from XIAO ESP32-C6 via ESP-NOW
//    - Tracks HVAC cycle timing (start time, duration, temp delta)
//    - Publishes realtime status and cycle summaries to HiveMQ Cloud
//
//  ESP-NOW setup:
//    - This device is the RECEIVER (hub)
//    - Print MAC address on boot — paste into XIAO firmware as HUB_MAC
//    - Both devices must be on the same WiFi channel (printed on boot)
//
//  MQTT topics:
//    vacation/dev/hub/status  — realtime temp/humidity/hvac state (30s)
//    vacation/dev/hub/cycle   — HVAC cycle summary on each run end
//    vacation/dev/hub/daily   — hi/lo summary at 6PM Central
//
//  BME280 wiring (I2C):
//    VCC → 3.3V  |  GND → GND  |  SCL → GPIO22  |  SDA → GPIO21
//    CSB → 3.3V  |  SDO → GND  (sets I2C address 0x76)
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <time.h>
#include <sys/time.h>
#include "version.h"
#include "secrets.h"    // OTA_GITHUB_TOKEN — must come before ota_update.h
#include "ota_update.h"

// ─── WiFi credentials ────────────────────────────────────────────────────────
const char* WIFI_NETWORKS[][2] = {
  { "WookieBear",   "7737443630"    },
  { "airport",      "HitekPrinting" },
};

// ─── MQTT (HiveMQ Cloud) ─────────────────────────────────────────────────────
const char* MQTT_HOST  = "ad21434501c84aad995bc5621bf77f15.s1.eu.hivemq.cloud";
const int   MQTT_PORT  = 8883;
const char* MQTT_USER  = "vacation-sensor";
const char* MQTT_PASS  = "6WJJY9C@3R7rY7d";
const char* TOPIC_STATUS     = "vacation/dev/hub/status";
const char* TOPIC_CYCLE      = "vacation/dev/hub/cycle";
const char* TOPIC_DAILY      = "vacation/dev/hub/daily";
const char* TOPIC_SENSOR_RAW = "vacation/dev/hub/sensor_raw";  // raw vibration magnitude for threshold calibration

// ─── Timing ──────────────────────────────────────────────────────────────────
#define PUBLISH_INTERVAL  30000UL
#define TZ_CENTRAL        "CST6CDT,M3.2.0,M11.1.0"

// ─── ESP-NOW message structure (must match sensor firmware exactly) ───────────
typedef struct {
  bool     hvacOn;          // true = furnace vibration detected
  uint8_t  reason;          // 0 = state change, 1 = heartbeat
  uint32_t onDurationS;     // seconds vibration has been continuous (0 if off)
  uint16_t rawMagnitude;    // max axis deviation in ADXL345 counts — for calibration
} VibrationMsg;

// ─── State ───────────────────────────────────────────────────────────────────
Adafruit_BMP280    bme;
WiFiClientSecure   wifiSecure;     // MQTT TLS client
WiFiClientSecure   otaSecure;      // OTA TLS client (separate from MQTT)
PubSubClient       mqtt(wifiSecure);

bool          hvacOn          = false;
unsigned long cycleStartMs    = 0;
float         tempAtCycleStart = 0.0f;
unsigned long lastPublish      = 0;
bool          timeSynced       = false;

// ─── Daily hi/lo ─────────────────────────────────────────────────────────────
float dailyHi   = -999.0f, dailyLo = 999.0f;
int   dailyReadings = 0;
bool  dailySent = false;
int   lastDay   = -1;

// ─── Rolling cycle stats (last 10 cycles) ────────────────────────────────────
#define MAX_CYCLES 10
float cycleDurations[MAX_CYCLES] = {0};
int   cycleCount = 0;

float rollingAvgCycleMin() {
  int n = min(cycleCount, MAX_CYCLES);
  if (n == 0) return 0;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += cycleDurations[i];
  return sum / n;
}

// ─── ESP-NOW receive callback ─────────────────────────────────────────────────
void onVibrationReceived(const esp_now_recv_info_t* info,
                         const uint8_t* data, int len) {
  if (len != sizeof(VibrationMsg)) return;
  VibrationMsg msg;
  memcpy(&msg, data, sizeof(msg));

  int8_t rssi = info->rx_ctrl->rssi;
  Serial.printf("ESP-NOW rx: hvac=%s  reason=%s  onDur=%ds  mag=%u  rssi=%d dBm\n",
    msg.hvacOn ? "ON" : "OFF",
    msg.reason == 0 ? "state_change" : "heartbeat",
    msg.onDurationS,
    msg.rawMagnitude,
    rssi);

  // Publish raw magnitude on every message for threshold calibration
  // Subscribe to vacation/dev/hub/sensor_raw in HiveMQ to watch live values
  char rawPayload[120];
  snprintf(rawPayload, sizeof(rawPayload),
    "{\"hvac\":\"%s\",\"magnitude\":%u,\"reason\":\"%s\",\"rssi\":%d}",
    msg.hvacOn ? "on" : "off",
    msg.rawMagnitude,
    msg.reason == 0 ? "state_change" : "heartbeat",
    rssi);
  mqtt.publish(TOPIC_SENSOR_RAW, rawPayload);

  // State change — not just a heartbeat
  if (msg.reason == 0) {
    if (msg.hvacOn && !hvacOn) {
      // Furnace just started
      hvacOn           = true;
      cycleStartMs     = millis();
      tempAtCycleStart = bme.readTemperature() * 9.0f / 5.0f + 32.0f;
      Serial.printf("HVAC ON — start temp %.1f°F\n", tempAtCycleStart);

    } else if (!msg.hvacOn && hvacOn) {
      // Furnace just stopped — publish cycle summary
      hvacOn = false;
      float durationMin = (millis() - cycleStartMs) / 60000.0f;
      float tempNow     = bme.readTemperature() * 9.0f / 5.0f + 32.0f;
      float tempDelta   = tempNow - tempAtCycleStart;

      // Store in rolling buffer
      cycleDurations[cycleCount % MAX_CYCLES] = durationMin;
      cycleCount++;

      char payload[256];
      snprintf(payload, sizeof(payload),
        "{\"mode\":\"cycle\",\"duration_min\":%.1f,\"temp_start_f\":%.1f,"
        "\"temp_end_f\":%.1f,\"temp_delta_f\":%.1f,\"avg_cycle_min\":%.1f,"
        "\"cycle_count\":%d}",
        durationMin, tempAtCycleStart, tempNow, tempDelta,
        rollingAvgCycleMin(), cycleCount);

      Serial.printf("HVAC OFF — cycle %.1f min, delta +%.1f°F\n",
                    durationMin, tempDelta);
      mqtt.publish(TOPIC_CYCLE, payload);
    }
  }
}

// ─── WiFi connect ─────────────────────────────────────────────────────────────
bool connectWiFi() {
  int count = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
  for (int i = 0; i < count; i++) {
    Serial.printf("Trying WiFi: %s\n", WIFI_NETWORKS[i][0]);
    WiFi.begin(WIFI_NETWORKS[i][0], WIFI_NETWORKS[i][1]);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected — IP: %s  Channel: %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
      Serial.printf("Hub MAC: %s  ← paste into XIAO firmware\n",
                    WiFi.macAddress().c_str());
      return true;
    }
    WiFi.disconnect();
  }
  return false;
}

// ─── MQTT connect ─────────────────────────────────────────────────────────────
void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  String clientId = "hub-wroom32-" + WiFi.macAddress();
  while (!mqtt.connected()) {
    Serial.print("Connecting MQTT...");
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS))
      Serial.println(" OK");
    else {
      Serial.printf(" failed rc=%d — retry 5s\n", mqtt.state());
      delay(5000);
    }
  }
}

// ─── Daily summary ────────────────────────────────────────────────────────────
void checkDailySummary(float temp) {
  if (!timeSynced) {
    if (time(nullptr) > 1000000000UL) timeSynced = true;
    else return;
  }
  if (temp > dailyHi) dailyHi = temp;
  if (temp < dailyLo) dailyLo = temp;
  dailyReadings++;

  time_t now = time(nullptr);
  struct tm ct;
  localtime_r(&now, &ct);

  if (lastDay != -1 && ct.tm_mday != lastDay) {
    dailyHi = temp; dailyLo = temp; dailyReadings = 1; dailySent = false;
  }
  lastDay = ct.tm_mday;

  if (ct.tm_hour >= 18 && !dailySent && dailyReadings >= 1) {
    char dateBuf[12];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &ct);
    char payload[200];
    snprintf(payload, sizeof(payload),
      "{\"mode\":\"daily\",\"date\":\"%s\",\"hi_f\":%.1f,\"lo_f\":%.1f,"
      "\"cycles\":%d,\"avg_cycle_min\":%.1f}",
      dateBuf, dailyHi, dailyLo, cycleCount, rollingAvgCycleMin());
    if (mqtt.publish(TOPIC_DAILY, payload)) dailySent = true;
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n=== WROOM-32 Hub  fw=%s ===\n", FW_VERSION_STR);

  // I2C bus init
  Wire.begin();

#ifdef I2C_SCAN
  // Diagnostic scan — enabled via build_flags = -DI2C_SCAN in platformio.ini
  // Identifies address and chip type; disable once sensor is confirmed working
  Serial.println("Scanning I2C bus...");
  int i2cFound = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", a);
      // Read chip ID register (0xD0) to distinguish BMP280 vs BME280
      Wire.beginTransmission(a);
      Wire.write(0xD0);
      Wire.endTransmission(false);
      Wire.requestFrom(a, (uint8_t)1);
      if (Wire.available()) {
        uint8_t chipId = Wire.read();
        if      (chipId == 0x60) Serial.printf("    Chip ID 0x60 → BME280 ✓\n");
        else if (chipId == 0x58) Serial.printf("    Chip ID 0x58 → BMP280 (no humidity)\n");
        else                     Serial.printf("    Chip ID 0x%02X → unknown\n", chipId);
      }
      i2cFound++;
    }
  }
  if (i2cFound == 0) Serial.println("  No I2C devices found — check SDA(21)/SCL(22) wiring");
#endif

  // BME280
  uint8_t addr = 0;
  if      (bme.begin(0x76)) addr = 0x76;
  else if (bme.begin(0x77)) addr = 0x77;
  else {
    Serial.println("FATAL: BMP280 not found — check wiring.");
    while (true) delay(10000);
  }
  Serial.printf("BMP280 ready at 0x%02X\n", addr);

  // WiFi (needed before ESP-NOW so channel is set)
  WiFi.mode(WIFI_STA);
  if (!connectWiFi()) {
    Serial.println("FATAL: no WiFi — halting.");
    while (true) delay(10000);
  }

  // NTP
  setenv("TZ", TZ_CENTRAL, 1); tzset();
  configTime(0, 0, "pool.ntp.org");
  struct tm t;
  if (getLocalTime(&t, 8000)) {
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    Serial.printf("NTP synced: %s Central\n", buf);
    timeSynced = true;
  }

  // MQTT
  wifiSecure.setInsecure();
  connectMQTT();

  // OTA — uses a separate TLS client so MQTT is never interrupted
  otaSecure.setInsecure();
  otaInit(otaSecure);

  // ESP-NOW (init AFTER WiFi — inherits the WiFi channel)
  if (esp_now_init() != ESP_OK) {
    Serial.println("FATAL: ESP-NOW init failed.");
    while (true) delay(10000);
  }
  esp_now_register_recv_cb(onVibrationReceived);
  Serial.println("ESP-NOW receiver ready — waiting for XIAO...");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  otaLoop();   // rate-limited to OTA_CHECK_INTERVAL_S; no-ops until due

  unsigned long now = millis();
  if (now - lastPublish < PUBLISH_INTERVAL) return;
  lastPublish = now;

  float tempC = bme.readTemperature();
  if (isnan(tempC)) {
    Serial.println("ERROR: BMP280 read failed");
    return;
  }
  float tempF = tempC * 9.0f / 5.0f + 32.0f;

  checkDailySummary(tempF);

  float cycleRunMin = hvacOn ? (millis() - cycleStartMs) / 60000.0f : 0.0f;

  char payload[240];
  snprintf(payload, sizeof(payload),
    "{\"mode\":\"realtime\",\"temp_f\":%.1f,"
    "\"hvac\":\"%s\",\"cycle_min\":%.1f,\"fw\":\"%s\"}",
    tempF,
    hvacOn ? "on" : "off",
    cycleRunMin,
    FW_VERSION_STR);

  Serial.printf("[HUB] %s\n", payload);
  mqtt.publish(TOPIC_STATUS, payload);
}
