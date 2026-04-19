// =======================================================================
//  KIK Shooting Target — Central Receiver (ESP32)
//  v2.0
// -----------------------------------------------------------------------
//  Flash this to ONE ESP32 (the 16th). It:
//    * Hosts the "Target_System" WiFi AP
//    * Receives hits over ESP-NOW from 15 target nodes
//    * Serves the full-colour dashboard from LittleFS
//    * Exposes REST + WebSocket API for control + session management
//    * mDNS: reachable as http://kik.local/
//    * OTA for itself (ArduinoOTA)
//    * Buzzer on BUZZER_PIN for hit beep + PAR timer
// =======================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>

// ------------------- USER CONFIG ---------------------------------------
static const char* AP_SSID     = "Target_System";
static const char* AP_PASSWORD = "12345678";

// <<< Replace with the MAC address of each target ESP32 (rows 1..15) >>>
static const uint8_t targetMACs[16][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00},                   // 0 unused
    {0xA0,0xB7,0x65,0x00,0x00,0x01},                   // Target 1
    {0xA0,0xB7,0x65,0x00,0x00,0x02},                   // Target 2
    {0xA0,0xB7,0x65,0x00,0x00,0x03},                   // Target 3
    {0xA0,0xB7,0x65,0x00,0x00,0x04},                   // Target 4
    {0xA0,0xB7,0x65,0x00,0x00,0x05},                   // Target 5
    {0xA0,0xB7,0x65,0x00,0x00,0x06},                   // Target 6
    {0xA0,0xB7,0x65,0x00,0x00,0x07},                   // Target 7
    {0xA0,0xB7,0x65,0x00,0x00,0x08},                   // Target 8
    {0xA0,0xB7,0x65,0x00,0x00,0x09},                   // Target 9
    {0xA0,0xB7,0x65,0x00,0x00,0x0A},                   // Target 10
    {0xA0,0xB7,0x65,0x00,0x00,0x0B},                   // Target 11
    {0xA0,0xB7,0x65,0x00,0x00,0x0C},                   // Target 12
    {0xA0,0xB7,0x65,0x00,0x00,0x0D},                   // Target 13
    {0xA0,0xB7,0x65,0x00,0x00,0x0E},                   // Target 14
    {0xA0,0xB7,0x65,0x00,0x00,0x0F}                    // Target 15
};

static const uint8_t  NUM_TARGETS       = 15;
static const uint32_t HEALTH_TIMEOUT_MS = 15000;
static const int      BUZZER_PIN        = 25;
static const int      BUZZ_CH           = 0;
static const char*    OTA_HOSTNAME      = "kik-central";
static const char*    OTA_PASSWORD      = "kik-ota";
static const char*    MDNS_NAME         = "kik";   // http://kik.local

#define FW_MAJOR 2
#define FW_MINOR 0

// ---- Wire-format packets (MUST match target_node.ino) -----------------
#pragma pack(push, 1)
typedef struct {
  uint8_t  type, targetID, sensorID, hitZone;
  uint16_t hitCount;
  uint16_t amplitudes[4];
  uint8_t  score, shooterID;
  uint32_t timestampMs;
  uint32_t packetID;
  int16_t  centroidX, centroidY;
} HitData;

typedef struct {
  uint8_t  type, targetID;
  uint8_t  sensorStatus[4];
  uint16_t lastReading[4];
  uint32_t uptimeS;
  uint16_t batteryMV;
  int8_t   rssiDbm;
  uint8_t  fwMajor, fwMinor;
} HeartbeatData;

typedef struct {
  uint8_t  type, targetID;
  uint32_t packetID;
} AckData;

typedef struct {
  uint8_t  type, commandID, targetID;
  uint32_t timestampMs;
  uint8_t  payload[32];
} CommandData;
#pragma pack(pop)

enum : uint8_t { PKT_HIT=1, PKT_HB=2, PKT_ACK=3, PKT_CMD=4 };
enum : uint8_t {
  CMD_RESET_COUNTERS=1, CMD_UPDATE_CONFIG=2, CMD_BUZZ=3,
  CMD_OTA_MODE=4, CMD_PING=5, CMD_IDENTIFY=6
};

