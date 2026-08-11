# ATOVCD — Tablet Operator Console (prototype)

Automated Target Observation & Visual Change Detection: a Raspberry Pi serves a
FastAPI app over its own Wi-Fi, and the tablet just opens a browser — no app to
install, no internet required.

```
Raspberry Pi 5 (+ AI HAT+)          Tablet
┌──────────────────────────┐        ┌──────────────┐
│ camera → engine → SQLite │  Wi-Fi │ Chrome/Edge  │
│ FastAPI :8000  ──────────┼────────┤ http://pi:8000
└──────────────────────────┘        └──────────────┘
```

## Run it (no hardware needed)

```bash
cd atovcd
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
./run.sh            # then open http://localhost:8000/
```

Without a camera the server renders a synthetic range scene with Pillow, so the
whole operator flow (detections, change events, history, reports) is usable on a
laptop. On the Pi, run with `ATOVCD_CAMERA=picamera2` to use the real camera; it
falls back to synthetic frames if `picamera2` is missing.

## Screens

| Tab | Purpose |
|---|---|
| **LIVE** | MJPEG camera feed with AI bounding-box overlay, primary target + confidence, camera/IMU/battery health, NEW / OLD / UNCERTAIN counters |
| **CHANGE MAP** | Plan view of where changes are, colour-coded, plus a live change feed |
| **HISTORY** | Every logged event for any session (time, target, change, confidence, bbox) |
| **REPORT** | Rendered session report + PDF / CSV export |
| **SETTINGS** | Resolution, fps, confidence threshold, change sensitivity, Wi-Fi, storage, battery monitoring |

UI is 16:9 landscape, dark industrial with teal accent, Bahasa Malaysia by
default with an EN toggle.

## API

| Method | Path | Notes |
|---|---|---|
| GET | `/api/status` | Telemetry, counters, targets with normalised bboxes |
| GET | `/api/stream.mjpg` | MJPEG stream at the configured fps |
| GET | `/api/snapshot.jpg` | Single frame |
| GET | `/api/sessions` | Session list with per-state counts |
| POST | `/api/session/start` \| `/api/session/stop` | Session control |
| GET | `/api/history?session_id=&limit=` | Change events, newest first |
| GET | `/api/report?session_id=&format=json\|text\|csv\|pdf` | Session report |
| GET / PUT | `/api/settings` | Read / patch settings (persisted to `data/settings.json`) |

Colour semantics: green = normal/detected, red = new visual change,
grey = historical, amber = uncertain (needs operator review).

## Wiring in the real detector

`app/engine.py` currently drives target state from a simulated tracker so the
operator flow could be built and reviewed before inference lands. Replace
`Engine.analyse()` with the OpenCV/Hailo call and have `Engine.step()` use its
detections — the API, SQLite history and reports need no changes.

## Layout

```
atovcd/
├── app/
│   ├── main.py      FastAPI routes, MJPEG stream, static mount
│   ├── engine.py    target tracking + visual-change state machine
│   ├── camera.py    synthetic renderer / picamera2 source
│   ├── db.py        SQLite sessions + events
│   ├── config.py    settings dataclass persisted as JSON
│   └── report.py    CSV + dependency-free PDF writer
├── web/             index.html + css/ + js/ (served by FastAPI)
├── requirements.txt
└── run.sh
```

`data/` (SQLite + settings) is created at runtime and is not committed.
