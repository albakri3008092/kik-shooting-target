# KIK Shooting Target — v2

Upgrade of the original KIK 15-target shooting system. Same hardware
(15 × ESP32 + piezo targets, 1 × ESP32 central receiver, ESP-NOW Long-Range
radio) — with a fully re-written firmware and a new full-colour dashboard.

![dashboard screenshot](docs/dashboard-preview.png)

## What's new in v2

**Firmware**
- Weighted-centroid zone detection from all 4 piezos (not just "first sensor").
- ESP-NOW packets now include **packet IDs + ACK + retransmit** → shots
  don't get silently dropped anymore.
- **Battery voltage** and **RSSI** included in every heartbeat.
- **OTA firmware updates** over WiFi (`kik-central.local`, and per-target
  `kik-target-NN.local`).
- **Remote config**: push threshold / debounce / zone scores from the
  dashboard — no re-upload.
- LittleFS (not SPIFFS) for the central receiver.
- mDNS: reach the dashboard at **`http://kik.local/`**.

**Dashboard** (served by the central from LittleFS)
- Live tab with a per-target circular bullseye and animated hit dots
  from the centroid.
- **Session manager** with PAR time, random-start buzzer, named shooters,
  CSV / JSON export.
- Scoreboard with podium, accuracy %, real-time ranking.
- Health panel with battery bar, RSSI, firmware version, sensor status.
- Session history (persisted on LittleFS) + JSON download.
- Settings page to edit threshold/debounce/zone scores and push to all
  (or a single) target.
- **TV mode** (`/tv.html`) — huge fullscreen leaderboard for demos.
- **Instructor** view (`/instructor.html`) — pick 1..5 targets to watch.
- Bahasa Malaysia default, English toggle.
- Dark / light theme.
- PWA: "Add to Home Screen" on tablet, works offline.

## Repository layout

```
kik-shooting-target/
├── firmware/
│   ├── target_node/         Target ESP32 sketch (flash 15× with TARGET_ID 1..15)
│   ├── central_receiver/    Central ESP32 sketch (flash 1×)
│   │   └── data/            LittleFS payload (copied from dashboard/src by scripts/build_data.sh)
│   └── get_mac/             Utility: prints MAC of any ESP32
├── dashboard/
│   └── src/                 HTML/JS/CSS/icons served from LittleFS
├── simulator/               Node.js server mimicking the central receiver (browser preview, no hardware)
├── docs/
│   ├── SETUP_MY.md          Full step-by-step in Bahasa Malaysia (the original PDF, re-typed)
│   ├── SETUP_EN.md          English translation
│   └── wiring.md            Pinout / wiring diagrams
├── scripts/
│   └── build_data.sh        Copies dashboard/src → firmware/central_receiver/data
└── .github/workflows/       CI (arduino-cli compile + dashboard lint)
```

## Quick start (no hardware, preview the dashboard)

```bash
cd simulator
npm install
npm start
# open http://localhost:8080
```

## Flashing (with hardware)

1. `docs/SETUP_MY.md` has the full step-by-step in **Bahasa Malaysia**.
2. Short version:
   ```bash
   # In Arduino IDE:
   # 1. Install "ESP32 by Espressif Systems" via Board Manager
   # 2. Install libraries: ArduinoJson, AsyncTCP, ESPAsyncWebServer, LittleFS_esp32
   # 3. Flash firmware/get_mac/get_mac.ino to every board, note MACs
   # 4. Edit firmware/target_node/target_node.ino:
   #      - TARGET_ID (1..15)
   #      - RECEIVER_MAC (central MAC)
   #    then flash each target board
   # 5. Edit firmware/central_receiver/central_receiver.ino:
   #      - targetMACs[] (your 15 target MACs)
   #    then flash the central
   # 6. scripts/build_data.sh   (copies dashboard/src -> data/)
   # 7. Tools -> ESP32 Sketch Data Upload (uploads LittleFS)
   ```
3. Power on the central first, then the 15 targets.
4. Connect to WiFi `Target_System` / `12345678`, open `http://kik.local/`
   or `http://192.168.4.1/`.

## License

MIT — see [LICENSE](LICENSE).