// ---- Dedup cache (per target) -----------------------------------------
static uint32_t lastSeenPacket[16] = {0};

// ---- Aggregated state -------------------------------------------------
struct TargetState {
  bool     online        = false;
  uint32_t lastSeenMs    = 0;
  uint16_t hits[5]       = {0,0,0,0,0};     // index 1..4
  uint16_t zoneHits[5]   = {0,0,0,0,0};     // index 1..4
  uint32_t score         = 0;
  uint8_t  sensorStatus[5] = {0,0,0,0,0};   // index 1..4
  uint16_t lastReading[5]  = {0,0,0,0,0};
  uint16_t batteryMV     = 0;
  int8_t   rssiDbm       = 0;
  uint8_t  fwMajor = 0, fwMinor = 0;
};
static TargetState T[16];

// ---- Session manager --------------------------------------------------
struct ShotLog {
  uint32_t t;          // ms since session start
  uint8_t  target;
  uint8_t  sensor;
  uint8_t  zone;
  uint8_t  score;
  int16_t  cx, cy;
  uint8_t  shooterID;
};
static const int SHOTLOG_CAP = 1024;
static ShotLog shotLog[SHOTLOG_CAP];
static int     shotLogLen = 0;

struct Session {
  bool       active          = false;
  uint32_t   startedAtMs     = 0;
  uint32_t   parMs           = 0;
  uint32_t   startEpoch      = 0;     // absolute
  char       name[48]        = "";
  char       shooter[48]     = "";
  uint8_t    shooterID       = 0;
  bool       randomStart     = false;
  uint32_t   randomFireMs    = 0;     // when buzzer will sound
  bool       buzzerFired     = false;
};
static Session session;

// ---- WebServer --------------------------------------------------------
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");
AsyncWebSocket  wsInstructor("/instructor");

// ---- Peer info --------------------------------------------------------
static esp_now_peer_info_t peers[16]{};

// ---- Preferences (shooters, config) -----------------------------------
Preferences prefs;

// =======================================================================
//  Helpers
// =======================================================================
static void sendAck(const uint8_t* mac, uint8_t targetID, uint32_t packetID) {
  AckData a{}; a.type = PKT_ACK; a.targetID = targetID; a.packetID = packetID;
  esp_now_send(mac, (const uint8_t*)&a, sizeof(a));
}

static void broadcastCmd(uint8_t cmd, uint8_t targetID, const uint8_t* payload = nullptr, size_t plen = 0) {
  CommandData c{};
  c.type        = PKT_CMD;
  c.commandID   = cmd;
  c.targetID    = targetID;
  c.timestampMs = millis();
  if (payload && plen) memcpy(c.payload, payload, plen > 32 ? 32 : plen);
  if (targetID == 0) {
    for (int i = 1; i <= NUM_TARGETS; i++) {
      esp_now_send(targetMACs[i], (const uint8_t*)&c, sizeof(c));
      delay(5);
    }
  } else {
    esp_now_send(targetMACs[targetID], (const uint8_t*)&c, sizeof(c));
  }
}

// Non-blocking buzzer: we schedule a "buzz off" time and check it in loop().
static uint32_t buzzOffAtMs = 0;
static void buzz(uint32_t durationMs, uint16_t freqHz = 2000) {
  ledcWriteTone(BUZZ_CH, freqHz);
  buzzOffAtMs = millis() + durationMs;
}

static void appendShotLog(const HitData& h) {
  if (!session.active) return;
  if (shotLogLen >= SHOTLOG_CAP) return;
  ShotLog& s = shotLog[shotLogLen++];
  s.t         = millis() - session.startedAtMs;
  s.target    = h.targetID;
  s.sensor    = h.sensorID;
  s.zone      = h.hitZone;
  s.score     = h.score;
  s.cx        = h.centroidX;
  s.cy        = h.centroidY;
  s.shooterID = session.shooterID;
}

