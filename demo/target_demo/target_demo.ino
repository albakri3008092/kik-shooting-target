// =====================================================================
//  KIK Shooting Target — DEMO (target node, v1 simple)
// =====================================================================
//  Flash this SAME sketch to every target ESP32. Before uploading each
//  board, change only TWO things at the top:
//     1. TARGET_ID      (1, 2, 3, … unique per board)
//     2. RECEIVER_MAC[] (MAC of the central receiver board)
//
//  No external libraries needed — ESP32 core only.
// =====================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- EDIT THESE TWO LINES PER BOARD ----------
#define TARGET_ID    1                                     // 1..15
static uint8_t RECEIVER_MAC[6] = { 0xA0, 0xB7, 0x65, 0x00, 0x00, 0x00 };
// ----------------------------------------------------

// Piezo wiring: one piezo per GPIO.
//   Preferred: 1 MΩ to GND pull-down on each pin (bleeds piezo charge).
//   Demo fallback (no resistor): we enable INPUT_PULLDOWN on GPIO 32/33
//   (which support an internal pull) and rely on software baseline
//   tracking for GPIO 34/35 (input-only pins with no internal pull).
static const uint8_t  PIEZO_PINS[4]  = { 32, 33, 34, 35 };
static const bool     PIEZO_HASPULL[4] = { true, true, false, false };
static const uint16_t TRIG_DELTA     = 800;   // trigger when sample > baseline + delta
static const uint16_t FAST_RISE      = 600;   // minimum single-step jump to count as hit
static const uint16_t BASELINE_MAX   = 1500;  // clamp baseline so trig stays reachable
static const uint16_t DEBOUNCE_MS    = 250;   // per-sensor debounce
static const uint16_t HB_INTERVAL    = 5000;  // heartbeat every 5 s
static const uint16_t HEALTH_INTERVAL = 2000; // per-sensor health every 2 s
static const uint16_t BASELINE_SEED  = 150;   // initial baseline (adc counts)
static const uint8_t  BASELINE_SHIFT = 5;     // EMA: new = old*(2^s-1)/2^s + sample/2^s

// Zone scoring per sensor (S1=10, S2=8, S3=6, S4=4).
static const uint8_t ZONE_SCORES[4] = { 10, 8, 6, 4 };

#pragma pack(push, 1)
struct HitPacket {
  uint8_t  type;        // 1 = hit, 2 = heartbeat
  uint8_t  targetID;
  uint8_t  sensorID;    // 1..4
  uint8_t  zone;        // 1..4
  uint8_t  score;       // 10 / 8 / 6 / 4
  uint16_t total;       // cumulative hits on this target (demo-only)
  uint16_t amp;         // amplitude of first-triggering piezo
};

// Per-sensor health: baseline and running-peak for all 4 piezos.
// Central classifies health from these (see handleEspNow in central).
struct HealthPacket {
  uint8_t  type;        // 3 = health
  uint8_t  targetID;
  uint16_t baseline[4]; // current EMA baseline (ADC counts)
  uint16_t peak[4];     // max sample observed since last health send
};
#pragma pack(pop)

static uint16_t hitTotal = 0;
static uint32_t lastFired[4]  = {0, 0, 0, 0};
static uint16_t baseline[4]   = {BASELINE_SEED, BASELINE_SEED,
                                 BASELINE_SEED, BASELINE_SEED};
static uint16_t peakWindow[4] = {0, 0, 0, 0};  // peak since last health send
static uint16_t prevSample[4] = {0, 0, 0, 0};  // previous ADC sample (for rising-edge)
static uint32_t lastHeartbeat = 0;
static uint32_t lastHealth    = 0;

// The ESP32 Arduino core changed the send-callback signature in 3.3:
//   * 2.x and 3.0..3.2:   void(const uint8_t* mac, esp_now_send_status_t)
//   * 3.3+ :              void(const wifi_tx_info_t* info, esp_now_send_status_t)
// Pick the right prototype at compile time so this sketch builds cleanly
// across all three eras.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && \
    (ESP_ARDUINO_VERSION_MAJOR > 3 || \
     (ESP_ARDUINO_VERSION_MAJOR == 3 && ESP_ARDUINO_VERSION_MINOR >= 3))
