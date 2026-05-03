// =====================================================================
//  KIK Shooting Target — PROTOTYPE V3 (central receiver)
// =====================================================================
//  V3 base = V2 stable (commit 6753cef). Receiver / scoring logic is
//  identical to V2 stable: ESP-NOW listener for HitPacketV2 +
//  HeartbeatPacket + HealthPacket, ISSF zone scoring, calibration,
//  CSV log over SoftAP at 192.168.4.1.
//
//  V3 changes are dashboard-only: the embedded HTML/CSS/JS at
//  http://192.168.4.1/ is being redesigned for tablet use (less
//  cluttered, larger touch targets, clearer OK / MATI status). The
//  redesign lands in follow-up commits on this branch as the user's
//  wireframe is finalised.
// =====================================================================
//  Improvements over demo (v1):
//    1. Receives HitPacketV2 with all 4 sensor amplitudes per hit, then
//       triangulates an (X, Y) impact point on the target face using a
//       weighted centroid of the per-sensor peaks. Each target is mapped
//       in the [-1, +1] x [-1, +1] normalized square; the dashboard
//       paints dots over a 5-ring concentric target visualization.
//    2. Split-time tracking: every consecutive pair of hits on the same
//       target produces a "split" in milliseconds. The dashboard shows
//       the most recent split plus the running average — useful for
//       tactical / double-tap evaluation.
//    3. Score zones use a simple 5-ring concentric layout (5..1 marks)
//       computed from the distance from center, not the demo's static
//       S1=10 / S2=8 mapping. Each hit's zone + score is included in the
//       ring buffer and the per-target running total.
//    4. Sensor health monitoring kept from demo (HealthPacket type 3),
//       with the same OK / NOISY / BROKEN / UNKNOWN classification.
//
//  No external libraries required (ESP32 core only).
//  Embedded dashboard HTML is served from / over the AP at 192.168.4.1.
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---------- Config ----------
// Bumped to 15 to support a full multi-lane range. ESP-NOW receive does
// not require pre-registering peers, so the central can accept hits from
// any of the 15 target boards as long as their TARGET_ID is in [1..15]
// and their RECEIVER_MAC matches this board's softAP MAC.
#define NUM_TARGETS       15
// Ring buffer of recent hits shown in the feed. With 15 targets firing
// in parallel during a string, 24 was too short; bump so the feed shows
// roughly the last 4-5 shots per target on average.
#define RECENT_N          48
static const char*  AP_SSID     = "Target_System_V2";
static const char*  AP_PASSWORD = "12345678";
static const uint8_t BUZZER_PIN = 25;
static const int     BUZZ_CH    = 0;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define KIK_BUZZ_SETUP(pin, ch, f, res) ledcAttach((pin), (f), (res))
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((pin), (f))
#else
  #define KIK_BUZZ_SETUP(pin, ch, f, res) do { ledcSetup((ch), (f), (res)); ledcAttachPin((pin), (ch)); } while (0)
  #define KIK_BUZZ_TONE(pin, ch, f)       ledcWriteTone((ch), (f))
#endif

// Sensor positions in the normalized [-1, +1] target square.
//   S1 top-left  S2 top-right
//   S3 bot-left  S4 bot-right
// Triangulation is a peak-weighted centroid in this space.
static const float SENSOR_X[4] = { -1.f, +1.f, -1.f, +1.f };
static const float SENSOR_Y[4] = { +1.f, +1.f, -1.f, -1.f };

// 5-zone target ring radii (outer edge) in normalized [0, 1].
// Centre is 5 marks, each next ring decrements by 1 down to 1 at the edge.
// The 6th index is "miss" (off-target) and scores 0.
static const uint8_t NUM_RINGS = 5;
static const float RING_R[NUM_RINGS] = {
  0.20f, // 5 marks (centre)
  0.40f, // 4 marks
  0.60f, // 3 marks
  0.80f, // 2 marks
  1.30f, // 1 mark  (extreme outer; anything beyond is a miss)
};
static const uint8_t RING_SCORE[NUM_RINGS] = { 5, 4, 3, 2, 1 };

// ---------- Stage 5: per-sensor gain calibration ----------
// Each target stores 4 reference peak values (one per sensor) representing
// the response when a known shot lands directly above that sensor (corner).
// During triangulation, raw peaks are normalized: peak[i] / refPeak[i]
// before the weighted-centroid is computed. This compensates for piezos
// of differing sensitivity, varying distance to mounting points, etc.
//
// Reference peaks are persisted in NVS Preferences so they survive reboots.
// A value of 0 means "uncalibrated" — fall back to raw peaks.
struct Calibration {
  bool     valid;
  uint16_t refPeak[4];
};

// Calibration UX state machine: when active, the central intercepts hits
// for the chosen target and records peak[step] (peak of the sensor we're
// asking the user to tap directly). Hits in this state do NOT advance the
// target's hit count or score — they are training samples only.
enum class CalState : uint8_t { IDLE, ACTIVE };

// ---------- Stage 6: SPIFFS session log ----------
// Every accepted hit is queued (FreeRTOS) and drained in loop() into
// /session.csv. The HTTP server can stream the file at /log.csv. We use a
// queue rather than direct writes from onEspNow because SPIFFS writes can
// occasionally block for tens of milliseconds (wear levelling) — we don't
// want to stall the ESP-NOW receive callback.
// Drained in loop() at up to 4 writes per iteration. Sized for ~5 hits/s
// across 15 targets bursty for a few seconds without dropping events.
#define LOG_QUEUE_LEN  128
static const char* LOG_PATH = "/session.csv";
static const char* LOG_HEADER =
  "ts_ms,target,trigger,zone,score,x,y,split_ms,peak1,peak2,peak3,peak4\n";

#pragma pack(push, 1)
// MUST match target_v2.ino HitPacketV2.
struct HitPacketV2 {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t hitSeq;
  uint32_t timestampMs;
  uint16_t peak[4];
  uint8_t  triggerSensor;
  uint8_t  windowMs;
};
struct HeartbeatPacket {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t hitSeq;
};
struct HealthPacket {
  uint8_t  type;
  uint8_t  targetID;
  uint16_t baseline[4];
  uint16_t peak[4];
};
#pragma pack(pop)

// ---------- Per-target state ----------
struct TargetState {
  bool     online        = false;
  uint32_t lastSeen      = 0;
  uint32_t lastHitMs     = 0;
  uint16_t hitCount      = 0;
  uint32_t scoreSum      = 0;     // running total marks (5+4+3+2+1 zones)
  uint16_t lastHitSeq    = 0;
  uint8_t  lastTrigger   = 0;     // 1..4
  uint8_t  lastZone      = 0;     // 0=5pt centre, 1=4pt, ... 4=1pt, 5=miss
  uint8_t  lastScore     = 0;
  float    lastX         = 0.f;
  float    lastY         = 0.f;
  uint16_t lastPeak[4]   = {0, 0, 0, 0};

  // Split-time tracking (between consecutive hits on this target).
  uint16_t lastSplitMs   = 0;     // most recent split
  uint16_t minSplitMs    = 0;     // best (smallest) split observed
  uint32_t splitSumMs    = 0;     // running sum for average
  uint16_t splitCount    = 0;     // number of splits recorded

  // Per-sensor health (HealthPacket).
  uint32_t lastHealthMs  = 0;
  uint16_t baseline[4]   = {0, 0, 0, 0};
  uint16_t peak[4]       = {0, 0, 0, 0};

