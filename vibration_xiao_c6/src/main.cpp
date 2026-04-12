// ============================================================
//  SteadyState — XIAO ESP32-C6 Vibration Sensor
//
//  Responsibilities:
//    - Reads ADXL345 accelerometer via I2C each wake cycle
//    - Detects furnace on/off by measuring vibration variance
//    - Sends state changes to WROOM-32 hub via ESP-NOW
//    - Sends heartbeat every HEARTBEAT_CYCLES wake cycles
//    - Reports raw vibration magnitude for threshold calibration
//    - Deep sleeps SLEEP_DURATION_S seconds between samples
//    - Checks GitHub for OTA firmware updates every OTA_CHECK_CYCLES wakes
//
//  First-time setup:
//    1. Flash hub_wroom32 first — it prints its MAC and channel on boot
//    2. HUB_MAC and WIFI_CHANNEL below are already set
//    3. Attach rubber duck antenna to U.FL connector
//    4. Flash this firmware
//
//  ADXL345 wiring (I2C, XIAO ESP32-C6):
//    VCC  → 3.3V     GND  → GND
//    SDA  → D4       SCL  → D5
//    SDO  → GND      (sets I2C address 0x53)
//    CS   → 3.3V     (MUST be HIGH for I2C mode)
//
//  VibrationMsg struct must match WROOM32 hub exactly.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <WiFiClientSecure.h>
#include <SparkFun_ADXL345.h>
#include "version.h"
#include "secrets.h"      // OTA_GITHUB_TOKEN — must come before ota_update.h
#include "ota_update.h"

// ─── Hub MAC and channel ──────────────────────────────────────────────────────
uint8_t HUB_MAC[]   = { 0x3C, 0x8A, 0x1F, 0x90, 0xD4, 0x30 };
#define WIFI_CHANNEL  6   // must match channel printed by hub on boot

// ─── WiFi credentials (for OTA — ESP-NOW doesn't need association) ───────────
const char* WIFI_NETWORKS[][2] = {
  { "WookieBear",   "7737443630"    },
  { "airport",      "HitekPrinting" },
};

// ─── Pin assignments (XIAO ESP32-C6) ─────────────────────────────────────────
#define SDA_PIN  22   // D4
#define SCL_PIN  23   // D5

// ─── Timing ──────────────────────────────────────────────────────────────────
#define SLEEP_DURATION_S    30    // wake and sample every N seconds
#define SAMPLE_COUNT         5    // readings per wake cycle
#define SAMPLE_INTERVAL_MS 400    // ms between readings
#define HEARTBEAT_CYCLES     2    // heartbeat every N cycles (2×30s = 1min)
#define OTA_CHECK_CYCLES   120    // OTA check every N cycles (120×30s = 1hr)

// ─── Vibration detection ─────────────────────────────────────────────────────
// Uses range (max - min) across SAMPLE_COUNT readings per axis.
// A still sensor has low range; a vibrating sensor has high range.
// No stored baseline needed — self-referential per wake cycle.
// ON_RANGE_THRESHOLD: if max range across any axis exceeds this, furnace is ON.
// Watch vacation/dev/hub/sensor_raw in HiveMQ to find your furnace's actual
// range values when on vs off, then set this between them.
#define ON_RANGE_THRESHOLD  12    // ADXL345 counts — calibrated 2026-04-12
                                  // idle=2-4, startup transient=5-8, blower=13-64
                                  // 12 sits cleanly between noise floor and blower
#define ON_THRESHOLD         3    // of SAMPLE_COUNT readings must show vibration
#define OFF_THRESHOLD        4    // of SAMPLE_COUNT must be quiet to confirm OFF

// ─── RTC memory — persists across deep sleep ──────────────────────────────────
RTC_DATA_ATTR bool     lastHvacState  = false;
RTC_DATA_ATTR uint32_t wakeCycle      = 0;
RTC_DATA_ATTR uint32_t hvacOnSince    = 0;
RTC_DATA_ATTR uint32_t lastOtaCycle   = 0;  // wake cycle when OTA was last checked

// ─── ESP-NOW message struct (must match WROOM32 hub exactly) ─────────────────
typedef struct {
  bool     hvacOn;          // true = furnace vibration detected
  uint8_t  reason;          // 0 = state change, 1 = heartbeat
  uint32_t onDurationS;     // seconds vibration has been continuous (0 if off)
  uint16_t rawMagnitude;    // max axis range seen this wake cycle (for calibration)
} VibrationMsg;