static void onSent(const wifi_tx_info_t* /*info*/, esp_now_send_status_t status) {
#else
static void onSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
#endif
  // Blink built-in LED (GPIO2) on success
  digitalWrite(2, status == ESP_NOW_SEND_SUCCESS ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);                    // status LED
  analogReadResolution(12);
  for (int i = 0; i < 4; i++) {
    // GPIO 32, 33 support internal pulldown; 34, 35 are input-only.
    pinMode(PIEZO_PINS[i], PIEZO_HASPULL[i] ? INPUT_PULLDOWN : INPUT);
  }

  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  // Match central: B/G/N only (drop LR so ESP-NOW stays compatible
  // with the non-LR central AP).
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  // Lock STA to channel 1 to match central's softAP channel.
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 1; peer.encrypt = false;
  if (!esp_now_is_peer_exist(RECEIVER_MAC)) esp_now_add_peer(&peer);

  Serial.printf("Target %u ready. MAC=%s\n", TARGET_ID, WiFi.macAddress().c_str());
}

static void sendPacket(uint8_t sensorID, uint16_t amp) {
  HitPacket p{};
  p.type     = 1;
  p.targetID = TARGET_ID;
  p.sensorID = sensorID;
  p.zone     = sensorID;                 // demo: zone == sensor
  p.score    = ZONE_SCORES[sensorID - 1];
  p.total    = ++hitTotal;
  p.amp      = amp;
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&p, sizeof(p));
  Serial.printf("Hit! T=%u S=%u amp=%u total=%u\n",
                p.targetID, p.sensorID, p.amp, p.total);
}

static void sendHeartbeat() {
  HitPacket p{};
  p.type     = 2;
  p.targetID = TARGET_ID;
  p.total    = hitTotal;
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&p, sizeof(p));
}

static void sendHealth() {
  HealthPacket h{};
  h.type     = 3;
  h.targetID = TARGET_ID;
  for (int i = 0; i < 4; i++) {
    h.baseline[i] = baseline[i];
    h.peak[i]     = peakWindow[i];
    peakWindow[i] = 0;                 // reset window
  }
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&h, sizeof(h));
  Serial.printf("Health T=%u bl=[%u,%u,%u,%u] peak=[%u,%u,%u,%u]\n",
                TARGET_ID,
                h.baseline[0], h.baseline[1], h.baseline[2], h.baseline[3],
                h.peak[0], h.peak[1], h.peak[2], h.peak[3]);
}

void loop() {
  uint32_t now = millis();

  int triggered = -1;
  uint16_t peak = 0;
  for (int i = 0; i < 4; i++) {
    uint16_t v = analogRead(PIEZO_PINS[i]);
    if (v > peakWindow[i]) peakWindow[i] = v;   // track max for health report
    // Hit detection combines TWO conditions to reject noise on pins that
    // float (GPIO 34/35 without hardware pull-down):
    //   (a) absolute:  sample > (clamped baseline) + TRIG_DELTA
    //   (b) rising:    sample - prevSample > FAST_RISE
    // A floating pin drifts slowly — it may satisfy (a) but not (b). A
    // real piezo strike produces a sharp millisecond-scale spike, which
    // easily satisfies both.
    uint16_t bl = baseline[i];
    uint16_t blc = bl > BASELINE_MAX ? BASELINE_MAX : bl;
    uint16_t trig = blc + TRIG_DELTA;
    int32_t  rise = (int32_t)v - (int32_t)prevSample[i];
    bool hit = (v > trig) && (rise > (int32_t)FAST_RISE) &&
               ((now - lastFired[i]) > DEBOUNCE_MS);
    if (hit) {
      if (v > peak) { peak = v; triggered = i; }
    } else if (v <= trig) {
      // Only adapt baseline when comfortably below the trigger line.
      // Exponential moving average: heavily weighted toward old value.
      baseline[i] = (uint16_t)(((uint32_t)bl * ((1u << BASELINE_SHIFT) - 1) + v)
                               >> BASELINE_SHIFT);
    }
    prevSample[i] = v;
  }
  if (triggered >= 0) {
    lastFired[triggered] = now;
    sendPacket(triggered + 1, peak);
  }

  if (now - lastHeartbeat > HB_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
  if (now - lastHealth > HEALTH_INTERVAL) {
    lastHealth = now;
    sendHealth();
  }
}
