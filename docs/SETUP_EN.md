# Setup Guide — KIK Shooting Target System v2

> Step-by-step guide in **English**. For the Bahasa Malaysia version, see
> [`SETUP_MY.md`](SETUP_MY.md).

---

## 1. System Overview

| #    | Module          | Count | Role                                         |
|------|-----------------|-------|----------------------------------------------|
| 1    | Target node     | **15**| ESP32 + 4 piezos, reports hits               |
| 2    | Central receiver| **1** | ESP32, receives hits, serves dashboard, buzzer |
| 3    | Tablet / phone  | 1+    | Displays dashboard at `http://kik.local/`    |

Targets ↔ central: **ESP-NOW Long Range** (no WiFi router needed).
Central creates an Access Point (`SSID=Target_System`, `PSK=12345678`).

---

## 2. Bill of Materials

**Per target (×15):**
- 1 × ESP32 DevKit (30-pin) with antenna
- 4 × 27 mm piezo discs (glued to back of target plate)
- 4 × 1 MΩ resistors (pull-down)
- 2 × 100 kΩ resistors (battery voltage divider — optional)
- 1 × 5 V / 10 000 mAh power bank
- JST / Dupont wires, silicone glue, heavy-duty double-sided tape

**Central (×1):**
- 1 × ESP32 DevKit
- 1 × passive piezo buzzer (or small speaker)
- 1 × enclosure + power switch
- 1 × power bank or 5 V supply

**Tablet / phone:**
- Any modern browser (Chrome / Safari latest)

---

## 3. Software Installation (one-time)

### 3.1 Arduino IDE

1. Download Arduino IDE 2.x from <https://www.arduino.cc/en/software>
2. `File → Preferences → Additional board manager URLs`, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools → Board → Boards Manager` → find **esp32** → **Install** (v2.0.17+).

### 3.2 Libraries

`Sketch → Include Library → Manage Libraries`:

- **ArduinoJson** (Benoit Blanchon) — v7 or newer
- **AsyncTCP** (me-no-dev)
- **ESPAsyncWebServer** (me-no-dev)

LittleFS is already bundled with the ESP32 core.

### 3.3 LittleFS Upload Plugin

1. Grab the jar from
   <https://github.com/earlephilhower/arduino-esp32fs-plugin/releases>
2. Extract to `~/Arduino/tools/` (create the folder if missing).
3. Restart Arduino IDE → you should see `Tools → ESP32 Sketch Data Upload`.

---

## 4. Clone the Repo

```bash
git clone https://github.com/albakri3008092/kik-shooting-target.git
cd kik-shooting-target
```

---

## 5. Step 1: Read the MAC of Every Board

1. Open `firmware/get_mac/get_mac.ino`.
2. `Tools → Board → ESP32 Dev Module`, correct COM port.
3. Upload.
4. `Tools → Serial Monitor` at **115200 baud** → MAC will be printed.
5. **Label the physical board** (masking tape, Sharpie): `T01`, `T02`, …
   `T15`, `CENTRAL`.
6. Write all 16 MACs into a table:

| Label     | MAC                 |
|-----------|---------------------|
| Central   | 24:6F:28:XX:XX:XX   |
| Target 1  | 24:6F:28:AA:BB:01   |
| …         | …                   |
| Target 15 | 24:6F:28:AA:BB:15   |

---

## 6. Step 2: Flash the Central Receiver

1. Open `firmware/central_receiver/central_receiver.ino`.
2. Edit the `targetMACs[]` table near the top:
   ```cpp
   static uint8_t targetMACs[NUM_TARGETS][6] = {
     {0x24,0x6F,0x28,0xAA,0xBB,0x01}, // Target 1
     ...
     {0x24,0x6F,0x28,0xAA,0xBB,0x0F}, // Target 15
   };
   ```
3. Plug the **central** ESP32 into USB, pick the correct port.
4. **Upload**.

### Upload the Dashboard to LittleFS

```bash
./scripts/build_data.sh
```

In Arduino IDE:
- `Tools → Partition Scheme → Default 4MB with spiffs`
  (or `Huge APP` if the sketch is too big).
- `Tools → ESP32 Sketch Data Upload`.

---

## 7. Step 3: Flash All 15 Target Nodes

For **each** physical board (1 → 15):

1. Open `firmware/target_node/target_node.ino`.
2. Edit these two lines:
   ```cpp
   #define TARGET_ID    1
   static uint8_t RECEIVER_MAC[6] = { 0x24,0x6F,0x28,0xXX,0xXX,0xXX };
   ```
3. Upload.
4. Change `TARGET_ID` to `2`, plug in the next board, upload.
5. Repeat until `TARGET_ID = 15`.

---

## 8. Step 4: Piezo Wiring

See [`docs/wiring.md`](wiring.md). Summary:

```
  Piezo Sn (+) ─── GPIO (32/33/34/35) ──── 1 MΩ ──── GND
  Piezo Sn (–) ─── GND