// ─── Send callback ────────────────────────────────────────────────────────────
volatile bool sendComplete = false;
void onSendComplete(const uint8_t* mac, esp_now_send_status_t status) {
  sendComplete = true;
  Serial.printf("ESP-NOW send: %s\n",
    status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

// ─── Sample ADXL345 ───────────────────────────────────────────────────────────
// Returns: highCount (readings above ON_RANGE_THRESHOLD)
// Writes:  rawMagnitude (max range seen across any axis)
int sampleADXL345(uint16_t &rawMagnitude) {
  Wire.begin(SDA_PIN, SCL_PIN);
  ADXL345 adxl;
  adxl.powerOn();
  delay(100);  // stabilize

  // Verify chip is responding via I2C ack (address 0x53 when SDO=GND)
  Wire.beginTransmission(0x53);
  if (Wire.endTransmission() != 0) {
    Serial.println("ADXL345 not found — check wiring");
    rawMagnitude = 0;
    return 0;
  }

  // Collect SAMPLE_COUNT readings
  int xs[SAMPLE_COUNT], ys[SAMPLE_COUNT], zs[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    adxl.readAccel(&xs[i], &ys[i], &zs[i]);
    delay(SAMPLE_INTERVAL_MS);
  }

  // Compute range (max - min) per axis
  int xMin = xs[0], xMax = xs[0];
  int yMin = ys[0], yMax = ys[0];
  int zMin = zs[0], zMax = zs[0];
  for (int i = 1; i < SAMPLE_COUNT; i++) {
    xMin = min(xMin, xs[i]); xMax = max(xMax, xs[i]);
    yMin = min(yMin, ys[i]); yMax = max(yMax, ys[i]);
    zMin = min(zMin, zs[i]); zMax = max(zMax, zs[i]);
  }
  int xRange = xMax - xMin;
  int yRange = yMax - yMin;
  int zRange = zMax - zMin;
  int maxRange = max(xRange, max(yRange, zRange));

  rawMagnitude = (uint16_t)maxRange;
  Serial.printf("ADXL345 ranges — x:%d y:%d z:%d  max:%d\n",
                xRange, yRange, zRange, maxRange);

  // Count how many individual readings showed significant vibration
  // Use per-reading magnitude vs mid-point of each axis range as reference
  int xMid = (xMin + xMax) / 2;
  int yMid = (yMin + yMax) / 2;
  int zMid = (zMin + zMax) / 2;
  int highCount = 0;
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int dev = max(abs(xs[i] - xMid), max(abs(ys[i] - yMid), abs(zs[i] - zMid)));
    if (dev > ON_RANGE_THRESHOLD / 2) highCount++;
  }

  return highCount;
}

// ─── Send ESP-NOW message to hub ─────────────────────────────────────────────
void sendToHub(bool hvacOn, uint8_t reason, uint32_t onDurationS, uint16_t rawMagnitude) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();           // forces WiFi stack to start — required on C6

  // Seeed XIAO ESP32-C6: select external u.FL antenna via RF switch GPIOs.
  // GPIO3 (WIFI_ENABLE)      LOW  = activate RF switch control circuit
  // GPIO14 (WIFI_ANT_CONFIG) HIGH = external antenna, LOW = internal PCB antenna
#if defined(ARDUINO_XIAO_ESP32C6)
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif

  // Disable modem sleep and max TX power — prevents power bank auto-shutoff
  // and ensures the external antenna is fully utilized.
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  delay(100);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSendComplete);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, HUB_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  VibrationMsg msg = { hvacOn, reason, onDurationS, rawMagnitude };
  sendComplete = false;
  esp_now_send(HUB_MAC, (uint8_t*)&msg, sizeof(msg));

  // Wait for send callback (max 500ms)
  unsigned long t = millis();
  while (!sendComplete && millis() - t < 500) delay(10);

  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

// ─── OTA check (runs when due, after ESP-NOW send) ───────────────────────────
// Connects to WiFi (full association), checks GitHub for new firmware.
// If an update is available, flashes and reboots — never returns.
// Called from setup() when (wakeCycle - lastOtaCycle) >= OTA_CHECK_CYCLES.
void checkOtaIfDue() {
  bool otaDue = (wakeCycle == 1) ||
                ((wakeCycle - lastOtaCycle) >= OTA_CHECK_CYCLES);
  if (!otaDue) return;

  lastOtaCycle = wakeCycle;
  Serial.println("[OTA] Check due — connecting WiFi...");

  // External antenna (same as ESP-NOW path)
#if defined(ARDUINO_XIAO_ESP32C6)
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  int netCount = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
  bool connected = false;
  for (int i = 0; i < netCount; i++) {
    WiFi.begin(WIFI_NETWORKS[i][0], WIFI_NETWORKS[i][1]);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
      delay(500); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[OTA] WiFi: %s\n", WIFI_NETWORKS[i][0]);
      connected = true;
      break;
    }
    WiFi.disconnect();
  }

  if (!connected) {
    Serial.println("[OTA] WiFi failed — skipping OTA check.");
    WiFi.mode(WIFI_OFF);
    return;
  }

  // otaCheckNow() flashes + reboots if update found; no-op if up to date.
  static WiFiClientSecure otaSsl;
  otaSsl.setInsecure();
  otaInit(otaSsl);
  otaCheckNow();    // returns false if up to date, never returns if flashed

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  Serial.println("[OTA] Done — resuming sleep cycle.");
}