  // Hit-based dead-sensor detection. A wire that is broken/disconnected
  // produces baseline ~0 (internal pull-down or external 1 MΩ holds the
  // line at GND), which is electrically indistinguishable from a working
  // but idle piezo. The HealthPacket alone therefore cannot tell the two
  // apart. But during an actual shot, a working piezo always picks up
  // SOME mechanical vibration — even if it isn't the trigger sensor, the
  // impact propagates through the board and registers a peak well above
  // baseline (~50–500 ADC counts). A broken sensor stays flat. We track
  // a per-sensor streak: how many consecutive recent hits this sensor
  // failed to register any meaningful response. After DEAD_STREAK_N
  // silent hits (with at least DEAD_MIN_HITS total hits to bootstrap),
  // we surface the sensor as "DEAD" in the dashboard so the user can
  // physically inspect the wire.
  uint8_t  deadStreak[4] = {0, 0, 0, 0};
  uint16_t hitsForHealth = 0;   // saturating counter, used as bootstrap

  // Idle dead-sensor detection. The hit-based check above only fires when
  // the user is actually shooting/tapping the board. In a freshly-built
  // prototype where only one piezo is wired, cutting that piezo means
  // NO sensor crosses the trigger threshold and the central never sees
  // a HitPacketV2 at all — so the hit-based streak never advances.
  //
  // We can't reliably tell a cut wire from a quiet, *connected* piezo
  // by looking at one sensor alone: a piezo on a properly pulled-down
  // pin reads ~0 ADC at rest just like an open wire. We CAN tell them
  // apart by looking at *the other sensors on the same target* in the
  // same 5-second Health window. If even one other sensor saw real
  // activity (peak >= SILENT_ACTIVITY_THRESH) but this one stayed at
  // the floor, the most likely explanation is that this sensor is
  // disconnected. After SILENT_IN_ACTIVE_STREAK_N such windows we
  // surface it as "dead". Idle target (no movement at all on any
  // sensor) leaves the streak untouched, so an undisturbed target
  // doesn't false-flag every sensor as MATI.
  uint8_t  silentInActiveStreak[4] = {0, 0, 0, 0};
};
static TargetState T[NUM_TARGETS + 1];   // index 1..NUM_TARGETS

// Sensor-health classification thresholds. Declared up here (rather
// than next to sensorHealthLabel below) so onEspNow can reference
// them while updating the per-sensor streak.
static const uint8_t  DEAD_STREAK_N             = 3;  // silent hits in a row
static const uint16_t DEAD_MIN_HITS             = 3;  // need >= this many hits to trust streak
// SILENT_ACTIVITY_THRESH: peak[] (in ADC counts) at or above this
// inside a 5-second Health window means "this sensor saw real activity
// in the window". 100 ~= 2.4 % of 12-bit full-scale: well above the
// open-wire / EMI noise floor (typically <30) but well under the peaks
// produced by an actual hit (>= TRIG_DELTA = 800 plus baseline) or
// even by a board nudge picked up by a connected piezo (peak ~ 200+).
//
// SILENT_IN_ACTIVE_STREAK_N: how many consecutive Health windows we
// require where (a) at least one OTHER sensor on this target saw
// activity, and (b) THIS sensor stayed below the threshold, before
// surfacing the sensor as MATI. 1 window = a single ~5 s active
// window with this sensor silent is enough; tuned for the demo where
// snappy MATI feedback after one tap on the board matters more than
// surviving an isolated dropped Health packet. The differential
// nature of the check (we require *another* sensor to have crossed
// SILENT_ACTIVITY_THRESH in the same window) keeps it from
// false-flagging idle targets even at this aggressive setting.
// Idle targets (no sensor active anywhere) leave every streak alone,
// so an undisturbed target stays "ok".
static const uint16_t SILENT_ACTIVITY_THRESH    = 100;
static const uint8_t  SILENT_IN_ACTIVE_STREAK_N = 1;

// ---------- Recent hits ring buffer ----------
struct RecentHit {
  uint32_t ts;
  uint8_t  targetID;
  uint8_t  triggerSensor;     // 1..4
  uint8_t  zone;              // 0=5pt centre, 1=4pt, ... 4=1pt, 5=miss
  uint8_t  score;             // 0..5 marks
  int16_t  x10;               // X * 1000 (normalized -1000..+1000)
  int16_t  y10;               // Y * 1000
  uint16_t splitMs;           // split from previous hit on same target
};
static RecentHit recentBuf[RECENT_N] = {};
static uint8_t   recentHead  = 0;
static uint16_t  recentCount = 0;

// Stage 5 globals.
static Calibration  gCal[NUM_TARGETS + 1];        // index 1..NUM_TARGETS
static CalState     gCalState  = CalState::IDLE;
static uint8_t      gCalTarget = 0;               // 1..NUM_TARGETS
static uint8_t      gCalStep   = 0;               // 0..3
static uint16_t     gCalCapture[4] = {0, 0, 0, 0};// peaks captured this run
static Preferences  gPrefs;
// Deferred-commit handoff from the ESP-NOW callback (WiFi task) to the
// main loop. NVS Preferences (`gPrefs`) is not safe to share across
// tasks: a flash write from the WiFi task while an HTTP handler holds
// `gPrefs.begin()` on the main loop can corrupt the internal NVS handle.
// We avoid that by only ever calling Preferences from one task — the
// main loop. When calibration completes inside `onEspNow` we copy the
// 4 captured peaks here, set `gCalCommitPending`, and let `loop()`
// perform the actual gCal write + NVS save.
static volatile bool gCalCommitPending = false;
static uint8_t       gCalCommitTarget  = 0;
static uint16_t      gCalCommitRef[4]  = {0, 0, 0, 0};

// Stage 6 globals.
struct LogEntry {
  uint32_t ts;
  uint8_t  targetID;
  uint8_t  triggerSensor;
  uint8_t  zone;
  uint8_t  score;
  int16_t  x10;
  int16_t  y10;
  uint16_t splitMs;
  uint16_t peak[4];
};
static QueueHandle_t gLogQueue = nullptr;
static volatile uint32_t gLogLines     = 0;       // approx; ground-truth on disk
static volatile bool     gSpiffsReady  = false;

static WebServer server(80);
static volatile bool gHitPending = false;

// =====================================================================
//  Triangulation + scoring
// =====================================================================
// Peak-weighted centroid in the normalized [-1, +1] square. Returns
// (X, Y) and the magnitude r = sqrt(X^2 + Y^2) for ring lookup. When the
// target has stored Calibration (Stage 5), each per-sensor peak is first
// divided by its reference peak so that all 4 sensors contribute on a
// comparable scale regardless of intrinsic sensitivity differences.
static void triangulate(uint8_t targetID, const uint16_t peak[4],
                        float& outX, float& outY) {
  const Calibration& cal = (targetID >= 1 && targetID <= NUM_TARGETS)
                              ? gCal[targetID]
                              : gCal[0];   // sentinel: invalid -> no cal
  float w[4];
  float total = 0.f;
  for (int i = 0; i < 4; i++) {
    float p = (float)peak[i];
    if (cal.valid && cal.refPeak[i] > 0) {
      // Normalize to "fraction of this sensor's calibrated max response".
      p = p / (float)cal.refPeak[i];
    }
    w[i]  = p;
    total += p;
  }
  if (total <= 0.f) { outX = 0.f; outY = 0.f; return; }
  float fx = 0.f, fy = 0.f;
  for (int i = 0; i < 4; i++) {
    float r = w[i] / total;
    fx += r * SENSOR_X[i];
    fy += r * SENSOR_Y[i];
  }
  // Empirical gain: a perfectly centered hit puts equal weight on all 4
  // sensors so the centroid sits at (0, 0); a corner hit ~+/-0.6. Apply
  // a 1.6x stretch so the visualization fills the target area.
  const float kStretch = 1.6f;
  fx *= kStretch;
  fy *= kStretch;
  if (fx >  1.3f) fx =  1.3f;
  if (fx < -1.3f) fx = -1.3f;
  if (fy >  1.3f) fy =  1.3f;
  if (fy < -1.3f) fy = -1.3f;
  outX = fx;
  outY = fy;
}

