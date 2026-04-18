// =======================================================================
//  KIK Shooting Target — Target Node (ESP32)
//  v2.0  (Upgrade: centroid zone, battery, OTA, remote config, ACK)
// -----------------------------------------------------------------------
//  Flash this sketch to every TARGET ESP32 (15 units).
//  Before upload:
//    1. Set TARGET_ID below (1..15)
//    2. Set RECEIVER_MAC to the MAC of the Central Receiver
// =======================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

// ------------------- USER CONFIG (EDIT THESE) --------------------------
static const uint8_t TARGET_ID = 1;                   // 1..15
static const uint8_t RECEIVER_MAC[6] = {
    0xA0, 0xB7, 0x65, 0x12, 0x34, 0x56                // <<< central MAC
};
static const char* OTA_HOSTNAME_PREFIX = "kik-target"; // becomes kik-target-01
static const char* OTA_PASSWORD        = "kik-ota";    // change in production

// Fallback WiFi used only when OTA mode is requested by central.
// When a user wants to OTA, they trigger it from the dashboard, which makes
// the central issue a CMD_OTA_MODE with SSID/PSK in the payload.
// -----------------------------------------------------------------------

// Default runtime config (overridable via CMD_CONFIG from central).
struct Config {
  uint16_t threshold   = 1500;   // ADC threshold for hit detection
  uint16_t debounceMs  = 150;    // per-sensor debounce
  uint16_t hbIntervalMs = 5000;  // heartbeat interval
  uint16_t sensorOkMin = 100;
  uint16_t sensorOkMax = 3000;
  uint8_t  zoneScores[5] = {0, 10, 8, 6, 4};
  uint16_t battDivNum  = 200;    // battery divider: Vbatt_mV = adc * num / den
  uint16_t battDivDen  = 100;    // default 2.00 (100k/100k divider, 3V3 ref)
};
static Config cfg;
static Preferences prefs;

static const int PIEZO_PINS[4] = {32, 33, 34, 35};
static const int BATT_PIN      = 36;  // VP (ADC1_CH0)

// ---- Wire-format packets (MUST match central) -------------------------
#pragma pack(push, 1)
typedef struct {
  uint8_t  type;            // 1 = hit
  uint8_t  targetID;
  uint8_t  sensorID;        // 1..4 (sensor that triggered first)
  uint8_t  hitZone;         // 1..4
  uint16_t hitCount;        // hits so far on sensorID
  uint16_t amplitudes[4];
  uint8_t  score;
  uint8_t  shooterID;       // 0 = unassigned
  uint32_t timestampMs;
  uint32_t packetID;        // for dedup/ack
  int16_t  centroidX;       // -1000..1000 (normalised across width)
  int16_t  centroidY;       // -1000..1000 (normalised across height)
} HitData;

typedef struct {
  uint8_t  type;            // 2 = heartbeat
  uint8_t  targetID;
  uint8_t  sensorStatus[4]; // 0=OK 1=WARNING 2=FAILED
  uint16_t lastReading[4];
  uint32_t uptimeS;
  uint16_t batteryMV;
  int8_t   rssiDbm;
  uint8_t  fwMajor;
  uint8_t  fwMinor;
} HeartbeatData;

typedef struct {
  uint8_t  type;            // 3 = ack (central -> node, echoed back)
  uint8_t  targetID;
  uint32_t packetID;
} AckData;

typedef struct {
  uint8_t  type;            // 4 = command
  uint8_t  commandID;       // see CMD_*
  uint8_t  targetID;        // 0 = broadcast
  uint32_t timestampMs;
  uint8_t  payload[32];
} CommandData;
#pragma pack(pop)

enum : uint8_t {
  PKT_HIT    = 1,
  PKT_HB     = 2,
  PKT_ACK    = 3,
  PKT_CMD    = 4,
};
enum : uint8_t {
  CMD_RESET_COUNTERS = 1,
  CMD_UPDATE_CONFIG  = 2,
  CMD_BUZZ           = 3,
  CMD_OTA_MODE       = 4,
  CMD_PING           = 5,
  CMD_IDENTIFY       = 6,
};

#define FW_MAJOR 2
#define FW_MINOR 0

// ---- State -----------------------------------------------------------
static uint16_t hitCount[4]        = {0, 0, 0, 0};
static uint32_t lastHitMs[4]       = {0, 0, 0, 0};
static uint32_t lastHeartbeatMs    = 0;
static esp_now_peer_info_t peerInfo{};

// Retransmit queue (simple single-slot) for hit packets until ACK received.
static HitData   pendingPkt{};
static bool      pendingValid = false;
static uint32_t  pendingSentMs = 0;
static uint8_t   pendingRetries = 0;
static const uint8_t  MAX_RETRIES   = 4;
static const uint32_t RETRY_EVERY_MS = 120;
static uint32_t nextPacketID = 1;

