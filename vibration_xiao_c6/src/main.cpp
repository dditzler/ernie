// ============================================================
//  SteadyState — XIAO ESP32-C6 Vibration Sensor  fw 1.1.0
//
//  Responsibilities:
//    - Wakes on ADXL345 INT1 (activity interrupt) or 30s timer
//    - Samples ADXL345 accelerometer via I2C each wake
//    - Sends raw magnitude to hub via ESP-NOW every wake
//    - Hub injects timestamp + temp_trend; cloud assembles cycles
//    - No on/off threshold logic in firmware — cloud calibrates
//    - Heartbeat every HEARTBEAT_CYCLES wakes for keepalive
//    - Checks GitHub for OTA firmware updates every OTA_CHECK_CYCLES wakes
//
//  Wiring (XIAO ESP32-C6):
//    ADXL345 SDA  → D4 (GPIO22)
//    ADXL345 SCL  → D5 (GPIO23)
//    ADXL345 SDO  → GND         (I2C address 0x53)
//    ADXL345 CS   → 3.3V        (MUST be HIGH for I2C mode)
//    ADXL345 INT1 → D1 (GPIO1)  (activity interrupt → deep sleep wakeup)
//
//  VibrationMsg struct must match hub exactly.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include <WiFiClientSecure.h>
#include <SparkFun_ADXL345.h>
#include "version.h"
#include "secrets.h"
#include "ota_update.h"

// ─── Hub MAC and channel ──────────────────────────────────────────────────────
// T-SIM7000G (SIM7-0001) — primary cellular hub
uint8_t HUB_MAC[]  = { 0x04, 0x83, 0x08, 0x11, 0x5F, 0xE8 };
#define WIFI_CHANNEL  1

// ─── WiFi credentials (for OTA only) ─────────────────────────────────────────
const char* WIFI_NETWORKS[][2] = {
  { "WookieBear",   "7737443630"    },
  { "airport",      "HitekPrinting" },
};

// ─── Pin assignments ──────────────────────────────────────────────────────────
#define SDA_PIN      22   // D4
#define SCL_PIN      23   // D5
#define ADXL_INT1    1    // D1 — ADXL345 INT1 → deep sleep wakeup

// ─── Timing ──────────────────────────────────────────────────────────────────
#define SLEEP_DURATION_S    30    // timer-based wake interval
#define SAMPLE_COUNT         5    // ADXL345 readings per wake
#define SAMPLE_INTERVAL_MS 100    // ms between readings
#define HEARTBEAT_CYCLES     2    // keepalive every N cycles (2×30s = 1 min)
#define OTA_CHECK_CYCLES   120    // OTA check every N cycles (~1 hr)

// ─── ADXL345 activity threshold ──────────────────────────────────────────────
// Units: 62.5 mg/LSB. 4 = 250 mg — fires on meaningful vibration, ignores noise.
// Tune up if you get spurious wakes at idle; tune down if missing real activity.
#define ADXL_ACTIVITY_THRESHOLD  4   // LSBs (4 × 62.5 mg = 250 mg)

// ─── ESP-NOW message struct (must match hub exactly) ─────────────────────────
// fw 1.1.0: no on/off decision in firmware. Hub injects temp_trend; cloud
// assembles cycles and calibrates per device from the raw magnitude stream.
typedef struct {
  uint8_t  wakeReason;    // 0=activity interrupt, 1=timer, 2=heartbeat
  uint16_t rawMagnitude;  // max axis range in ADXL345 counts this wake
} VibrationMsg;           // sizeof = 4 bytes

// ─── RTC memory — persists across deep sleep ─────────────────────────────────
RTC_DATA_ATTR uint32_t wakeCycle    = 0;
RTC_DATA_ATTR uint32_t lastOtaCycle = 0;