// =======================================================================
//  ESP-NOW receive
//  ESP32 Arduino core 2.x vs 3.x have different recv-callback signatures.
//  This shim keeps the sketch compilable on both.
// =======================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onDataReceived(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
#else
static void onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len < 1) return;
  uint8_t type = data[0];

  if (type == PKT_HIT && len == sizeof(HitData)) {
    HitData h; memcpy(&h, data, sizeof(h));
    int t = h.targetID;
    if (t < 1 || t > NUM_TARGETS) return;

    // Always ACK so node stops retransmitting
    sendAck(mac, t, h.packetID);

    if (h.packetID == lastSeenPacket[t] && h.packetID != 0) return;
    lastSeenPacket[t] = h.packetID;

    T[t].online     = true;
    T[t].lastSeenMs = millis();
    T[t].hits[h.sensorID]      = h.hitCount;
    T[t].zoneHits[h.hitZone]  += 1;
    T[t].score                += h.score;

    appendShotLog(h);

    // Local beep on hit (short blip)
    buzz(40, 2500);

    StaticJsonDocument<320> doc;
    doc["type"]    = "hit";
    doc["target"]  = t;
    doc["sensor"]  = h.sensorID;
    doc["zone"]    = h.hitZone;
    doc["count"]   = h.hitCount;
    doc["score"]   = h.score;
    doc["total"]   = T[t].score;
    doc["cx"]      = h.centroidX;
    doc["cy"]      = h.centroidY;
    doc["ts"]      = h.timestampMs;
    if (session.active) {
      doc["sess"]   = (uint32_t)(millis() - session.startedAtMs);
      doc["shooter"]= session.shooter;
    }
    String out; serializeJson(doc, out);
    ws.textAll(out);
    wsInstructor.textAll(out);

  } else if (type == PKT_HB && len == sizeof(HeartbeatData)) {
    HeartbeatData hb; memcpy(&hb, data, sizeof(hb));
    int t = hb.targetID;
    if (t < 1 || t > NUM_TARGETS) return;
    T[t].online     = true;
    T[t].lastSeenMs = millis();
    for (int s = 0; s < 4; s++) {
      T[t].sensorStatus[s+1] = hb.sensorStatus[s];
      T[t].lastReading[s+1]  = hb.lastReading[s];
    }
    T[t].batteryMV = hb.batteryMV;
    T[t].rssiDbm   = hb.rssiDbm;
    T[t].fwMajor   = hb.fwMajor;
    T[t].fwMinor   = hb.fwMinor;

    StaticJsonDocument<512> doc;
    doc["type"]    = "health";
    doc["target"]  = t;
    doc["online"]  = true;
    JsonArray st = doc.createNestedArray("status");
    for (int s = 0; s < 4; s++) st.add(hb.sensorStatus[s]);
    JsonArray rd = doc.createNestedArray("readings");
    for (int s = 0; s < 4; s++) rd.add(hb.lastReading[s]);
    doc["batt"]    = hb.batteryMV;
    doc["rssi"]    = hb.rssiDbm;
    doc["uptime"]  = hb.uptimeS;
    doc["fw"]      = String(hb.fwMajor) + "." + String(hb.fwMinor);
    String out; serializeJson(doc, out);
    ws.textAll(out);
  }
}

// =======================================================================
//  Session persistence
// =======================================================================
static void saveSessionToLittleFS() {
  if (!shotLogLen) return;
  char path[48];
  snprintf(path, sizeof(path), "/sessions/%lu.json", (unsigned long)session.startEpoch);
  LittleFS.mkdir("/sessions");
  File f = LittleFS.open(path, "w");
  if (!f) return;
  f.print("{\"name\":\""); f.print(session.name);
  f.print("\",\"shooter\":\""); f.print(session.shooter);
  f.print("\",\"epoch\":"); f.print(session.startEpoch);
  f.print(",\"par\":"); f.print(session.parMs);
  f.print(",\"shots\":[");
  for (int i = 0; i < shotLogLen; i++) {
    if (i) f.print(',');
    f.printf("[%lu,%u,%u,%u,%u,%d,%d,%u]",
        (unsigned long)shotLog[i].t,
        shotLog[i].target, shotLog[i].sensor, shotLog[i].zone,
        shotLog[i].score, shotLog[i].cx, shotLog[i].cy, shotLog[i].shooterID);
  }
  f.print("]}");
  f.close();
}