```

Layout (back of target plate):

```
   S1 (10 pts)     S2 (8 pts)
            🎯
   S3 (6 pts)      S4 (4 pts)
```

Use **thick double-sided tape**; glue too rigid will dampen vibration.

---

## 9. Step 5: Power-On Test

1. Power on the **central** first.
2. Power on all **15 targets**.
3. Connect a tablet to WiFi `Target_System` / `12345678`.
4. Open **<http://kik.local/>** (or `http://192.168.4.1/`).
5. Health tab should show all 15 targets **Online**.
6. Tap each target lightly to confirm all 4 piezos register hits.

---

## 10. Dashboard Usage

### 10.1 Live tab
Grid of 15 target cards with bullseye + live dots from centroid.

### 10.2 Scores tab
Leaderboard with podium 🥇🥈🥉 + accuracy percentage.

### 10.3 Health tab
Battery, RSSI, firmware version, sensor status.

### 10.4 Session tab
Named sessions with PAR timer, random-start buzzer, shot log table,
score-over-time chart, zone distribution, CSV/JSON export.

### 10.5 History tab
Previous sessions stored on LittleFS — download as JSON.

### 10.6 Settings tab
Edit threshold / debounce / zone scores and push live to all targets.
Add shooter profiles. "Identify" button beeps a specific target.

### 10.7 TV mode
Open `/tv.html` on a big screen for a fullscreen leaderboard.

---

## 11. Over-the-Air Firmware Updates

### From Arduino IDE

1. Computer connected to `Target_System` WiFi.
2. `Tools → Port` lists network ports:
   - `kik-central at 192.168.4.1`
   - `kik-target-01 at 192.168.4.xx`
   - …
3. Select and Upload.
4. OTA password: **`kik-ota`**.

### From command line

```bash
pip install esptool
python -m espota -i 192.168.4.1 -p 3232 -r -f \
  -a kik-ota \
  ~/Arduino/build/esp32.esp32.esp32/central_receiver.ino.bin
```

---

## 12. Troubleshooting

| Problem                           | Fix                                                     |
|-----------------------------------|---------------------------------------------------------|
| `kik.local` won't resolve         | Use the IP directly: `http://192.168.4.1/`              |
| A target goes offline after 15s   | Reset power, verify MAC table, check antenna            |
| No hits registered                | Raise threshold in Settings; check piezo wires          |
| Dashboard is blank                | Re-run `build_data.sh` then re-run Data Upload          |
| Too many false hits               | Raise debounce to 200-300 ms                            |
| Battery drains quickly            | Verify divider resistors; default sleeps after 30 min idle |

---

## 13. Development / Browser Preview

You can preview the dashboard without any hardware using the bundled
Node.js simulator:

```bash
cd simulator
npm install
npm start
# open http://localhost:8080
```

See [`simulator/README.md`](../simulator/README.md).

Happy shooting! 🎯
