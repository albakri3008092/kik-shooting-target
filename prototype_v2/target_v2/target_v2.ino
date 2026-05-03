// =====================================================================
//  KIK Shooting Target — PROTOTYPE V2 (target node)
// =====================================================================
//  Improvements over demo (v1):
//    1. Hit-window state machine: when a piezo crosses threshold, we
//       capture the peak amplitude on ALL 4 sensors for the next
//       HIT_WINDOW_MS so the central can do amplitude-triangulation.
//    2. Lower debounce (60 ms) so double-taps with splits >= 0.10 s are
//       detected as two independent hits, not one merged event.
//    3. New protocol HitPacketV2 carries timestamp + 4 peaks + trigger
//       sensor, enabling central-side (X, Y) position estimation, split
//       time tracking, and ISSF score zone classification.
//
//  Wiring assumes the production hardware: 1 MO pull-down to GND on
//  each piezo signal pin. The demo software fallbacks (rising-edge +
//  baseline clamp) are kept for resilience.
//
//  Per-board edits (top of file):
//    - TARGET_ID       (1..NUM_TARGETS, unique per board)
//    - RECEIVER_MAC[]  (MAC of central receiver, AP-mode MAC)
// =====================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- EDIT THESE TWO LINES PER BOARD ----------
// TARGET_ID must be unique across all 15 target boards. The central
// (central_v2.ino) tracks state for IDs 1..15 and ignores anything
// outside that range, so flashing 16+ boards will silently drop them.
#define TARGET_ID    1                                      // 1..15
static uint8_t RECEIVER_MAC[6] = { 0xB0, 0xCB, 0xD8, 0xCF, 0xF1, 0x61 };
// ----------------------------------------------------

// Piezo wiring: GPIO 32, 33, 34, 35 are all ADC1 (safe with WiFi on).
//   Production: 1 MO pull-down to GND on each pin.
//   Fallback (no resistor): GPIO 32/33 use INPUT_PULLDOWN; GPIO 34/35
//   stay floating — software baseline-tracking and rising-edge tests
//   below compensate.
static const uint8_t  PIEZO_PINS[4]    = { 32, 33, 34, 35 };
static const bool     PIEZO_HASPULL[4] = { true, true, false, false };

// ---------- Hit-detection tuning ----------
static const uint16_t TRIG_DELTA       = 800;   // sample > baseline + delta
static const uint16_t FAST_RISE        = 600;   // min single-step jump
static const uint16_t BASELINE_MAX     = 1500;  // clamp baseline so trig <= 4095
static const uint16_t HIT_WINDOW_MS    = 30;    // time to collect peak on all sensors
static const uint16_t DEBOUNCE_MS      = 60;    // gap before next hit can start
static const uint16_t HB_INTERVAL_MS   = 5000;  // heartbeat
static const uint16_t HEALTH_INTERVAL_MS = 5000;// per-sensor health (every 5 s)
static const uint16_t BASELINE_SEED    = 150;
static const uint8_t  BASELINE_SHIFT   = 5;     // EMA: new = old*31/32 + sample/32

#pragma pack(push, 1)
// HitPacketV2: rich hit event with all 4 amplitudes + timestamp.
// Central uses the 4 peaks to triangulate (X, Y) on the target face.
struct HitPacketV2 {
  uint8_t  type;            // 1 = hit
  uint8_t  targetID;
  uint16_t hitSeq;          // monotonic per-target counter
  uint32_t timestampMs;     // target's millis() at hit-window start
  uint16_t peak[4];         // peak amplitude in window for S1..S4
  uint8_t  triggerSensor;   // 1..4 which crossed threshold first
  uint8_t  windowMs;        // diagnostic: actual window length
};

// HeartbeatPacket: 2 = heartbeat (target alive, no hit).
struct HeartbeatPacket {
  uint8_t  type;            // 2 = heartbeat
  uint8_t  targetID;
  uint16_t hitSeq;          // last sequence (helps central detect drops)
};