// =======================================================================
//  Session control
// =======================================================================
static void startSession(const char* name, const char* shooter, uint8_t shooterID,
                         uint32_t parMs, bool randomStart) {
  session.active       = true;
  session.startedAtMs  = millis();
  session.parMs        = parMs;
  session.shooterID    = shooterID;
  session.randomStart  = randomStart;
  session.buzzerFired  = !randomStart;
  // if random start -> delay 1.5..4.0s before buzzer
  session.randomFireMs = randomStart ? (millis() + 1500 + random(2500)) : millis();
  time_t now = time(nullptr);
  session.startEpoch   = (now > 1000000000) ? (uint32_t)now : millis() / 1000;
  strncpy(session.name,    name    ? name    : "",    sizeof(session.name) - 1);
  strncpy(session.shooter, shooter ? shooter : "",    sizeof(session.shooter) - 1);
  session.name[sizeof(session.name) - 1]       = 0;
  session.shooter[sizeof(session.shooter) - 1] = 0;
  shotLogLen = 0;

  // Reset target counters and our aggregated view
  for (int t = 1; t <= NUM_TARGETS; t++) {
    for (int s = 0; s < 5; s++) { T[t].hits[s] = 0; T[t].zoneHits[s] = 0; }
    T[t].score = 0;
  }
  broadcastCmd(CMD_RESET_COUNTERS, 0);

  StaticJsonDocument<256> doc;
  doc["type"] = "session_start";
  doc["name"] = session.name;
  doc["shooter"] = session.shooter;
  doc["par"]  = parMs;
  doc["random"] = randomStart;
  doc["fireInMs"] = (uint32_t)(session.randomFireMs - millis());
  String out; serializeJson(doc, out);
  ws.textAll(out);
  wsInstructor.textAll(out);
}

static void stopSession() {
  if (!session.active) return;
  session.active = false;
  saveSessionToLittleFS();
  StaticJsonDocument<128> doc;
  doc["type"]  = "session_stop";
  doc["shots"] = shotLogLen;
  doc["epoch"] = session.startEpoch;
  String out; serializeJson(doc, out);
  ws.textAll(out);
  wsInstructor.textAll(out);
}

static void resetAll() {
  for (int t = 1; t <= NUM_TARGETS; t++) {
    for (int s = 0; s < 5; s++) { T[t].hits[s] = 0; T[t].zoneHits[s] = 0; }
    T[t].score = 0;
  }
  shotLogLen = 0;
  broadcastCmd(CMD_RESET_COUNTERS, 0);
  ws.textAll("{\"type\":\"reset\"}");
  wsInstructor.textAll("{\"type\":\"reset\"}");
}

// =======================================================================
//  HTTP handlers
// =======================================================================
static String statusJson() {
  DynamicJsonDocument doc(4096);
  doc["fw"]       = String(FW_MAJOR) + "." + String(FW_MINOR);
  doc["uptime"]   = millis() / 1000;
  JsonObject sess = doc.createNestedObject("session");
  sess["active"]  = session.active;
  sess["name"]    = session.name;
  sess["shooter"] = session.shooter;
  sess["par"]     = session.parMs;
  sess["elapsed"] = session.active ? (uint32_t)(millis() - session.startedAtMs) : 0;
  sess["shots"]   = shotLogLen;
  sess["epoch"]   = session.startEpoch;

  JsonArray arr = doc.createNestedArray("targets");
  for (int t = 1; t <= NUM_TARGETS; t++) {
    JsonObject o = arr.createNestedObject();
    o["id"]      = t;
    o["online"]  = T[t].online;
    o["score"]   = T[t].score;
    JsonArray h  = o.createNestedArray("hits");
    for (int s = 1; s <= 4; s++) h.add(T[t].hits[s]);
    JsonArray zh = o.createNestedArray("zones");
    for (int s = 1; s <= 4; s++) zh.add(T[t].zoneHits[s]);
    JsonArray ss = o.createNestedArray("status");
    for (int s = 1; s <= 4; s++) ss.add(T[t].sensorStatus[s]);
    o["batt"]    = T[t].batteryMV;
    o["rssi"]    = T[t].rssiDbm;
    o["fw"]      = String(T[t].fwMajor) + "." + String(T[t].fwMinor);
  }
  String out; serializeJson(doc, out); return out;
}