// ─── Setup (runs on every wake from deep sleep) ───────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  wakeCycle++;
  Serial.printf("\n=== XIAO EC6-0001  fw=%s — wake #%lu ===\n",
                FW_VERSION_STR, wakeCycle);

  // Sample ADXL345
  uint16_t rawMagnitude = 0;
  int highReadings = sampleADXL345(rawMagnitude);
  Serial.printf("Vibration readings: %d/%d above threshold  rawMag=%u\n",
                highReadings, SAMPLE_COUNT, rawMagnitude);

  bool currentState = lastHvacState;

  // State transitions with hysteresis
  if (!lastHvacState && highReadings >= ON_THRESHOLD) {
    currentState = true;
    hvacOnSince  = wakeCycle;
    Serial.println("STATE CHANGE: furnace ON");
  } else if (lastHvacState && highReadings <= (SAMPLE_COUNT - OFF_THRESHOLD)) {
    currentState = false;
    Serial.println("STATE CHANGE: furnace OFF");
  }

  bool stateChanged = (currentState != lastHvacState);
  bool heartbeatDue = (wakeCycle == 1) || (wakeCycle % HEARTBEAT_CYCLES == 0);
  lastHvacState     = currentState;

  uint32_t onDurationS = 0;
  if (currentState && hvacOnSince > 0)
    onDurationS = (wakeCycle - hvacOnSince) * SLEEP_DURATION_S;

  if (stateChanged || heartbeatDue) {
    uint8_t reason = stateChanged ? 0 : 1;
    Serial.printf("Sending — hvac=%s  reason=%s  onDur=%ds  mag=%u\n",
      currentState ? "ON" : "OFF",
      reason == 0 ? "state_change" : "heartbeat",
      onDurationS, rawMagnitude);
    sendToHub(currentState, reason, onDurationS, rawMagnitude);
  } else {
    Serial.printf("No send — hvac=%s  next heartbeat in %lu cycles\n",
      currentState ? "ON" : "OFF",
      HEARTBEAT_CYCLES - (wakeCycle % HEARTBEAT_CYCLES));
  }

  // OTA check — runs at most once per OTA_CHECK_CYCLES wake cycles.
  // WiFi is up if we just called sendToHub(); OTA reuses the WiFi radio
  // but needs full association (not just ESP-NOW channel tuning), so
  // checkOtaIfDue() brings up its own association. If firmware flashed,
  // the device reboots — never reaches the sleep call below.
  checkOtaIfDue();

  Serial.printf("Sleeping %ds...\n\n", SLEEP_DURATION_S);
  Serial.flush();

#ifndef DEBUG_NO_SLEEP
  esp_deep_sleep(SLEEP_DURATION_S * 1000000ULL);
#endif
}

void loop() {
#ifdef DEBUG_NO_SLEEP
  // Continuous raw output + periodic ESP-NOW send for calibration.
  // Reads ADXL345 every 500ms, sends magnitude to hub every 5s.
  static unsigned long lastSend = 0;
  static int16_t xMin = 32767, xMax = -32768;
  static int16_t yMin = 32767, yMax = -32768;
  static int16_t zMin = 32767, zMax = -32768;

#if defined(ARDUINO_XIAO_ESP32C6)
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif

  Wire.begin(SDA_PIN, SCL_PIN);
  ADXL345 adxl;
  adxl.powerOn();
  delay(100);

  int x, y, z;
  adxl.readAccel(&x, &y, &z);
  Serial.printf("x=%4d  y=%4d  z=%4d\n", x, y, z);

  // Track running min/max for magnitude calculation
  if (x < xMin) xMin = x; if (x > xMax) xMax = x;
  if (y < yMin) yMin = y; if (y > yMax) yMax = y;
  if (z < zMin) zMin = z; if (z > zMax) zMax = z;

  // Send to hub every 5 seconds with current max range as rawMagnitude
  unsigned long now = millis();
  if (now - lastSend >= 5000) {
    uint16_t mag = (uint16_t)max((int)(xMax - xMin),
                             max((int)(yMax - yMin),
                                 (int)(zMax - zMin)));
    Serial.printf("  → sending mag=%u to hub\n", mag);
    sendToHub(false, 1, 0, mag);  // reason=1 heartbeat
    // Reset min/max window
    xMin = x; xMax = x;
    yMin = y; yMax = y;
    zMin = z; zMax = z;
    lastSend = now;
  }

  delay(500);
#endif
}