// =====================================================================
//  Stage 5: calibration helpers
// =====================================================================
// Build NVS key for a single uint16 reference peak: "T<id>S<sensor>".
// (Preferences keys are limited to 15 chars; this fits.)
static String calKey(uint8_t targetID, uint8_t sensorIdx) {
  String k = "T"; k += targetID; k += "S"; k += sensorIdx; return k;
}

static void loadCalibration() {
  // Open the "kikcal" namespace read-only first; Preferences will create
  // it lazily on the first save.
  if (!gPrefs.begin("kikcal", /*readOnly=*/true)) {
    // No namespace yet; everything stays uncalibrated.
    for (int t = 0; t <= NUM_TARGETS; t++) {
      gCal[t].valid = false;
      for (int i = 0; i < 4; i++) gCal[t].refPeak[i] = 0;
    }
    return;
  }
  for (int t = 1; t <= NUM_TARGETS; t++) {
    bool any = false;
    for (int i = 0; i < 4; i++) {
      uint16_t v = gPrefs.getUShort(calKey((uint8_t)t, (uint8_t)i).c_str(), 0);
      gCal[t].refPeak[i] = v;
      if (v > 0) any = true;
    }
    gCal[t].valid = any;
  }
  gPrefs.end();
}

static void saveCalibrationFor(uint8_t targetID) {
  if (!gPrefs.begin("kikcal", /*readOnly=*/false)) return;
  for (int i = 0; i < 4; i++) {
    gPrefs.putUShort(calKey(targetID, (uint8_t)i).c_str(),
                     gCal[targetID].refPeak[i]);
  }
  gPrefs.end();
}

static void clearCalibrationFor(uint8_t targetID) {
  if (gPrefs.begin("kikcal", /*readOnly=*/false)) {
    for (int i = 0; i < 4; i++) {
      gPrefs.remove(calKey(targetID, (uint8_t)i).c_str());
    }
    gPrefs.end();
  }
  gCal[targetID].valid = false;
  for (int i = 0; i < 4; i++) gCal[targetID].refPeak[i] = 0;
}

// =====================================================================
//  Stage 6: log helpers
// =====================================================================
static void logResetFile() {
  if (!gSpiffsReady) return;
  // Re-create the file with just the header line.
  File f = SPIFFS.open(LOG_PATH, FILE_WRITE);
  if (!f) return;
  f.print(LOG_HEADER);
  f.close();
  gLogLines = 0;
}

static void logFlushOne(const LogEntry& e) {
  if (!gSpiffsReady) return;
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  // Use printf for compactness; SPIFFS_FILE has print() that supports it.
  f.printf("%lu,%u,%u,%u,%u,%.3f,%.3f,%u,%u,%u,%u,%u\n",
           (unsigned long)e.ts,
           (unsigned)e.targetID,
           (unsigned)e.triggerSensor,
           (unsigned)e.zone,
           (unsigned)e.score,
           e.x10 / 1000.f,
           e.y10 / 1000.f,
           (unsigned)e.splitMs,
           (unsigned)e.peak[0], (unsigned)e.peak[1],
           (unsigned)e.peak[2], (unsigned)e.peak[3]);
  f.close();
  gLogLines++;
}

// Map (X, Y) -> zone index (0=centre 5pt, 1=4pt, 2=3pt, 3=2pt, 4=1pt) and
// score. zoneIdx == NUM_RINGS means "off target" (miss, 0 marks).
static void scoreZone(float x, float y, uint8_t& zoneIdx, uint8_t& score) {
  float r = sqrtf(x * x + y * y);
  for (int i = 0; i < NUM_RINGS; i++) {
    if (r <= RING_R[i]) {
      zoneIdx = (uint8_t)i;
      score   = RING_SCORE[i];
      return;
    }
  }
  zoneIdx = NUM_RINGS;   // off target
  score   = 0;
}