// ---- Helpers ----------------------------------------------------------
static uint16_t readBatteryMV() {
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(BATT_PIN);
  uint32_t avg = sum / 16;                          // 0..4095
  uint32_t mv  = (avg * 3300UL) / 4095UL;           // ADC mV
  return (uint16_t)((mv * cfg.battDivNum) / cfg.battDivDen);
}

static void loadConfig() {
  prefs.begin("kik", false);
  cfg.threshold    = prefs.getUShort("thr",  cfg.threshold);
  cfg.debounceMs   = prefs.getUShort("deb",  cfg.debounceMs);
  cfg.hbIntervalMs = prefs.getUShort("hb",   cfg.hbIntervalMs);
  cfg.sensorOkMin  = prefs.getUShort("smin", cfg.sensorOkMin);
  cfg.sensorOkMax  = prefs.getUShort("smax", cfg.sensorOkMax);
  for (int i = 0; i < 5; i++) {
    char key[8]; snprintf(key, 8, "z%d", i);
    cfg.zoneScores[i] = prefs.getUChar(key, cfg.zoneScores[i]);
  }
  prefs.end();
}

static void saveConfig() {
  prefs.begin("kik", false);
  prefs.putUShort("thr",  cfg.threshold);
  prefs.putUShort("deb",  cfg.debounceMs);
  prefs.putUShort("hb",   cfg.hbIntervalMs);
  prefs.putUShort("smin", cfg.sensorOkMin);
  prefs.putUShort("smax", cfg.sensorOkMax);
  for (int i = 0; i < 5; i++) {
    char key[8]; snprintf(key, 8, "z%d", i);
    prefs.putUChar(key, cfg.zoneScores[i]);
  }
  prefs.end();
}

static void registerPeer() {
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(RECEIVER_MAC)) esp_now_add_peer(&peerInfo);
}

static void sendHit(const HitData& h) {
  esp_now_send(RECEIVER_MAC, (const uint8_t*)&h, sizeof(h));
}

// ESP-NOW receive callback (command / ack from central)
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < 1) return;
  uint8_t type = data[0];
  if (type == PKT_ACK && len == sizeof(AckData)) {
    AckData a; memcpy(&a, data, sizeof(a));
    if (pendingValid && a.packetID == pendingPkt.packetID) {
      pendingValid = false;
    }
    return;
  }
  if (type == PKT_CMD && len == sizeof(CommandData)) {
    CommandData c; memcpy(&c, data, sizeof(c));
    if (c.targetID != 0 && c.targetID != TARGET_ID) return;
    switch (c.commandID) {
      case CMD_RESET_COUNTERS:
        for (int i = 0; i < 4; i++) { hitCount[i] = 0; lastHitMs[i] = 0; }
        Serial.printf("Target %d: reset\n", TARGET_ID);
        break;
      case CMD_UPDATE_CONFIG: {
        // payload: thr(2) deb(2) hb(2) smin(2) smax(2) z0..z4 (5)  = 15 bytes
        const uint8_t* p = c.payload;
        cfg.threshold    = (p[0] | (p[1] << 8));
        cfg.debounceMs   = (p[2] | (p[3] << 8));
        cfg.hbIntervalMs = (p[4] | (p[5] << 8));
        cfg.sensorOkMin  = (p[6] | (p[7] << 8));
        cfg.sensorOkMax  = (p[8] | (p[9] << 8));
        for (int i = 0; i < 5; i++) cfg.zoneScores[i] = p[10 + i];
        saveConfig();
        Serial.printf("Target %d: config updated\n", TARGET_ID);
        break;
      }
      case CMD_BUZZ:
        // optional local piezo buzz (repurpose GPIO 25 if wired)
        break;
      case CMD_IDENTIFY:
        Serial.printf("Target %d: identify\n", TARGET_ID);
        break;
      case CMD_OTA_MODE: {
        // payload: SSID (up to 20) + "\0" + PSK (rest)
        char ssid[21] = {0};
        char psk[21]  = {0};
        memcpy(ssid, c.payload, 20);
        memcpy(psk,  c.payload + 20, 12);
        Serial.printf("Target %d: entering OTA on %s\n", TARGET_ID, ssid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, psk);
        for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
        char host[32]; snprintf(host, sizeof(host), "%s-%02u", OTA_HOSTNAME_PREFIX, TARGET_ID);
        ArduinoOTA.setHostname(host);
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.begin();
        // Stay in OTA loop for 5 minutes then reboot
        uint32_t t0 = millis();
        while (millis() - t0 < 300000UL) {
          ArduinoOTA.handle();
          delay(10);
        }
        ESP.restart();
        break;
      }
      default: break;
    }
  }
}