// HealthPacket: 3 = per-sensor health (baseline + recent peak).
struct HealthPacket {
  uint8_t  type;            // 3 = health
  uint8_t  targetID;
  uint16_t baseline[4];     // current EMA baseline per sensor (ADC counts)
  uint16_t peak[4];         // max sample observed since last health send
};
#pragma pack(pop)

// ---------- Runtime state ----------
enum class HitState : uint8_t { IDLE, CAPTURING, DEBOUNCING };

static HitState  hitState        = HitState::IDLE;
static uint32_t  hitWinStart     = 0;
static uint32_t  hitWinEnd       = 0;          // captureEnd, then debounceEnd
static uint16_t  hitPeak[4]      = {0, 0, 0, 0};
static uint8_t   hitTrigger      = 0;          // 1..4
static uint16_t  hitSeq          = 0;

static uint16_t  baseline[4]     = {BASELINE_SEED, BASELINE_SEED,
                                    BASELINE_SEED, BASELINE_SEED};
static uint16_t  prevSample[4]   = {0, 0, 0, 0};
static uint16_t  peakWindow[4]   = {0, 0, 0, 0}; // for HealthPacket
static uint32_t  lastHeartbeat   = 0;
static uint32_t  lastHealth      = 0;

// ---------- ESP-NOW ----------
// The send-callback signature changed twice in the ESP32 Arduino core:
//   * 2.x and 3.0..3.2:   void(const uint8_t* mac, esp_now_send_status_t)
//   * 3.3+ :              void(const wifi_tx_info_t* info, esp_now_send_status_t)
// We pick the right prototype at compile time so the same source file
// builds cleanly across all three eras (CI also exercises 2.0.17, 3.1.3,
// and 3.3.x).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && \
    (ESP_ARDUINO_VERSION_MAJOR > 3 || \
     (ESP_ARDUINO_VERSION_MAJOR == 3 && ESP_ARDUINO_VERSION_MINOR >= 3))
static void onSent(const wifi_tx_info_t* /*info*/, esp_now_send_status_t status) {
#else
static void onSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
#endif
  digitalWrite(2, status == ESP_NOW_SEND_SUCCESS ? HIGH : LOW);
}

void setup() {
  // Brownout detector is left at its default-enabled setting. Disabling
  // it (which we tried as a workaround for cheap USB cables that dip the
  // 5V rail during WiFi TX) sounds harmless on paper, but in practice
  // when the rail does dip far enough the chip ends up running with
  // partially-corrupted RAM instead of cleanly resetting, producing
  // mysterious panic-handler loops:
  //
  //     Panic handler entered multiple times. Abort panic handling.
  //     PC: 0x20033420  (DRAM, not code)  Backtrace: <-CORRUPTED
  //
  // With BOD enabled, the same bad cable shows up as a clean
  // `E BOD: Brownout detector was triggered` reset that points the user
  // straight at the hardware. The proper production fix is still a
  // beefier 5V supply + bulk cap on the rail.
  Serial.begin(115200);
  pinMode(2, OUTPUT);
  analogReadResolution(12);
  for (int i = 0; i < 4; i++) {
    pinMode(PIEZO_PINS[i], PIEZO_HASPULL[i] ? INPUT_PULLDOWN : INPUT);
  }

  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 1; peer.encrypt = false;
  if (!esp_now_is_peer_exist(RECEIVER_MAC)) esp_now_add_peer(&peer);

  Serial.printf("Target v2 #%u ready. MAC=%s win=%ums dbnc=%ums\n",
                TARGET_ID, WiFi.macAddress().c_str(),
                HIT_WINDOW_MS, DEBOUNCE_MS);
}

// ---------- Packet senders ----------
static void sendHit() {
  HitPacketV2 p{};
  p.type          = 1;
  p.targetID      = TARGET_ID;
  p.hitSeq        = ++hitSeq;
  p.timestampMs   = hitWinStart;
  for (int i = 0; i < 4; i++) p.peak[i] = hitPeak[i];
  p.triggerSensor = hitTrigger;
  p.windowMs      = HIT_WINDOW_MS;
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&p, sizeof(p));
  Serial.printf("Hit T=%u seq=%u ts=%lu trig=S%u peak=[%u,%u,%u,%u]\n",
                TARGET_ID, p.hitSeq, (unsigned long)p.timestampMs,
                p.triggerSensor,
                p.peak[0], p.peak[1], p.peak[2], p.peak[3]);
}