// =====================================================================
//  Dashboard (single-file HTML + CSS + JS, polls /status)
// =====================================================================
static const char INDEX_HTML[] PROGMEM = R"RAW(
<!doctype html>
<html lang="ms">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>KIK Target V3</title>
<style>
  :root{
    --bg:#0b1221;--surf:#162036;--surf2:#1e2b4a;--bd:#263556;
    --tx:#f4f6fb;--mut:#8896b8;
    --p:#00e5a8;--i:#4cc9f0;--a:#b56cff;--d:#ff4d6d;--w:#ffd43b;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  html,body{height:100%}
  body{
    font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    background:radial-gradient(1200px 500px at 0% -10%,rgba(0,229,168,.14),transparent 60%),
               radial-gradient(1000px 500px at 100% 0%,rgba(76,201,240,.10),transparent 60%),
               var(--bg);
    color:var(--tx);min-height:100vh;padding:18px;
  }
  /* ---------- Header ---------- */
  header{
    display:flex;align-items:center;gap:14px;margin-bottom:18px;flex-wrap:wrap;
  }
  .logo{
    width:48px;height:48px;border-radius:14px;
    background:linear-gradient(135deg,var(--p),var(--i));
    display:grid;place-items:center;font-weight:900;font-size:21px;color:#0b1221;
  }
  h1{font-size:20px;letter-spacing:.4px}
  h1 small{color:var(--mut);font-weight:500;font-size:12px;display:block;margin-top:2px}
  .pill{
    margin-left:auto;display:inline-flex;align-items:center;gap:8px;
    padding:8px 14px;border:1px solid var(--bd);border-radius:999px;
    background:rgba(255,255,255,.04);font-size:13px;color:var(--mut);
  }
  .dot{width:9px;height:9px;border-radius:50%;background:var(--mut)}
  .dot.on{background:var(--p);box-shadow:0 0 12px var(--p)}
  /* ---------- Hero card ---------- */
  .hero{
    background:var(--surf);border:1px solid var(--bd);border-radius:20px;
    padding:22px;margin-bottom:18px;
  }
  .hero-stats{
    display:grid;grid-template-columns:1fr 2fr;gap:18px;
    padding-bottom:18px;margin-bottom:18px;border-bottom:1px solid var(--bd);
  }
  .hs{
    background:var(--surf2);border:1px solid var(--bd);border-radius:16px;
    padding:18px 22px;display:flex;flex-direction:column;justify-content:center;
  }
  .hs .lbl{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.8px}
  .hs .num{font-size:48px;font-weight:900;line-height:1.05;margin-top:6px}
  .hs .num small{font-size:20px;color:var(--mut);font-weight:600;margin-left:4px}
  .hs.big{background:linear-gradient(135deg,rgba(0,229,168,.18),rgba(76,201,240,.10));
          border-color:rgba(0,229,168,.4)}
  .hs.big .num{font-size:72px;color:var(--p)}
  /* ---------- Tile grid ---------- */
  .tile-label{
    font-size:13px;color:var(--mut);text-transform:uppercase;letter-spacing:.7px;
    margin-bottom:10px;
  }
  .tile-label small{font-weight:500;text-transform:none;letter-spacing:0;margin-left:6px}
  .tile-grid{
    display:grid;grid-template-columns:repeat(5,1fr);gap:10px;
  }
  @media (max-width:720px){
    .tile-grid{grid-template-columns:repeat(3,1fr)}
    .hero-stats{grid-template-columns:1fr;gap:12px}
    .hs.big .num{font-size:56px}
  }
  .tile{
    position:relative;background:var(--surf2);border:2px solid var(--bd);
    border-radius:14px;padding:14px 8px;text-align:center;cursor:pointer;
    transition:transform .12s ease,border-color .12s ease,background .12s ease;
    user-select:none;-webkit-tap-highlight-color:transparent;
    min-height:96px;display:flex;flex-direction:column;justify-content:center;gap:4px;
  }
  .tile:hover{transform:translateY(-2px)}
  .tile:active{transform:translateY(0)}
  .tile .tnum{font-size:13px;color:var(--mut);font-weight:700;letter-spacing:.5px}
  .tile .thits{font-size:32px;font-weight:900;line-height:1}
  .tile .tlast{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px;
               margin-top:2px;min-height:12px}
  .tile.online{border-color:rgba(0,229,168,.45);background:rgba(0,229,168,.06)}
  .tile.online .thits{color:var(--p)}
  .tile.offline{opacity:.42}
  .tile.offline .thits{color:var(--mut)}
  .tile.dead{
    border-color:rgba(255,77,109,.55);background:rgba(255,77,109,.10);
    animation:tileBlink 1.6s ease-in-out infinite;
  }
  .tile.dead .thits{color:var(--d)}
  @keyframes tileBlink{50%{background:rgba(255,77,109,.22)}}
  .tile-legend{
    display:flex;gap:18px;flex-wrap:wrap;margin-top:14px;
    font-size:11px;color:var(--mut);
  }
  .tile-legend .lg{display:inline-block;width:9px;height:9px;border-radius:50%;
                   margin-right:6px;vertical-align:middle}
  .tile-legend .lg.ok{background:var(--p);box-shadow:0 0 6px var(--p)}
  .tile-legend .lg.off{background:var(--mut)}
  .tile-legend .lg.dead{background:var(--d);box-shadow:0 0 6px var(--d)}
  /* ---------- Modal ---------- */
  .modal{
    position:fixed;inset:0;background:rgba(11,18,33,.78);backdrop-filter:blur(4px);
    display:flex;align-items:center;justify-content:center;padding:18px;z-index:50;
  }
  .modal.hidden{display:none}
  .modal-card{
    background:var(--surf);border:1px solid var(--bd);border-radius:20px;
    padding:22px;width:100%;max-width:480px;
  }
  .modal-head{display:flex;align-items:center;gap:12px;margin-bottom:18px}
  .modal-title{font-size:22px;font-weight:900;flex:1}
  .modal-title small{color:var(--mut);font-size:12px;font-weight:500;display:block;margin-top:2px}
  .close{
    background:var(--surf2);border:1px solid var(--bd);color:var(--tx);
    width:36px;height:36px;border-radius:10px;font-size:22px;font-weight:700;
    cursor:pointer;line-height:1;
  }
  .close:hover{background:var(--bd)}
  .m-stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
  .m-stat{background:var(--surf2);border:1px solid var(--bd);border-radius:12px;padding:14px}
  .m-stat .lbl{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px}
  .m-stat .v{font-size:28px;font-weight:900;margin-top:4px}
  .m-stat.span2{grid-column:span 2}
  .health-row{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:14px}
  .hp{
    border-radius:12px;border:2px solid var(--bd);padding:10px 4px;
    text-align:center;background:rgba(255,255,255,.02);
  }
  .hp .lbl{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
  .hp .st{font-size:14px;font-weight:900;letter-spacing:.4px;margin-top:4px}
  .hp.ok    {border-color:rgba(0,229,168,.4)}
  .hp.ok    .st{color:var(--p)}
  .hp.noisy {border-color:rgba(255,212,59,.4)}
  .hp.noisy .st{color:var(--w)}
  .hp.broken{border-color:rgba(255,77,109,.4)}
  .hp.broken .st{color:var(--d);animation:blink 1s steps(2) infinite}
  .hp.dead  {border-color:rgba(255,77,109,.55);background:rgba(255,77,109,.08)}
  .hp.dead  .st{color:var(--d);animation:blink 1s steps(2) infinite;font-weight:900}
  .hp.unknown .st{color:var(--mut)}
  @keyframes blink{50%{opacity:.4}}
  /* ---------- Recent feed ---------- */
  .feed{
    background:var(--surf);border:1px solid var(--bd);border-radius:18px;
    padding:18px;margin-bottom:14px;
  }
  .feed h3{
    font-size:13px;color:var(--mut);text-transform:uppercase;letter-spacing:.7px;
    margin-bottom:10px;
  }
  .row{
    display:grid;grid-template-columns:60px 60px 1fr 80px;gap:10px;
    padding:10px 8px;border-bottom:1px dashed rgba(255,255,255,.06);
    font-size:13px;align-items:center;
  }
  .row:last-child{border-bottom:none}
  .row .t{font-weight:800}
  .row .s{font-weight:800;text-align:center;border-radius:8px;padding:4px 0;font-size:12px}
  .row .s.s1{background:rgba(255,77,109,.15);color:#ff4d6d}
  .row .s.s2{background:rgba(255,212,59,.15);color:#ffd43b}
  .row .s.s3{background:rgba(76,201,240,.15);color:#4cc9f0}
  .row .s.s4{background:rgba(0,229,168,.15);color:#00e5a8}
  .row .ago{color:var(--mut);font-family:Menlo,monospace;font-size:12px;text-align:right}
  /* ---------- Action buttons ---------- */
  .actions{
    display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;
  }
  button,a.btn{
    background:var(--surf2);border:1px solid var(--bd);color:var(--tx);
    padding:11px 18px;border-radius:12px;font-weight:700;cursor:pointer;
    font-size:14px;text-decoration:none;display:inline-block;
  }
  button:hover,a.btn:hover{background:var(--bd)}
  button.danger{color:var(--d);border-color:rgba(255,77,109,.45)}
  button.primary{color:var(--p);border-color:rgba(0,229,168,.45)}
  /* Modal tools row */
  .modal-tools{display:flex;gap:10px;flex-wrap:wrap;margin-top:6px}
  .modal-tools button{flex:1;min-width:120px}
</style>
</head>
<body>
<header>
  <div class="logo">KT</div>
  <h1>KIK Target <small>Prototype V3 — pengesanan tembakan</small></h1>
  <div class="pill"><span class="dot" id="dot"></span><span id="conn">Menyambung…</span></div>
</header>

<!-- HERO: keseluruhan + grid 15 tile -->
<div class="hero">
  <div class="hero-stats">
    <div class="hs">
      <div class="lbl">Sasaran Online</div>
      <div class="num"><span id="onCount">0</span><small>/<span id="totCount">15</span></small></div>
    </div>
    <div class="hs big">
      <div class="lbl">Jumlah Hit (semua sasaran)</div>
      <div class="num" id="totalHits">0</div>
    </div>
  </div>
  <div class="tile-label">Sasaran <small>(tap untuk detail)</small></div>
  <div class="tile-grid" id="tileGrid"></div>
  <div class="tile-legend">
    <span><span class="lg ok"></span>Online</span>
    <span><span class="lg off"></span>Offline</span>
    <span><span class="lg dead"></span>Sensor MATI</span>
  </div>
</div>

<!-- DETAIL MODAL -->
<div class="modal hidden" id="modal" onclick="closeModalBg(event)">
  <div class="modal-card" onclick="event.stopPropagation()">
    <div class="modal-head">
      <div class="modal-title">Sasaran <span id="mNum">—</span> <small id="mStatus">offline</small></div>
      <button class="close" onclick="closeModal()">&times;</button>
    </div>
    <div class="m-stats">
      <div class="m-stat span2"><div class="lbl">Jumlah Hit</div><div class="v" id="mHits">0</div></div>
      <div class="m-stat"><div class="lbl">Last Trigger</div><div class="v" id="mTrig">—</div></div>
      <div class="m-stat"><div class="lbl">Last Hit</div><div class="v" id="mAgo">—</div></div>
    </div>
    <div class="health-row">
      <div class="hp unknown" id="mHp1"><div class="lbl">S1</div><div class="st">?</div></div>
      <div class="hp unknown" id="mHp2"><div class="lbl">S2</div><div class="st">?</div></div>
      <div class="hp unknown" id="mHp3"><div class="lbl">S3</div><div class="st">?</div></div>
      <div class="hp unknown" id="mHp4"><div class="lbl">S4</div><div class="st">?</div></div>
    </div>
  </div>
</div>

<!-- TEMBAKAN TERKINI + ACTIONS -->
<div class="feed">
  <h3>Tembakan Terkini</h3>
  <div id="feed"></div>
  <div class="actions">
    <button onclick="resetAll()" class="danger">Reset Sesi</button>
    <a class="btn" href="/log.csv" download>Muat Turun CSV (<span id="loglines">0</span>)</a>
    <button onclick="clearLog()">Padam Log</button>
  </div>
</div>

<script>
// ---------- config ----------
const N = 15;
const POLL_MS = 250;
const SENSOR_LABEL = {ok:'OK', noisy:'NOISY', broken:'ROSAK', dead:'MATI', unknown:'?'};

// ---------- helpers ----------
function fmtAgo(ms){
  if(!ms) return '—';
  if(ms < 1500) return 'baru';
  const s = Math.round(ms/1000);
  if(s < 60) return s+'s';
  return Math.round(s/60)+'m';
}
function tileStatusClass(t){
  if(!t || !t.online) return 'offline';
  // any sensor flagged dead/broken -> mark tile as dead so it blinks
  for(const sd of (t.sensors || [])){
    if(sd.h === 'dead' || sd.h === 'broken') return 'dead';
  }
  return 'online';
}

// ---------- tile grid ----------
function buildTiles(){
  const g = document.getElementById('tileGrid');
  g.innerHTML = '';
  for(let i=1;i<=N;i++){
    const d = document.createElement('div');
    d.className = 'tile offline';
    d.id = 'tile'+i;
    d.onclick = () => openModal(i);
    d.innerHTML =
      '<div class="tnum">T'+i+'</div>' +
      '<div class="thits" id="th'+i+'">0</div>' +
      '<div class="tlast" id="tl'+i+'">offline</div>';
    g.appendChild(d);
  }
}

// ---------- modal (per-target detail) ----------
let currentModalTarget = 0;
let lastSnapshot = null;
function openModal(i){
  currentModalTarget = i;
  document.getElementById('modal').classList.remove('hidden');
  refreshModal();
}
function closeModal(){
  currentModalTarget = 0;
  document.getElementById('modal').classList.add('hidden');
}
function closeModalBg(e){
  if(e.target.id === 'modal') closeModal();
}
function refreshModal(){
  if(!currentModalTarget || !lastSnapshot) return;
  const i = currentModalTarget;
  const t = (lastSnapshot.targets || []).find(x => x.id === i);
  document.getElementById('mNum').textContent = i;
  if(!t || !t.online){
    document.getElementById('mStatus').textContent = 'offline';
    document.getElementById('mHits').textContent = (t && t.hits) || 0;
    document.getElementById('mTrig').textContent = '—';
    document.getElementById('mAgo').textContent = '—';
    for(let s=1;s<=4;s++){
      const el = document.getElementById('mHp'+s);
      el.classList.remove('ok','noisy','broken','dead','unknown');
      el.classList.add('unknown');
      el.querySelector('.st').textContent = '?';
    }
    return;
  }
  document.getElementById('mStatus').textContent = 'online';
  document.getElementById('mHits').textContent = t.hits || 0;
  document.getElementById('mTrig').textContent = t.lastTrigger ? ('S'+t.lastTrigger) : '—';
  document.getElementById('mAgo').textContent = fmtAgo(t.ago || 0);
  for(let s=1;s<=4;s++){
    const sd = (t.sensors || [])[s-1] || {};
    const h = sd.h || 'unknown';
    const el = document.getElementById('mHp'+s);
    el.classList.remove('ok','noisy','broken','dead','unknown');
    el.classList.add(h);
    el.querySelector('.st').textContent = SENSOR_LABEL[h] || '?';
    el.title = 'baseline='+(sd.bl||0)+'  peak='+(sd.pk||0);
  }
}
document.addEventListener('keydown', (e) => {
  if(e.key === 'Escape') closeModal();
});

// ---------- poll loop ----------
async function tick(){
  try{
    const r = await fetch('/status', {cache:'no-store'});
    const d = await r.json();
    lastSnapshot = d;
    document.getElementById('dot').classList.add('on');
    document.getElementById('conn').textContent = 'Tersambung';
    let online = 0, totalHits = 0;
    for(let i=1;i<=N;i++){
      const t = (d.targets || []).find(x => x.id === i);
      const tile = document.getElementById('tile'+i);
      const th   = document.getElementById('th'+i);
      const tl   = document.getElementById('tl'+i);
      tile.classList.remove('online','offline','dead');
      tile.classList.add(tileStatusClass(t));
      if(t && t.online){
        online++;
        totalHits += t.hits || 0;
        th.textContent = t.hits || 0;
        tl.textContent = t.lastTrigger ? ('S'+t.lastTrigger+' • '+fmtAgo(t.ago||0)) : 'idle';
      } else {
        th.textContent = (t && t.hits) || 0;
        tl.textContent = 'offline';
      }
    }
    document.getElementById('onCount').textContent = online;
    document.getElementById('totCount').textContent = N;
    document.getElementById('totalHits').textContent = totalHits;
    renderFeed(d.recent || []);
    const ll = document.getElementById('loglines');
    if(ll) ll.textContent = (d.log && typeof d.log.lines === 'number') ? d.log.lines : '0';
    refreshModal();
  } catch(e){
    document.getElementById('dot').classList.remove('on');
    document.getElementById('conn').textContent = 'Terputus';
  }
}

// ---------- recent feed (no zone / score) ----------
function renderFeed(recent){
  const f = document.getElementById('feed');
  f.innerHTML = '';
  if(!recent || recent.length === 0){
    f.innerHTML = '<div style="color:var(--mut);font-size:13px;padding:8px">Belum ada tembakan…</div>';
    return;
  }
  const now = Date.now();
  // newest first
  for(let k = recent.length - 1; k >= 0; k--){
    const r = recent[k];
    const row = document.createElement('div');
    row.className = 'row';
    const ageMs = r.ts ? Math.max(0, now - (now - (recent[recent.length-1].ts - r.ts))) : 0;
    // Simpler: use recent ordering as ago label since ts is target-board millis
    // not synced to dashboard wallclock.
    const idx = recent.length - 1 - k;
    const agoLbl = idx === 0 ? 'baru' : (idx + ' tembakan lepas');
    row.innerHTML =
      '<div class="t">T'+r.t+'</div>' +
      '<div class="s s'+r.trig+'">S'+r.trig+'</div>' +
      '<div></div>' +
      '<div class="ago">'+agoLbl+'</div>';
    f.appendChild(row);
  }
}

// ---------- actions ----------
async function resetAll(){
  if(!confirm('Reset semua kaunter hit dan tembakan terkini?')) return;
  await fetch('/reset', {method:'POST'});
}
async function clearLog(){
  if(!confirm('Padam log sesi (CSV)?')) return;
  await fetch('/log/clear', {method:'POST'});
}

// ---------- boot ----------
buildTiles();
setInterval(tick, POLL_MS);
tick();
</script>
</body>
</html>
)RAW";

// =====================================================================
//  ESP-NOW receive callback (core 2.x and 3.x signature shim)
// =====================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNow(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  (void)info;
#else
static void onEspNow(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;
#endif
  if (len < 2) return;
  const uint8_t ptype = data[0];
  const uint8_t tid   = data[1];
  if (tid < 1 || tid > NUM_TARGETS) return;
  TargetState& t = T[tid];

  if (ptype == 3) {
    if (len != (int)sizeof(HealthPacket)) return;
    HealthPacket h; memcpy(&h, data, sizeof(h));
    t.online       = true;
    t.lastSeen     = millis();
    t.lastHealthMs = millis();
    for (int i = 0; i < 4; i++) {
      t.baseline[i] = h.baseline[i];
      t.peak[i]     = h.peak[i];
    }
    // Differential silent-sensor detection. We can't reliably tell a
    // cut wire from a quiet, *connected* piezo by looking at one
    // sensor in isolation — both sit at ADC 0 when nothing hits the
    // board. What we CAN check is: in this 5-second window, did at
    // least one OTHER sensor on the same target see real activity?
    // If yes, any sensor that stayed at the floor in the same window
    // is suspect (cut wire / dead piezo). If no, the whole target was
    // just idle and we don't penalise anyone.
    uint16_t maxOtherPeak[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        if (j != i && h.peak[j] > maxOtherPeak[i]) maxOtherPeak[i] = h.peak[j];
      }
    }
    for (int i = 0; i < 4; i++) {
      if (h.peak[i] >= SILENT_ACTIVITY_THRESH) {
        // This sensor itself was active — definitely alive, clear streak.
        t.silentInActiveStreak[i] = 0;
      } else if (maxOtherPeak[i] >= SILENT_ACTIVITY_THRESH) {
        // Other sensors active but this one stayed silent — suspicious.
        if (t.silentInActiveStreak[i] < 0xFF) t.silentInActiveStreak[i]++;
      }
      // else: whole-target idle window; leave streak unchanged so a
      // long undisturbed period doesn't either set or clear MATI.
    }
    // Echo received Health to Serial so a user with a laptop hooked to
    // the central can read the real ADC numbers without the dashboard
    // (mobile Chrome silently drops `title=` tooltips, which was our
    // only previous way to expose these). Format mirrors the target
    // Serial print so identical lines on both ends prove what got
    // transmitted vs received.
    Serial.printf("Health rx T=%u bl=[%u,%u,%u,%u] pk=[%u,%u,%u,%u] streak=[%u,%u,%u,%u]\n",
                  (unsigned)tid,
                  (unsigned)h.baseline[0], (unsigned)h.baseline[1],
                  (unsigned)h.baseline[2], (unsigned)h.baseline[3],
                  (unsigned)h.peak[0], (unsigned)h.peak[1],
                  (unsigned)h.peak[2], (unsigned)h.peak[3],
                  (unsigned)t.silentInActiveStreak[0], (unsigned)t.silentInActiveStreak[1],
                  (unsigned)t.silentInActiveStreak[2], (unsigned)t.silentInActiveStreak[3]);
    return;
  }
  if (ptype == 2) {
    if (len != (int)sizeof(HeartbeatPacket)) return;
    HeartbeatPacket h; memcpy(&h, data, sizeof(h));
    t.online   = true;
    t.lastSeen = millis();
    return;
  }
  if (ptype != 1) return;
  if (len != (int)sizeof(HitPacketV2)) return;

  HitPacketV2 p; memcpy(&p, data, sizeof(p));
  t.online   = true;
  t.lastSeen = millis();

  // ---------- Stage 5: calibration interception ----------
  // While the user is calibrating this target, hits are training samples,
  // not scored events. Capture peak[step] (the sensor we're asking the
  // user to tap directly) and advance. When all 4 sensors are captured,
  // commit to NVS and exit calibration mode.
  if (gCalState == CalState::ACTIVE && gCalTarget == tid) {
    if (gCalStep < 4) {
      // Use the trigger sensor's peak — that's the sensor closest to the
      // tap. In the UX flow we tell the user "tap directly above S<step+1>"
      // so trigger should already match step, but capturing peak[step]
      // unconditionally avoids surprises if they miss-tap slightly.
      uint8_t s = gCalStep;
      gCalCapture[s] = p.peak[s];
      Serial.printf("Cal T=%u S%u captured peak=%u\n",
                    (unsigned)tid, (unsigned)(s + 1), (unsigned)p.peak[s]);
      gCalStep++;
      if (gCalStep >= 4) {
        // Done capturing. Hand the captured peaks off to the main loop
        // for the actual `gCal[tid]` update + NVS save — those touch
        // `gPrefs`, which `handleCalClear()` (running on the main loop)
        // also opens, and Preferences is not thread-safe. Doing the
        // commit on the main loop also keeps tens-of-ms NVS flash
        // writes out of the WiFi receive callback.
        for (int i = 0; i < 4; i++) gCalCommitRef[i] = gCalCapture[i];
        gCalCommitTarget  = tid;
        gCalCommitPending = true;
        Serial.printf("Cal T=%u capture complete, queued for commit\n",
                      (unsigned)tid);
        // Exit the calibration UX immediately so the dashboard stops
        // prompting for taps; the in-memory `gCal[tid]` + NVS write
        // will land a few ms later in loop().
        gCalState  = CalState::IDLE;
        gCalTarget = 0;
        gCalStep   = 0;
      }
    }
    gHitPending = true;     // still let buzzer chirp so user has feedback
    return;                 // do NOT add to score / ring buffer
  }

  // Triangulate position + score zone.
  float x, y;
  triangulate(tid, p.peak, x, y);
  uint8_t zoneIdx, score;
  scoreZone(x, y, zoneIdx, score);

  // Split time: gap from last hit on this target. First hit -> 0.
  uint32_t now = millis();
  uint16_t splitMs = 0;
  if (t.hitCount > 0 && t.lastHitMs != 0) {
    uint32_t dt = now - t.lastHitMs;
    splitMs = dt > 0xFFFFu ? 0xFFFFu : (uint16_t)dt;
    t.splitSumMs += splitMs;
    t.splitCount++;
    if (t.minSplitMs == 0 || splitMs < t.minSplitMs) t.minSplitMs = splitMs;
  }
  t.lastSplitMs = splitMs;

  t.hitCount++;
  t.scoreSum    += score;
  t.lastHitMs    = now;
  t.lastHitSeq   = p.hitSeq;
  t.lastTrigger  = p.triggerSensor;
  t.lastZone     = zoneIdx;
  t.lastScore    = score;
  t.lastX        = x;
  t.lastY        = y;
  for (int i = 0; i < 4; i++) t.lastPeak[i] = p.peak[i];

  // ---------- Hit-based dead-sensor detection ----------
  // For each sensor, did this hit register any meaningful response
  // (peak well above current baseline)? If not, increment the silent
  // streak; otherwise reset it. Threshold is conservative on purpose:
  // even cross-target propagation typically lifts an idle piezo by
  // 50–100 counts when a shot lands, while a broken wire stays within
  // a few counts of baseline.
  static const int16_t DEAD_DELTA = 50;
  for (int i = 0; i < 4; i++) {
    int32_t resp = (int32_t)p.peak[i] - (int32_t)t.baseline[i];
    if (resp < DEAD_DELTA) {
      if (t.deadStreak[i] < 0xFF) t.deadStreak[i]++;
    } else {
      t.deadStreak[i] = 0;
    }
  }
  if (t.hitsForHealth < 0xFFFF) t.hitsForHealth++;

  // Append to ring buffer.
  RecentHit& r = recentBuf[recentHead];
  r.ts            = now;
  r.targetID      = tid;
  r.triggerSensor = p.triggerSensor;
  r.zone          = zoneIdx;
  r.score         = score;
  r.x10           = (int16_t)(x * 1000.f);
  r.y10           = (int16_t)(y * 1000.f);
  r.splitMs       = splitMs;
  recentHead = (recentHead + 1) % RECENT_N;
  if (recentCount < 0xFFFF) recentCount++;

  // ---------- Stage 6: enqueue for SPIFFS log ----------
  // Non-blocking send. If the queue is full, we silently drop — better
  // than stalling the ESP-NOW callback. The main loop drains the queue.
  if (gLogQueue) {
    LogEntry le{};
    le.ts            = now;
    le.targetID      = tid;
    le.triggerSensor = p.triggerSensor;
    le.zone          = zoneIdx;
    le.score         = score;
    le.x10           = r.x10;
    le.y10           = r.y10;
    le.splitMs       = splitMs;
    for (int i = 0; i < 4; i++) le.peak[i] = p.peak[i];
    xQueueSend(gLogQueue, &le, 0);
  }

  gHitPending = true;
}

// =====================================================================
//  HTTP handlers
// =====================================================================
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// Helper: JSON-escaped status of one sensor's health.
// `deadStreak` and `hitsForHealth` come from the per-target hit history
// and let us flag a wire-disconnect (which a baseline check alone
// cannot detect, because a broken wire and an idle sensor both sit at
// ~0 V due to the pull-down).
static const char* sensorHealthLabel(uint16_t baselineV, uint32_t healthAge,
                                     bool haveHealth,
                                     uint8_t deadStreak,
                                     uint16_t hitsForHealth,
                                     uint8_t silentInActiveStreak) {
  if (!haveHealth)         return "unknown";
  if (baselineV >= 3000)   return "broken";
  if (hitsForHealth >= DEAD_MIN_HITS && deadStreak >= DEAD_STREAK_N) {
    // Sensor failed to respond to several consecutive shots while other
    // sensors on the same target ARE registering activity — almost
    // certainly a wire/contact problem at this piezo.
    return "dead";
  }
  if (silentInActiveStreak >= SILENT_IN_ACTIVE_STREAK_N) {
    // Same conclusion as the hit-based path but driven from Health
    // packets: in N back-to-back Health windows where at least one
    // OTHER sensor saw activity, this one stayed at the floor.
    return "dead";
  }
  if (baselineV >= 1000)   return "noisy";
  return "ok";
  (void)healthAge;
}

static void handleStatus() {
  String j;
  // ~400-450 B per target plus recent[] (~80 B/entry, RECENT_N=48) and the
  // top-level cal/log objects. 12 KB headroom for 15 targets + full feed.
  j.reserve(12288);
  j += "{\"targets\":[";
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    if (i > 1) j += ",";
    j += "{\"id\":";          j += i;
    j += ",\"online\":";      j += (t.online ? "true" : "false");
    j += ",\"calValid\":";    j += (gCal[i].valid ? "true" : "false");
    j += ",\"calRef\":[";
      j += gCal[i].refPeak[0]; j += ",";
      j += gCal[i].refPeak[1]; j += ",";
      j += gCal[i].refPeak[2]; j += ",";
      j += gCal[i].refPeak[3];
    j += "]";
    j += ",\"hits\":";        j += t.hitCount;
    j += ",\"scoreSum\":";    j += (uint32_t)t.scoreSum;
    j += ",\"lastHitSeq\":";  j += t.lastHitSeq;
    j += ",\"lastTrigger\":"; j += t.lastTrigger;
    j += ",\"lastZone\":";    j += t.lastZone;
    j += ",\"lastZoneLabel\":\"";
    // Emit "" until the first hit so the dashboard's `value || '—'` fallback
    // can show a dash instead of misreporting zone-0 (centre) as the last
    // hit on a freshly-reset / freshly-online target.
    if (t.hitCount == 0)             { /* leave empty */ }
    else if (t.lastZone < NUM_RINGS) j += String((int)RING_SCORE[t.lastZone]);
    else                             j += "M";
    j += "\"";
    j += ",\"lastScore\":";   j += t.lastScore;
    // Floats serialized with 3 decimals.
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", t.lastX); j += ",\"lastX\":"; j += buf;
    snprintf(buf, sizeof(buf), "%.3f", t.lastY); j += ",\"lastY\":"; j += buf;
    j += ",\"lastSplit\":";   j += t.lastSplitMs;
    j += ",\"minSplit\":";    j += t.minSplitMs;
    j += ",\"splitSum\":";    j += (uint32_t)t.splitSumMs;
    j += ",\"splitN\":";      j += t.splitCount;
    j += ",\"ago\":";         j += (uint32_t)(t.lastHitMs ? (millis() - t.lastHitMs) : 0);
    // Per-sensor health.
    uint32_t healthAge = t.lastHealthMs ? (millis() - t.lastHealthMs) : 0xFFFFFFFFu;
    // Health packet now arrives every 5 s; allow ~2.5x interval so a
    // single dropped/jittered packet doesn't flip every sensor to
    // "unknown" between reports.
    bool haveHealth = t.lastHealthMs && healthAge < 12000;
    j += ",\"healthAge\":"; j += (uint32_t)healthAge;
    j += ",\"sensors\":[";
    for (int s = 0; s < 4; s++) {
      if (s) j += ",";
      uint16_t bl = t.baseline[s];
      uint16_t pk = t.peak[s];
      j += "{\"bl\":" ;  j += bl;
      j += ",\"pk\":" ; j += pk;
      j += ",\"h\":\""; j += sensorHealthLabel(bl, healthAge, haveHealth,
                                                t.deadStreak[s],
                                                t.hitsForHealth,
                                                t.silentInActiveStreak[s]);
      j += "\"}";
    }
    j += "]}";
  }
  j += "],\"recent\":[";
  // Walk ring buffer in chronological order (oldest first).
  uint16_t n = recentCount < RECENT_N ? recentCount : RECENT_N;
  for (uint16_t k = 0; k < n; k++) {
    uint8_t idx = (uint8_t)((recentHead + RECENT_N - n + k) % RECENT_N);
    const RecentHit& r = recentBuf[idx];
    if (k > 0) j += ",";
    j += "{\"t\":";        j += r.targetID;
    j += ",\"trig\":";     j += r.triggerSensor;
    j += ",\"zone\":";     j += r.zone;
    j += ",\"score\":";    j += r.score;
    j += ",\"x10\":";      j += r.x10;
    j += ",\"y10\":";      j += r.y10;
    j += ",\"split\":";    j += r.splitMs;
    j += ",\"ts\":";       j += (uint32_t)r.ts;
    j += "}";
  }
  j += "]";
  // Top-level calibration / log status.
  j += ",\"cal\":{";
    j += "\"active\":";       j += (gCalState == CalState::ACTIVE ? "true" : "false");
    j += ",\"target\":";      j += gCalTarget;
    j += ",\"step\":";        j += gCalStep;
    j += ",\"capture\":[";
      j += gCalCapture[0]; j += ",";
      j += gCalCapture[1]; j += ",";
      j += gCalCapture[2]; j += ",";
      j += gCalCapture[3];
    j += "]";
  j += "}";
  j += ",\"log\":{";
    j += "\"ready\":";        j += (gSpiffsReady ? "true" : "false");
    j += ",\"lines\":";       j += (uint32_t)gLogLines;
  j += "}";
  j += "}";
  // Disable HTTP caching: the dashboard polls /status every 200 ms and
  // mobile browsers (the primary deployment target — tablets/phones) can
  // aggressively cache responses without an explicit directive, leading
  // to stale hit counts / scores / sensor health on screen.
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// ---------- Stage 5 endpoints ----------
static void handleCalStart() {
  if (!server.hasArg("target")) { server.send(400, "application/json",
      "{\"ok\":false,\"err\":\"missing target\"}"); return; }
  int tid = server.arg("target").toInt();
  if (tid < 1 || tid > NUM_TARGETS) { server.send(400, "application/json",
      "{\"ok\":false,\"err\":\"bad target\"}"); return; }
  gCalState  = CalState::ACTIVE;
  gCalTarget = (uint8_t)tid;
  gCalStep   = 0;
  for (int i = 0; i < 4; i++) gCalCapture[i] = 0;
  Serial.printf("Cal START T=%d\n", tid);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleCalCancel() {
  Serial.println("Cal CANCEL");
  gCalState  = CalState::IDLE;
  gCalTarget = 0;
  gCalStep   = 0;
  for (int i = 0; i < 4; i++) gCalCapture[i] = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleCalClear() {
  if (!server.hasArg("target")) { server.send(400, "application/json",
      "{\"ok\":false,\"err\":\"missing target\"}"); return; }
  int tid = server.arg("target").toInt();
  if (tid < 1 || tid > NUM_TARGETS) { server.send(400, "application/json",
      "{\"ok\":false,\"err\":\"bad target\"}"); return; }
  clearCalibrationFor((uint8_t)tid);
  Serial.printf("Cal CLEAR T=%d\n", tid);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- Stage 6 endpoints ----------
static void handleLogCsv() {
  if (!gSpiffsReady) { server.send(503, "text/plain", "SPIFFS not ready"); return; }
  if (!SPIFFS.exists(LOG_PATH)) {
    // Empty log — return just the header so the file is well-formed.
    server.send(200, "text/csv; charset=utf-8", LOG_HEADER);
    return;
  }
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=session.csv");
  server.streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

static void handleLogClear() {
  logResetFile();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleReset() {
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    t.hitCount = 0;
    t.scoreSum = 0;
    t.lastHitSeq = 0;
    t.lastHitMs = 0;
    t.lastTrigger = 0;
    t.lastZone = 0;
    t.lastScore = 0;
    t.lastX = 0; t.lastY = 0;
    t.lastSplitMs = 0;
    t.minSplitMs = 0;
    t.splitSumMs = 0;
    t.splitCount = 0;
    for (int k = 0; k < 4; k++) t.lastPeak[k] = 0;
  }
  recentHead = 0; recentCount = 0;
  for (int k = 0; k < RECENT_N; k++) recentBuf[k] = RecentHit{};
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
  server.send(404, "text/plain", "404");
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  KIK_BUZZ_SETUP(BUZZER_PIN, BUZZ_CH, 4000, 8);
  KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);

  // WiFi softAP (B/G/N only — DROP LR so phones/tablets see the SSID).
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, /*channel=*/1, /*hidden=*/0,
              /*max_conn=*/4);
  esp_wifi_set_protocol(WIFI_IF_AP,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.println();
  Serial.println("KIK Central V2 ready.");
  Serial.printf("MAC:      %s   <-- paste this into target_v2 RECEIVER_MAC\n",
                WiFi.softAPmacAddress().c_str());
  Serial.printf("AP SSID:  %s\n", AP_SSID);
  Serial.println("AP IP:    192.168.4.1");
  Serial.println("Open http://192.168.4.1/ on your tablet.");

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); while (1) delay(500);
  }
  esp_now_register_recv_cb(onEspNow);

  // ---------- Stage 5: load calibration from NVS ----------
  loadCalibration();
  for (int t = 1; t <= NUM_TARGETS; t++) {
    if (gCal[t].valid) {
      Serial.printf("Cal T=%d loaded ref=[%u,%u,%u,%u]\n", t,
                    gCal[t].refPeak[0], gCal[t].refPeak[1],
                    gCal[t].refPeak[2], gCal[t].refPeak[3]);
    } else {
      Serial.printf("Cal T=%d not yet calibrated\n", t);
    }
  }

  // ---------- Stage 6: SPIFFS log ----------
  if (SPIFFS.begin(/*formatOnFail=*/true)) {
    gSpiffsReady = true;
    if (!SPIFFS.exists(LOG_PATH)) {
      logResetFile();
    } else {
      // Count existing lines to seed gLogLines (cheap; file is small).
      File f = SPIFFS.open(LOG_PATH, FILE_READ);
      if (f) {
        uint32_t lines = 0;
        while (f.available()) {
          if (f.read() == '\n') lines++;
        }
        f.close();
        gLogLines = lines > 0 ? lines - 1 : 0;   // subtract header
      }
    }
    Serial.printf("SPIFFS ready, %lu lines in log\n",
                  (unsigned long)gLogLines);
  } else {
    Serial.println("SPIFFS mount failed; logging disabled");
  }
  gLogQueue = xQueueCreate(LOG_QUEUE_LEN, sizeof(LogEntry));
  if (!gLogQueue) Serial.println("Log queue alloc failed");

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/status",      HTTP_GET,  handleStatus);
  server.on("/reset",       HTTP_POST, handleReset);
  server.on("/cal/start",   HTTP_POST, handleCalStart);
  server.on("/cal/cancel",  HTTP_POST, handleCalCancel);
  server.on("/cal/clear",   HTTP_POST, handleCalClear);
  server.on("/log.csv",     HTTP_GET,  handleLogCsv);
  server.on("/log/clear",   HTTP_POST, handleLogClear);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  // Drain any pending calibration commit handed off from onEspNow.
  // Performing the gCal[] update + NVS save here means all gPrefs
  // access happens on a single task (the Arduino main loop), so the
  // shared Preferences object cannot be corrupted by concurrent
  // begin()/end() from the WiFi task. handleCalClear() also runs on
  // this task, so save and clear are now naturally serialized.
  if (gCalCommitPending) {
    gCalCommitPending = false;
    uint8_t tid = gCalCommitTarget;
    if (tid >= 1 && tid <= NUM_TARGETS) {
      for (int i = 0; i < 4; i++) gCal[tid].refPeak[i] = gCalCommitRef[i];
      gCal[tid].valid = true;
      saveCalibrationFor(tid);
      Serial.printf("Cal T=%u DONE ref=[%u,%u,%u,%u]\n",
                    (unsigned)tid,
                    gCal[tid].refPeak[0], gCal[tid].refPeak[1],
                    gCal[tid].refPeak[2], gCal[tid].refPeak[3]);
    }
  }

  // Non-blocking buzzer chirp: start a tone on each new hit and remember
  // when to silence it. We must NOT delay() here — at the v2 debounce
  // (60 ms) hits can arrive every ~90 ms, and any blocking time starves
  // server.handleClient() and the SPIFFS log drain below.
  static uint32_t buzzOff = 0;
  if (gHitPending) {
    gHitPending = false;
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 4000);
    buzzOff = now + 20;
  }
  if (buzzOff && now >= buzzOff) {
    KIK_BUZZ_TONE(BUZZER_PIN, BUZZ_CH, 0);
    buzzOff = 0;
  }

  // ---------- Stage 6: drain log queue ----------
  // We do at most a few writes per loop iteration so we don't hold off
  // server.handleClient() for too long. SPIFFS append of one short line
  // typically completes in <5 ms.
  if (gLogQueue) {
    LogEntry le;
    int drained = 0;
    while (drained < 4 && xQueueReceive(gLogQueue, &le, 0) == pdTRUE) {
      logFlushOne(le);
      drained++;
    }
  }

  // Mark targets offline after silence.
  for (int i = 1; i <= NUM_TARGETS; i++) {
    TargetState& t = T[i];
    if (t.online && (now - t.lastSeen) > 8000) {
      t.online = false;
    }
  }
}