static String shootersJson() {
  prefs.begin("kik", true);
  String s = prefs.getString("shooters", "[]");
  prefs.end();
  return s;
}

static void saveShooters(const String& json) {
  prefs.begin("kik", false);
  prefs.putString("shooters", json);
  prefs.end();
}

static void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->text(statusJson());
  } else if (type == WS_EVT_DATA) {
    StaticJsonDocument<512> doc;
    String s((char*)data, len);
    if (deserializeJson(doc, s)) return;
    const char* cmd = doc["command"] | "";
    if (!strcmp(cmd, "reset"))  resetAll();
    else if (!strcmp(cmd, "start_session")) {
      startSession(doc["name"] | "Session", doc["shooter"] | "",
                   doc["shooterID"] | 0, doc["par"] | 0, doc["random"] | false);
    } else if (!strcmp(cmd, "stop_session")) {
      stopSession();
    } else if (!strcmp(cmd, "buzz")) {
      buzz(doc["ms"] | 200, doc["hz"] | 2500);
    } else if (!strcmp(cmd, "identify")) {
      uint8_t tid = doc["target"] | 0;
      broadcastCmd(CMD_IDENTIFY, tid);
    }
  }
}

// =======================================================================
//  Setup
// =======================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BUZZER_PIN, OUTPUT);
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);
  ledcSetup(BUZZ_CH, 2000, 10);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS failed");
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP %s @ %s\n", AP_SSID, ip.toString().c_str());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
  }
  esp_now_register_recv_cb(onDataReceived);
  for (int t = 1; t <= NUM_TARGETS; t++) {
    memcpy(peers[t].peer_addr, targetMACs[t], 6);
    peers[t].channel = 0;
    peers[t].encrypt = false;
    esp_now_add_peer(&peers[t]);
  }
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  // ---- static assets from LittleFS ------------------------------------
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // ---- REST API -------------------------------------------------------
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", statusJson());
  });
  server.on("/api/shooters", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", shootersJson());
  });
  server.on("/api/shooters", HTTP_POST,
    [](AsyncWebServerRequest* req){}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total) {
      String body((char*)data, len);
      saveShooters(body);
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.on("/api/session/start", HTTP_POST,
    [](AsyncWebServerRequest* req){}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total) {
      StaticJsonDocument<256> d;
      if (deserializeJson(d, data, len)) { req->send(400); return; }
      startSession(d["name"] | "Session", d["shooter"] | "",
                   d["shooterID"] | 0, d["par"] | 0, d["random"] | false);
      req->send(200, "application/json", "{\"ok\":true}");
    });
  server.on("/api/session/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    stopSession();
    req->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/session/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    resetAll();
    req->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/sessions", HTTP_GET, [](AsyncWebServerRequest* req) {
    String out = "[";
    File root = LittleFS.open("/sessions");
    bool first = true;
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        if (!first) out += ",";
        out += "{\"file\":\""; out += f.name(); out += "\",\"size\":"; out += f.size(); out += "}";
        first = false;
        f = root.openNextFile();
      }
    }
    out += "]";
    req->send(200, "application/json", out);
  });
  server.on("/api/config/push", HTTP_POST,
    [](AsyncWebServerRequest* req){}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total) {
      StaticJsonDocument<256> d;
      if (deserializeJson(d, data, len)) { req->send(400); return; }
      uint8_t pl[32] = {0};
      uint16_t thr  = d["threshold"]  | 1500;
      uint16_t deb  = d["debounce"]   | 150;
      uint16_t hb   = d["heartbeat"]  | 5000;
      uint16_t smin = d["sensorMin"]  | 100;
      uint16_t smax = d["sensorMax"]  | 3000;
      pl[0]=thr&0xFF; pl[1]=thr>>8; pl[2]=deb&0xFF; pl[3]=deb>>8;
      pl[4]=hb&0xFF;  pl[5]=hb>>8;  pl[6]=smin&0xFF;pl[7]=smin>>8;
      pl[8]=smax&0xFF;pl[9]=smax>>8;
      pl[10]=0;
      pl[11] = d["z1"] | 10;
      pl[12] = d["z2"] | 8;
      pl[13] = d["z3"] | 6;
      pl[14] = d["z4"] | 4;
      uint8_t target = d["target"] | 0;
      broadcastCmd(CMD_UPDATE_CONFIG, target, pl, 15);
      req->send(200, "application/json", "{\"ok\":true}");
    });

  ws.onEvent(onWsEvent);
  wsInstructor.onEvent([](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t, void* a, uint8_t* d, size_t l){
    if (t == WS_EVT_CONNECT) c->text(statusJson());
  });
  server.addHandler(&ws);
  server.addHandler(&wsInstructor);
  server.begin();

  Serial.println("=== CENTRAL RECEIVER READY ===");
  Serial.printf("WiFi: %s / %s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Dashboard: http://%s/  (or http://%s.local/)\n",
                ip.toString().c_str(), MDNS_NAME);
}

