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
<title>KIK Target V2</title>
<style>
  :root{
    --bg:#0b1221;--surf:#162036;--surf2:#1e2b4a;--bd:#263556;
    --tx:#f4f6fb;--mut:#8896b8;
    --p:#00e5a8;--i:#4cc9f0;--a:#b56cff;--d:#ff4d6d;--w:#ffd43b;
    --s1:#ff4d6d;--s2:#ffd43b;--s3:#4cc9f0;--s4:#00e5a8;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{
    font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    background:radial-gradient(1200px 500px at 0% -10%,rgba(0,229,168,.14),transparent 60%),
               radial-gradient(1000px 500px at 100% 0%,rgba(181,108,255,.12),transparent 60%),
               var(--bg);
    color:var(--tx);min-height:100vh;padding:14px;
  }
  header{display:flex;align-items:center;gap:14px;margin-bottom:14px;flex-wrap:wrap}
  .logo{
    width:42px;height:42px;border-radius:12px;
    background:linear-gradient(135deg,var(--p),var(--i));
    display:grid;place-items:center;font-weight:900;font-size:19px;color:#0b1221;
  }
  h1{font-size:18px;letter-spacing:.4px}
  h1 small{color:var(--mut);font-weight:500;font-size:12px;display:block}
  .pill{
    margin-left:auto;display:inline-flex;align-items:center;gap:8px;
    padding:6px 12px;border:1px solid var(--bd);border-radius:999px;
    background:rgba(255,255,255,.04);font-size:12px;color:var(--mut);
  }
  .dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
  .dot.on{background:var(--p);box-shadow:0 0 10px var(--p)}
  .stat-row{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;margin-bottom:14px}
  .stat{
    background:var(--surf);border:1px solid var(--bd);border-radius:14px;padding:10px 12px;
  }
  .stat .l{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px}
  .stat .v{font-size:20px;font-weight:800;margin-top:2px}
  .stat .v small{font-size:11px;color:var(--mut);font-weight:500}
  /* With 15 cards we want more density. minmax(260px,1fr) lets a typical
     1024px-wide tablet fit 3 cards per row instead of 2, and a 1280px
     desktop fit 4. Cards stay readable because the per-target canvas
     stays square via aspect-ratio:1/1. */
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
  .card{
    background:var(--surf);border:1px solid var(--bd);border-radius:18px;
    padding:14px;display:flex;flex-direction:column;gap:10px;
  }
  .card.off{opacity:.4}
  .card-head{display:flex;align-items:center;gap:10px}
  .badge{
    background:var(--surf2);border:1px solid var(--bd);
    border-radius:999px;padding:4px 9px;font-size:11px;color:var(--mut);
  }
  .badge.online{color:var(--p);border-color:rgba(0,229,168,.4)}
  .ttl{font-size:15px;font-weight:800;letter-spacing:.4px}
  .ttl small{color:var(--mut);font-weight:500;font-size:11px}
  .target-vis{
    width:100%;aspect-ratio:1/1;background:#0d1628;
    border-radius:14px;border:1px solid var(--bd);position:relative;overflow:hidden;
  }
  .target-vis canvas{position:absolute;inset:0;width:100%;height:100%;display:block}
  .stats2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;font-size:12px}
  .stats2 .k{
    background:var(--surf2);border:1px solid var(--bd);border-radius:10px;
    padding:7px 9px;display:flex;align-items:center;gap:6px;
  }
  .stats2 .k b{color:var(--mut);text-transform:uppercase;font-size:10px;letter-spacing:.5px}
  .stats2 .k span{margin-left:auto;font-weight:800;font-size:14px}
  .health-row{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
  .hp{
    border-radius:10px;border:1px solid var(--bd);padding:6px 4px;
    text-align:center;background:rgba(255,255,255,.02);
  }
  .hp .lbl{font-size:9px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
  .hp .st{font-size:10px;font-weight:800;letter-spacing:.4px;margin-top:2px}
  .hp.ok     .st{color:var(--p)}
  .hp.noisy  .st{color:var(--w)}
  .hp.broken .st{color:var(--d);animation:blink 1s steps(2) infinite}
  .hp.dead   .st{color:var(--d);animation:blink 1s steps(2) infinite;font-weight:900}
  .hp.unknown .st{color:var(--mut)}
  @keyframes blink{50%{opacity:.4}}
  .feed{
    background:var(--surf);border:1px solid var(--bd);border-radius:18px;padding:14px;
    margin-top:14px;
  }
  .feed h3{font-size:13px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px;margin-bottom:8px}
  .row{
    display:grid;grid-template-columns:54px 60px 50px 70px 110px 70px;gap:8px;
    padding:7px 6px;border-bottom:1px dashed rgba(255,255,255,.06);
    font-size:12px;align-items:center;
  }
  .row:last-child{border-bottom:none}
  .row .t{font-weight:800}
  .row .s{font-weight:800;text-align:center;border-radius:8px;padding:2px 0}
  .row .s.s1{background:rgba(255,77,109,.15);color:var(--s1)}
  .row .s.s2{background:rgba(255,212,59,.15);color:var(--s2)}
  .row .s.s3{background:rgba(76,201,240,.15);color:var(--s3)}
  .row .s.s4{background:rgba(0,229,168,.15);color:var(--s4)}
  .row .z{color:var(--mut);font-size:11px}
  .row .xy{color:var(--mut);font-family:Menlo,monospace;font-size:11px}
  .row .sp{color:var(--w);text-align:right;font-family:Menlo,monospace}
  .actions{margin-top:12px;display:flex;gap:8px;flex-wrap:wrap}
  button{
    background:var(--surf2);border:1px solid var(--bd);color:var(--tx);
    padding:8px 14px;border-radius:10px;font-weight:700;cursor:pointer;
  }
  button:hover{background:var(--bd)}
  button.danger{color:var(--d);border-color:rgba(255,77,109,.4)}
  button.primary{color:var(--p);border-color:rgba(0,229,168,.4)}
  a.btn{display:inline-block;text-decoration:none;background:var(--surf2);
       color:var(--tx);border:1px solid var(--bd);padding:7px 12px;
       border-radius:10px;font-size:12px}
  .calbar{
    background:linear-gradient(135deg,rgba(255,212,59,.18),rgba(76,201,240,.10));
    border:1px solid rgba(255,212,59,.4);border-radius:14px;padding:10px 14px;
    margin-bottom:14px;display:flex;align-items:center;gap:12px;flex-wrap:wrap;
  }
  .calbar.idle{display:none}
  .calbar .ttl2{font-weight:800;color:var(--w)}
  .calbar .step{
    background:rgba(0,0,0,.3);border:1px solid var(--bd);border-radius:999px;
    padding:3px 10px;font-size:12px;color:var(--mut);font-weight:700;
  }
  .calbar .step.active{background:var(--w);color:#0b1221;border-color:var(--w)}
  .calbar .step.done{background:rgba(0,229,168,.2);color:var(--p);border-color:var(--p)}
  .cal-status{
    font-size:10px;letter-spacing:.5px;text-transform:uppercase;
    padding:2px 7px;border-radius:999px;border:1px solid var(--bd);
    background:rgba(255,255,255,.04);color:var(--mut);
  }
  .cal-status.on{color:var(--p);border-color:rgba(0,229,168,.4)}
  .cal-status.off{color:var(--mut)}
  .card-tools{display:flex;gap:6px;flex-wrap:wrap;margin-top:4px}
  .card-tools button{font-size:11px;padding:5px 9px}
</style>
</head>
<body>
<header>
  <div class="logo">KT</div>
  <h1>KIK Target — Prototype V2 <small>4-sensor + triangulation + split-time</small></h1>
  <div class="pill"><span class="dot" id="dot"></span><span id="conn">Menyambung…</span></div>
</header>
<div class="calbar idle" id="calbar">
  <div class="ttl2" id="caltitle">Mod Kalibrasi</div>
  <div class="step" id="cstep1">S1</div>
  <div class="step" id="cstep2">S2</div>
  <div class="step" id="cstep3">S3</div>
  <div class="step" id="cstep4">S4</div>
  <div style="margin-left:auto"><button onclick="cancelCal()" class="danger">Batal</button></div>
</div>
<div class="stat-row">
  <div class="stat"><div class="l">Sasaran Aktif</div><div class="v" id="sa">0/0</div></div>
  <div class="stat"><div class="l">Jumlah Hit</div><div class="v" id="sh">0</div></div>
  <div class="stat"><div class="l">Jumlah Skor</div><div class="v" id="ss">0</div></div>
  <div class="stat"><div class="l">Avg Split</div><div class="v" id="avgs">— <small>ms</small></div></div>
  <div class="stat"><div class="l">Best Split</div><div class="v" id="bests">— <small>ms</small></div></div>
</div>
<div class="grid" id="grid"></div>
<div class="feed">
  <h3>Tembakan Terkini</h3>
  <div id="feed"></div>
  <div class="actions" style="display:flex;gap:8px;flex-wrap:wrap;margin-top:10px">
    <button onclick="resetAll()" class="danger">Reset Sesi</button>
    <a class="btn" href="/log.csv" download>Muat Turun CSV (<span id="loglines">0</span>)</a>
    <button onclick="clearLog()">Padam Log</button>
  </div>
</div>
<script>
// Must match NUM_TARGETS in central_v2.ino. Dashboard builds N cards on
// load; if you change one, change the other.
const N = 15;
const POLL_MS = 200;
// 5-zone target rings (must match RING_R[] in central_v2.ino).
const RING_R   = [0.20, 0.40, 0.60, 0.80, 1.30];
const RING_PTS = [5, 4, 3, 2, 1];
function fmtAgo(ms){
  if(!ms || ms<1000) return 'baru';
  const s = Math.round(ms/1000);
  if(s<60) return s+'s';
  return Math.round(s/60)+'m';
}
function buildGrid(){
  const g = document.getElementById('grid');
  g.innerHTML = '';
  for(let i=1;i<=N;i++){
    const c = document.createElement('div');
    c.className = 'card off';
    c.id = 'c'+i;
    c.innerHTML = `
      <div class="card-head">
        <div class="ttl">Sasaran ${i} <small>4 piezo</small></div>
        <div class="badge" id="bd${i}">offline</div>
        <div class="cal-status off" id="cal${i}" title="Status kalibrasi">CAL: —</div>
      </div>
      <div class="target-vis"><canvas id="cv${i}" width="240" height="240"></canvas></div>
      <div class="stats2">
        <div class="k"><b>Hit</b><span id="h${i}">0</span></div>
        <div class="k"><b>Skor</b><span id="sc${i}">0</span></div>
        <div class="k"><b>Last Split</b><span id="ls${i}">—</span></div>
        <div class="k"><b>Avg Split</b><span id="as${i}">—</span></div>
        <div class="k"><b>Last Zon</b><span id="lz${i}">—</span></div>
        <div class="k"><b>Last Trig</b><span id="lt${i}">—</span></div>
      </div>
      <div class="health-row">
        <div class="hp unknown" id="hp${i}_1"><div class="lbl">S1</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_2"><div class="lbl">S2</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_3"><div class="lbl">S3</div><div class="st">?</div></div>
        <div class="hp unknown" id="hp${i}_4"><div class="lbl">S4</div><div class="st">?</div></div>
      </div>
      <div class="card-tools">
        <button class="primary" onclick="startCal(${i})">Kalibrasi</button>
        <button onclick="clearCal(${i})">Padam Kalibrasi</button>
      </div>
    `;
    g.appendChild(c);
  }
}
const dotsHistory = {};   // {targetId: [{x,y,score,ts}, ...]}
function drawTarget(i, t){
  const cv = document.getElementById('cv'+i);
  if(!cv) return;
  const ctx = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  const cx = W/2, cy = H/2, R = Math.min(W,H)/2 * 0.95;
  ctx.fillStyle = '#0d1628';
  ctx.fillRect(0,0,W,H);
  // Draw 5 concentric scoring rings, outermost first so inner rings paint
  // on top. Alternate light/dark fills for legibility on small canvases.
  const ringFill = ['rgba(255,77,109,0.35)','rgba(255,143,163,0.25)',
                    'rgba(255,212,59,0.22)','rgba(76,201,240,0.18)',
                    'rgba(0,229,168,0.14)'];
  for(let r=RING_R.length-1;r>=0;r--){
    const rr = RING_R[r] * R;
    ctx.beginPath();
    ctx.arc(cx, cy, rr, 0, Math.PI*2);
    ctx.fillStyle = ringFill[r] || '#0d1628';
    ctx.fill();
    ctx.strokeStyle = 'rgba(255,255,255,0.25)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }
  // Centre dot (5-mark zone visual highlight)
  ctx.beginPath();
  ctx.arc(cx, cy, RING_R[0]*R*0.35, 0, Math.PI*2);
  ctx.fillStyle = '#ffd43b';
  ctx.fill();
  // Zone labels (5,4,3,2,1) along the +X axis between consecutive rings.
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.font = 'bold 10px monospace';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  let prev = 0;
  for(let r=0;r<RING_R.length;r++){
    const mid = (prev + RING_R[r]) / 2;
    ctx.fillText(''+RING_PTS[r], cx + mid*R, cy);
    prev = RING_R[r];
  }
  ctx.textAlign = 'start';
  ctx.textBaseline = 'alphabetic';
  // Sensor markers (S1..S4 at corners)
  ctx.fillStyle = '#8896b8';
  ctx.font = '10px monospace';
  ctx.fillText('S1', cx - R*0.95, cy - R*0.85);
  ctx.fillText('S2', cx + R*0.85, cy - R*0.85);
  ctx.fillText('S3', cx - R*0.95, cy + R*0.95);
  ctx.fillText('S4', cx + R*0.85, cy + R*0.95);
  // Hit dots from history (older = faded)
  const hist = dotsHistory[i] || [];
  for(let k=0;k<hist.length;k++){
    const d = hist[k];
    const age = (hist.length - 1 - k);
    const alpha = Math.max(0.18, 1 - age*0.06);
    const px = cx + d.x * R;
    const py = cy - d.y * R;     // flip Y so +Y is up on screen
    ctx.beginPath();
    ctx.arc(px, py, 5, 0, Math.PI*2);
    ctx.fillStyle = 'rgba(255,77,109,'+alpha+')';
    ctx.fill();
    ctx.strokeStyle = 'rgba(255,255,255,0.6)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }
}
function pushDot(i, x, y, score){
  if(!dotsHistory[i]) dotsHistory[i] = [];
  dotsHistory[i].push({x:x, y:y, score:score, ts:Date.now()});
  if(dotsHistory[i].length > 30) dotsHistory[i].shift();
}
function clearDots(i){ dotsHistory[i] = []; }
const lastSeq = {};
async function tick(){
  try{
    const r = await fetch('/status', {cache:'no-store'});
    const d = await r.json();
    document.getElementById('dot').classList.add('on');
    document.getElementById('conn').textContent = 'Tersambung';
    let active = 0, hits = 0, score = 0, splitSum = 0, splitN = 0, bestSplit = null;
    for(let i=1;i<=N;i++){
      const t = (d.targets || []).find(x=>x.id===i);
      const card = document.getElementById('c'+i);
      const bd = document.getElementById('bd'+i);
      if(!t || !t.online){
        card.classList.add('off');
        bd.textContent = 'offline';
        bd.classList.remove('online');
        continue;
      }
      card.classList.remove('off');
      bd.textContent = 'online';
      bd.classList.add('online');
      active++;
      hits  += t.hits||0;
      score += t.scoreSum||0;
      if(t.splitN > 0){
        splitSum += t.splitSum||0;
        splitN   += t.splitN;
      }
      if(t.minSplit && (bestSplit===null || t.minSplit < bestSplit)) bestSplit = t.minSplit;
      // Per-target stats
      document.getElementById('h'+i).textContent  = t.hits||0;
      document.getElementById('sc'+i).textContent = t.scoreSum||0;
      document.getElementById('ls'+i).textContent = (t.lastSplit||t.lastSplit===0)
          ? (t.lastSplit + ' ms') : '—';
      document.getElementById('as'+i).textContent = (t.splitN > 0)
          ? Math.round(t.splitSum / t.splitN) + ' ms' : '—';
      document.getElementById('lz'+i).textContent = t.lastZoneLabel || '—';
      document.getElementById('lt'+i).textContent = t.lastTrigger ? ('S'+t.lastTrigger) : '—';
      // Sensor health badges
      const healthLabel = {ok:'OK', noisy:'NOISY', broken:'ROSAK',
                           dead:'MATI', unknown:'?'};
      for(let s=1;s<=4;s++){
        const sd = (t.sensors || [])[s-1] || {};
        const h = sd.h || 'unknown';
        const el = document.getElementById('hp'+i+'_'+s);
        if(!el) continue;
        el.classList.remove('ok','noisy','broken','dead','unknown');
        el.classList.add(h);
        el.querySelector('.st').textContent = healthLabel[h];
        el.title = 'baseline='+(sd.bl||0)+'  peak='+(sd.pk||0);
      }
      // New hit since last poll? Push new dot.
      const last = lastSeq[i] || 0;
      if(t.lastHitSeq && t.lastHitSeq > last){
        pushDot(i, t.lastX || 0, t.lastY || 0, t.lastScore || 0);
        lastSeq[i] = t.lastHitSeq;
      }
      drawTarget(i, t);
    }
    document.getElementById('sh').textContent = hits;
    document.getElementById('ss').textContent = score;
    document.getElementById('sa').textContent = active+'/'+N;
    document.getElementById('avgs').innerHTML = (splitN > 0)
        ? Math.round(splitSum/splitN) + ' <small>ms</small>'
        : '— <small>ms</small>';
    document.getElementById('bests').innerHTML = bestSplit !== null
        ? bestSplit + ' <small>ms</small>'
        : '— <small>ms</small>';
    renderFeed(d.recent || []);
    updateCalBar(d);
    updateCalBadges(d.targets || []);
    const ll = document.getElementById('loglines');
    if(ll) ll.textContent = (d.log && typeof d.log.lines === 'number') ? d.log.lines : '0';
  }catch(e){
    document.getElementById('dot').classList.remove('on');
    document.getElementById('conn').textContent = 'Terputus';
  }
}
function renderFeed(recent){
  const f = document.getElementById('feed');
  f.innerHTML = '';
  if(!recent || recent.length === 0){
    f.innerHTML = '<div style="color:var(--mut);font-size:12px;padding:8px">Belum ada tembakan…</div>';
    return;
  }
  for(let k=recent.length-1;k>=0;k--){
    const r = recent[k];
    const row = document.createElement('div');
    row.className = 'row';
    const x = (r.x10/1000).toFixed(2);
    const y = (r.y10/1000).toFixed(2);
    row.innerHTML = `
      <div class="t">T${r.t}</div>
      <div class="s s${r.trig}">S${r.trig}</div>
      <div class="z">Z${r.zone}=${r.score}</div>
      <div class="z">${zoneLabel(r.zone)}</div>
      <div class="xy">x=${x} y=${y}</div>
      <div class="sp">${r.split>0 ? r.split+'ms' : '—'}</div>
    `;
    f.appendChild(row);
  }
}
function zoneLabel(z){
  // 0=5pt centre, 1=4pt, 2=3pt, 3=2pt, 4=1pt, 5=miss
  if(z >= 0 && z <= 4) return ''+(5-z);
  return 'M';
}
async function resetAll(){
  if(!confirm('Reset semua kaunter & dot history?')) return;
  await fetch('/reset', {method:'POST'});
  for(let i=1;i<=N;i++){ clearDots(i); lastSeq[i] = 0; }
}
async function startCal(i){
  if(!confirm('Mula mod kalibrasi Sasaran '+i+'?\n'
    +'Anda akan diminta ketuk S1, S2, S3, S4 satu demi satu.\n'
    +'Hit semasa kalibrasi tidak dikira ke skor.')) return;
  const r = await fetch('/cal/start?target='+i, {method:'POST'});
  if(!r.ok){ alert('Gagal mula kalibrasi: '+r.status); return; }
}
async function cancelCal(){
  await fetch('/cal/cancel', {method:'POST'});
}
async function clearCal(i){
  if(!confirm('Padam kalibrasi Sasaran '+i+'?')) return;
  await fetch('/cal/clear?target='+i, {method:'POST'});
}
async function clearLog(){
  if(!confirm('Padam log sesi (CSV)?')) return;
  await fetch('/log/clear', {method:'POST'});
}
function updateCalBar(d){
  const bar = document.getElementById('calbar');
  const cal = d.cal || {active:false,target:0,step:0,capture:[0,0,0,0]};
  if(!cal.active){
    bar.classList.add('idle');
    return;
  }
  bar.classList.remove('idle');
  document.getElementById('caltitle').textContent =
    'Mod Kalibrasi T'+cal.target+' — Ketuk S'+(cal.step+1)+' sekarang';
  for(let s=1;s<=4;s++){
    const el = document.getElementById('cstep'+s);
    el.classList.remove('active','done');
    if(s-1 < cal.step) el.classList.add('done');
    else if(s-1 === cal.step) el.classList.add('active');
  }
}
function updateCalBadges(targets){
  for(const t of (targets || [])){
    const el = document.getElementById('cal'+t.id);
    if(!el) continue;
    el.classList.remove('on','off');
    if(t.calValid){
      el.classList.add('on');
      el.textContent = 'CAL: ON';
      el.title = 'Ref: '+(t.calRef||[]).join(', ');
    }else{
      el.classList.add('off');
      el.textContent = 'CAL: OFF';
      el.title = 'Belum dikalibrasi';
    }
  }
}
buildGrid();
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