static void computeCentroid(const uint16_t a[4], int16_t& cx, int16_t& cy) {
  // Piezo positions (normalised -1..+1):
  //   S1 top-left (-1,+1), S2 top-right (+1,+1)
  //   S3 bot-left (-1,-1), S4 bot-right (+1,-1)
  static const int8_t PX[4] = {-1, +1, -1, +1};
  static const int8_t PY[4] = {+1, +1, -1, -1};
  long num = 0, numY = 0, den = 0;
  for (int i = 0; i < 4; i++) {
    long w = a[i];
    if (w < 50) w = 0;              // ignore noise
    num  += (long)PX[i] * w;
    numY += (long)PY[i] * w;
    den  += w;
  }
  if (den <= 0) { cx = 0; cy = 0; return; }
  cx = (int16_t)((num  * 1000) / den);
  cy = (int16_t)((numY * 1000) / den);
}

static uint8_t zoneFromCentroid(int16_t cx, int16_t cy) {
  // For 10/8/6/4 quadrant scoring the PDF maps zone -> quadrant:
  //   zone 1 = S1 (top-left), zone 2 = S2 (top-right)
  //   zone 3 = S3 (bot-left), zone 4 = S4 (bot-right)
  // We keep compatibility: pick quadrant from centroid.
  if (cx <= 0 && cy >= 0) return 1;
  if (cx >  0 && cy >= 0) return 2;
  if (cx <= 0 && cy <  0) return 3;
  return 4;
}

// ---- Setup / Loop -----------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();

  for (int i = 0; i < 4; i++) pinMode(PIEZO_PINS[i], INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_PIN, ADC_11db);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  registerPeer();
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);

  Serial.printf("KIK Target %u ready (fw %d.%d) MAC %s\n",
      TARGET_ID, FW_MAJOR, FW_MINOR, WiFi.macAddress().c_str());
}

void loop() {
  uint16_t reading[4];
  bool triggered = false;
  int  firstSensor = -1;

  // Scan all 4 piezos very fast
  for (int i = 0; i < 4; i++) {
    reading[i] = analogRead(PIEZO_PINS[i]);
    if (reading[i] > cfg.threshold &&
        (millis() - lastHitMs[i] > cfg.debounceMs)) {
      if (!triggered) { triggered = true; firstSensor = i; }
      lastHitMs[i] = millis();
    }
  }

  if (triggered && firstSensor >= 0) {
    // Quickly sample again to capture peak of the wavefront on each sensor
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < 4; i++) {
        uint16_t r = analogRead(PIEZO_PINS[i]);
        if (r > reading[i]) reading[i] = r;
      }
    }
    int16_t cx, cy;
    computeCentroid(reading, cx, cy);
    uint8_t zone = zoneFromCentroid(cx, cy);

    hitCount[firstSensor]++;

    HitData h{};
    h.type        = PKT_HIT;
    h.targetID    = TARGET_ID;
    h.sensorID    = firstSensor + 1;
    h.hitZone     = zone;
    h.hitCount    = hitCount[firstSensor];
    for (int i = 0; i < 4; i++) h.amplitudes[i] = reading[i];
    h.score       = cfg.zoneScores[zone];
    h.shooterID   = 0;
    h.timestampMs = millis();
    h.packetID    = nextPacketID++;
    h.centroidX   = cx;
    h.centroidY   = cy;

    pendingPkt     = h;
    pendingValid   = true;
    pendingSentMs  = millis();
    pendingRetries = 0;
    sendHit(h);

    Serial.printf(">>> HIT T%u S%u Z%u pts=%u cnt=%u\n",
        TARGET_ID, h.sensorID, zone, h.score, h.hitCount);
  }

  // Retransmit pending hit until ACK
  if (pendingValid && millis() - pendingSentMs > RETRY_EVERY_MS) {
    if (pendingRetries < MAX_RETRIES) {
      sendHit(pendingPkt);
      pendingSentMs = millis();
      pendingRetries++;
    } else {
      pendingValid = false;   // give up; shot is still counted locally
    }
  }

  // Heartbeat
  if (millis() - lastHeartbeatMs > cfg.hbIntervalMs) {
    HeartbeatData hb{};
    hb.type     = PKT_HB;
    hb.targetID = TARGET_ID;
    hb.uptimeS  = millis() / 1000;
    for (int i = 0; i < 4; i++) {
      long sum = 0;
      for (int j = 0; j < 10; j++) { sum += analogRead(PIEZO_PINS[i]); delay(1); }
      uint16_t r = sum / 10;
      hb.lastReading[i] = r;
      if      (r < cfg.sensorOkMin || r > cfg.sensorOkMax) hb.sensorStatus[i] = 2;
      else if (r > 500 && r < 800)                         hb.sensorStatus[i] = 1;
      else                                                  hb.sensorStatus[i] = 0;
    }
    hb.batteryMV = readBatteryMV();
    hb.rssiDbm   = WiFi.RSSI();
    hb.fwMajor   = FW_MAJOR;
    hb.fwMinor   = FW_MINOR;
    esp_now_send(RECEIVER_MAC, (const uint8_t*)&hb, sizeof(hb));
    lastHeartbeatMs = millis();
  }

  delay(3);
}
