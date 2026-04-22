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
static const uint16_t TRIG_DELTA     = 2000;  // trigger when sample > baseline + delta
static const uint16_t DEBOUNCE_MS    = 250;   // per-sensor debounce
static const uint16_t HB_INTERVAL    = 5000;  // heartbeat every 5 s
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
#pragma pack(pop)

static uint16_t hitTotal = 0;
static uint32_t lastFired[4]  = {0, 0, 0, 0};
static uint16_t baseline[4]   = {BASELINE_SEED, BASELINE_SEED,
                                 BASELINE_SEED, BASELINE_SEED};
static uint32_t lastHeartbeat = 0;

static void onSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
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

void loop() {
  uint32_t now = millis();

  int triggered = -1;
  uint16_t peak = 0;
  for (int i = 0; i < 4; i++) {
    uint16_t v = analogRead(PIEZO_PINS[i]);
    // Rising-above-baseline detection (auto-compensates for drift &
    // pin-floating on GPIO 34/35 without hardware pull-down).
    uint16_t bl = baseline[i];
    uint16_t trig = bl + TRIG_DELTA;
    if (v > trig && (now - lastFired[i]) > DEBOUNCE_MS) {
      if (v > peak) { peak = v; triggered = i; }
    } else {
      // Only adapt baseline when we're NOT in the middle of a trigger.
      // Exponential moving average: heavily weighted toward old value.
      baseline[i] = (uint16_t)(((uint32_t)bl * ((1u << BASELINE_SHIFT) - 1) + v)
                               >> BASELINE_SHIFT);
    }
  }
  if (triggered >= 0) {
    lastFired[triggered] = now;
    sendPacket(triggered + 1, peak);
  }

  if (now - lastHeartbeat > HB_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }
}