static void sendHeartbeat() {
  HeartbeatPacket h{};
  h.type     = 2;
  h.targetID = TARGET_ID;
  h.hitSeq   = hitSeq;
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&h, sizeof(h));
}

static void sendHealth() {
  HealthPacket h{};
  h.type     = 3;
  h.targetID = TARGET_ID;
  for (int i = 0; i < 4; i++) {
    h.baseline[i] = baseline[i];
    h.peak[i]     = peakWindow[i];
    peakWindow[i] = 0;
  }
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&h, sizeof(h));
  Serial.printf("Health T=%u bl=[%u,%u,%u,%u] peak=[%u,%u,%u,%u]\n",
                TARGET_ID,
                h.baseline[0], h.baseline[1], h.baseline[2], h.baseline[3],
                h.peak[0], h.peak[1], h.peak[2], h.peak[3]);
}

// ---------- Hit-window state machine ----------
// IDLE         : look for first rising-edge trigger on any sensor.
// CAPTURING    : sample all 4 sensors at full speed, track per-sensor peak.
// DEBOUNCING   : ignore triggers; let piezo ringing settle.
static void scanAndUpdate(uint32_t now) {
  for (int i = 0; i < 4; i++) {
    uint16_t v = analogRead(PIEZO_PINS[i]);

    // Always track health-window peak (independent of hit state).
    if (v > peakWindow[i]) peakWindow[i] = v;

    if (hitState == HitState::CAPTURING) {
      // Grab the largest sample seen during the hit window.
      if (v > hitPeak[i]) hitPeak[i] = v;
    } else if (hitState == HitState::IDLE) {
      // Detect a rising edge. Two conditions to reject floating-pin noise:
      //   (a) above clamped baseline + TRIG_DELTA
      //   (b) jumped FAST_RISE in one step from previous sample
      uint16_t bl  = baseline[i];
      uint16_t blc = bl > BASELINE_MAX ? BASELINE_MAX : bl;
      uint16_t trig = blc + TRIG_DELTA;
      int32_t  rise = (int32_t)v - (int32_t)prevSample[i];
      bool fired = (v > trig) && (rise > (int32_t)FAST_RISE);
      if (fired) {
        // Open a hit window; this sensor is the first-trigger.
        hitState     = HitState::CAPTURING;
        hitWinStart  = now;
        hitWinEnd    = now + HIT_WINDOW_MS;
        hitTrigger   = (uint8_t)(i + 1);
        for (int k = 0; k < 4; k++) hitPeak[k] = 0;
        hitPeak[i] = v;
      } else if (v <= trig) {
        // Slow EMA only when comfortably below trigger line.
        baseline[i] = (uint16_t)(((uint32_t)bl * ((1u << BASELINE_SHIFT) - 1) + v)
                                  >> BASELINE_SHIFT);
      }
    }
    // DEBOUNCING: do nothing (don't update baseline either; piezo is still ringing).

    prevSample[i] = v;
  }
}

void loop() {
  uint32_t now = millis();

  scanAndUpdate(now);

  // Advance state machine.
  if (hitState == HitState::CAPTURING && (int32_t)(now - hitWinEnd) >= 0) {
    sendHit();
    // Move into debounce phase using the same hitWinEnd field as the
    // transition target.
    hitState  = HitState::DEBOUNCING;
    hitWinEnd = now + DEBOUNCE_MS;
  } else if (hitState == HitState::DEBOUNCING && (int32_t)(now - hitWinEnd) >= 0) {
    hitState = HitState::IDLE;
    // Refresh prevSample[] so the first IDLE iteration doesn't think the
    // post-ring sample was a fresh rising edge.
    for (int i = 0; i < 4; i++) prevSample[i] = analogRead(PIEZO_PINS[i]);
  }

  if (now - lastHeartbeat > HB_INTERVAL_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
  if (now - lastHealth > HEALTH_INTERVAL_MS) {
    lastHealth = now;
    sendHealth();
  }
}