// =======================================================================
//  Loop
// =======================================================================
static uint32_t lastHealthBroadcast = 0;

void loop() {
  ArduinoOTA.handle();
  ws.cleanupClients();
  wsInstructor.cleanupClients();

  uint32_t now = millis();

  // Turn buzzer off when scheduled
  if (buzzOffAtMs && now >= buzzOffAtMs) {
    ledcWriteTone(BUZZ_CH, 0);
    buzzOffAtMs = 0;
  }

  // Random-start buzzer
  if (session.active && session.randomStart && !session.buzzerFired && now >= session.randomFireMs) {
    buzz(400, 1200);
    session.buzzerFired = true;
    session.startedAtMs = now;   // timer starts at buzzer
    StaticJsonDocument<64> d; d["type"] = "timer_go"; String s; serializeJson(d, s);
    ws.textAll(s); wsInstructor.textAll(s);
  }

  // PAR time beep
  static uint32_t parBeepAt = 0;
  if (session.active && session.parMs && session.buzzerFired) {
    uint32_t parHit = session.startedAtMs + session.parMs;
    if (parBeepAt != parHit && now >= parHit) {
      buzz(600, 800);
      parBeepAt = parHit;
      ws.textAll("{\"type\":\"par\"}");
    }
  }

  // Online/offline polling + periodic health broadcast every 1s
  if (now - lastHealthBroadcast > 1000) {
    lastHealthBroadcast = now;
    for (int t = 1; t <= NUM_TARGETS; t++) {
      bool wasOnline = T[t].online;
      T[t].online = (now - T[t].lastSeenMs) < HEALTH_TIMEOUT_MS && T[t].lastSeenMs > 0;
      if (wasOnline != T[t].online) {
        StaticJsonDocument<96> d;
        d["type"] = "online"; d["target"] = t; d["online"] = T[t].online;
        String s; serializeJson(d, s);
        ws.textAll(s);
      }
    }
    // broadcast timer tick while active
    if (session.active) {
      StaticJsonDocument<128> d;
      d["type"]    = "tick";
      d["elapsed"] = (uint32_t)(now - session.startedAtMs);
      d["shots"]   = shotLogLen;
      String s; serializeJson(d, s);
      ws.textAll(s);
    }
  }

  delay(2);
}