// ─── Send callback ────────────────────────────────────────────────────────────
volatile bool sendComplete = false;
void onSendComplete(const uint8_t* mac, esp_now_send_status_t status) {
  sendComplete = true;
  Serial.printf("[ESP-NOW] send: %s\n",
    status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

// ─── Configure ADXL345 for activity interrupt on INT1 ────────────────────────
// Re-applied every wake — ADXL345 retains config across deep sleep as long as
// VCC stays on, but re-applying is cheap and guards against power glitches.
void configureADXL345Interrupt(ADXL345 &adxl) {
  adxl.setActivityThreshold(ADXL_ACTIVITY_THRESHOLD);
  adxl.setActivityX(true);
  adxl.setActivityY(true);
  adxl.setActivityZ(true);
  adxl.setActivityAc(true);    // AC-coupled: ignores DC gravity offset

  // Map Activity → INT1, everything else → INT2 (unused)
  // Args: single_tap, double_tap, free_fall, activity, inactivity
  adxl.setImportantInterruptMapping(ADXL345_INT2_PIN, ADXL345_INT2_PIN,
                                    ADXL345_INT2_PIN, ADXL345_INT1_PIN,
                                    ADXL345_INT2_PIN);

  // Clear any latched interrupt from previous wake
  adxl.getInterruptSource();

  // Enable Activity interrupt
  adxl.ActivityINT(true);

  Serial.printf("[ADXL] threshold=%d (%.0f mg) AC-coupled all-axes INT1\n",
    ADXL_ACTIVITY_THRESHOLD, ADXL_ACTIVITY_THRESHOLD * 62.5f);
}

// ─── Sample ADXL345 ───────────────────────────────────────────────────────────
// Returns raw max-axis range (ADXL345 counts) across SAMPLE_COUNT readings.
uint16_t sampleADXL345(ADXL345 &adxl) {
  int xs[SAMPLE_COUNT], ys[SAMPLE_COUNT], zs[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    adxl.readAccel(&xs[i], &ys[i], &zs[i]);
    if (i < SAMPLE_COUNT - 1) delay(SAMPLE_INTERVAL_MS);
  }

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

  Serial.printf("[ADXL] x:%d y:%d z:%d  max:%d counts\n",
                xRange, yRange, zRange, maxRange);
  return (uint16_t)maxRange;
}

// ─── Send ESP-NOW message to hub ─────────────────────────────────────────────
void sendToHub(uint8_t wakeReason, uint16_t rawMagnitude) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

#if defined(ARDUINO_XIAO_ESP32C6)
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif

  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  delay(100);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed");
    return;
  }
  esp_now_register_send_cb(onSendComplete);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, HUB_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  VibrationMsg msg = { wakeReason, rawMagnitude };
  sendComplete = false;
  esp_now_send(HUB_MAC, (uint8_t*)&msg, sizeof(msg));

  unsigned long t = millis();
  while (!sendComplete && millis() - t < 500) delay(10);

  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

// ─── OTA check ───────────────────────────────────────────────────────────────
void checkOtaIfDue() {
  bool otaDue = (wakeCycle == 1) ||
                ((wakeCycle - lastOtaCycle) >= OTA_CHECK_CYCLES);
  if (!otaDue) return;

  lastOtaCycle = wakeCycle;
  Serial.println("[OTA] Check due — connecting WiFi...");

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
    Serial.println("[OTA] WiFi failed — skipping.");
    WiFi.mode(WIFI_OFF);
    return;
  }

  static WiFiClientSecure otaSsl;
  otaSsl.setInsecure();
  otaInit(otaSsl);
  otaCheckNow();   // reboots if update found, no-op if current

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  Serial.println("[OTA] Done.");
}

// ─── Setup (runs on every wake from deep sleep) ───────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  wakeCycle++;

  // Determine wake reason
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint8_t wakeReason;
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    wakeReason = 0;
    Serial.printf("\n=== EC6-0001  fw=%s  wake #%lu  [ACTIVITY INTERRUPT] ===\n",
                  FW_VERSION_STR, wakeCycle);
  } else {
    wakeReason = 1;
    Serial.printf("\n=== EC6-0001  fw=%s  wake #%lu  [TIMER] ===\n",
                  FW_VERSION_STR, wakeCycle);
  }

  // Heartbeat overrides reason code for keepalive tracking
  bool heartbeatDue = (wakeCycle == 1) || (wakeCycle % HEARTBEAT_CYCLES == 0);
  if (heartbeatDue) wakeReason = 2;

  // Init I2C + ADXL345
  Wire.begin(SDA_PIN, SCL_PIN);
  ADXL345 adxl;
  adxl.powerOn();
  delay(10);

  // Verify chip responding
  Wire.beginTransmission(0x53);
  if (Wire.endTransmission() != 0) {
    Serial.println("[ADXL] Not found — check wiring. Sleeping.");
    esp_sleep_enable_ext1_wakeup(1ULL << ADXL_INT1, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_S * 1000000ULL);
    esp_deep_sleep_start();
  }

  // Configure activity interrupt
  configureADXL345Interrupt(adxl);

  // Sample raw magnitude
  uint16_t rawMag = sampleADXL345(adxl);

  Serial.printf("[SEND] reason=%d (%s)  rawMag=%u\n",
    wakeReason,
    wakeReason == 0 ? "activity" : wakeReason == 1 ? "timer" : "heartbeat",
    rawMag);

  // Send every wake — hub+cloud decide what the magnitude means
  sendToHub(wakeReason, rawMag);

  // OTA check (first wake + every ~1hr)
  checkOtaIfDue();

  // Clear ADXL interrupt latch so INT1 can fire again next cycle
  adxl.getInterruptSource();

  // Configure wakeup:
  //   EXT1  = ADXL345 INT1 on GPIO1, fires when HIGH (interrupt asserted)
  //   Timer = 30s fallback for heartbeat + quiet-period keepalive
  esp_sleep_enable_ext1_wakeup(1ULL << ADXL_INT1, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_S * 1000000ULL);

  Serial.printf("[SLEEP] ext1 mask=0x%llx (GPIO%d) + timer=%ds\n\n",
                1ULL << ADXL_INT1, ADXL_INT1, SLEEP_DURATION_S);
  Serial.flush();

#ifndef DEBUG_NO_SLEEP
  esp_deep_sleep_start();
#endif
}

void loop() {
#ifdef DEBUG_NO_SLEEP
  // Bench mode: continuous sample + send every 2s
  delay(2000);

  Wire.begin(SDA_PIN, SCL_PIN);
  ADXL345 adxl;
  adxl.powerOn();
  delay(10);

#if defined(ARDUINO_XIAO_ESP32C6)
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif

  configureADXL345Interrupt(adxl);
  uint16_t mag = sampleADXL345(adxl);
  Serial.printf("[LOOP] mag=%u — sending\n", mag);
  sendToHub(1, mag);
  adxl.getInterruptSource();
#endif
}
