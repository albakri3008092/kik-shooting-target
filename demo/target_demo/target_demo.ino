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

// Piezo wiring: one piezo per GPIO, 1 MΩ to GND pull-down.
static const uint8_t  PIEZO_PINS[4] = { 32, 33, 34, 35 };
static const uint16_t THRESHOLD     = 1500;   // ADC threshold for hit
static const uint16_t DEBOUNCE_MS   = 150;    // per-sensor debounce
static const uint16_t HB_INTERVAL   = 5000;   // heartbeat every 5 s

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
static uint32_t lastFired[4] = {0, 0, 0, 0};
static uint32_t lastHeartbeat = 0;

static void onSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
  // Blink built-in LED (GPIO2) on success
  digitalWrite(2, status == ESP_NOW_SEND_SUCCESS ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);                    // status LED
  analogReadResolution(12);
  for (int i = 0; i < 4; i++) pinMode(PIEZO_PINS[i], INPUT);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
      WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 0; peer.encrypt = false;
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
    if (v > THRESHOLD && (now - lastFired[i]) > DEBOUNCE_MS) {
      if (v > peak) { peak = v; triggered = i; }
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
