# KIK Target — Demo (Ringkas / Simple)

Versi **mudah** — hanya 1 kotak pusat + 3 sasaran, tiada library luar, tiada
langkah upload LittleFS. Untuk versi penuh (15 sasaran, OTA, sesi,
leaderboard, dsb.), lihat folder `firmware/` + `dashboard/` di root repo.

_A **minimal** version — 1 central + 3 targets, no external libraries, no
LittleFS upload step. For the full v2 system (15 targets, OTA, sessions,
leaderboard, etc.) see `firmware/` + `dashboard/` at the repo root._

---

## 🇲🇾 Bahasa Malaysia

### Barang yang diperlukan

- 4 × ESP32 DevKit (1 untuk pusat, 3 untuk sasaran)
- 12 × piezo disc 27 mm (4 untuk setiap sasaran)
- 12 × perintang 1 MΩ (pull-down piezo)
- 1 × buzzer pasif 5V (pilihan, untuk bunyi bila kena)
- Power bank / kuasa 5V untuk setiap board

### Langkah (3 langkah sahaja)

**1. Flash pusat (1 kali):**
- Buka Arduino IDE.
- Buka `demo/central_demo/central_demo.ino`.
- `Tools → Board → ESP32 Dev Module`.
- Pilih port yang betul, tekan Upload.
- Buka Serial Monitor 115200. Catat **MAC** yang dicetak
  (contoh: `A0:B7:65:12:34:56`).

**2. Flash sasaran (3 kali — sekali untuk setiap board):**
- Buka `demo/target_demo/target_demo.ino`.
- Ubah **dua** baris di atas:
  ```cpp
  #define TARGET_ID    1
  static uint8_t RECEIVER_MAC[6] = { 0xA0, 0xB7, 0x65, 0x12, 0x34, 0x56 };
  ```
- Sambung Target 1, Upload.
- Tukar `TARGET_ID = 2`, sambung Target 2, Upload.
- Tukar `TARGET_ID = 3`, sambung Target 3, Upload.

**3. Uji:**
- Hidupkan pusat dahulu, kemudian 3 sasaran.
- Phone / tablet → Wi-Fi → **Target_System** (password **12345678**).
- Pelayar → **<http://192.168.4.1/>**.
- Ketuk sasaran — kiraan, skor dan titik akan muncul serta-merta.

### Apa yang dipapar pada tablet

Setiap sasaran ada kad sendiri dengan:

1. **🎯 Sasaran X** + skor kumulatif (pill merah di kanan).
2. **Bullseye** — setiap peluru letakkan titik warna di kuadran yang kena.
3. **SENSOR TERAKHIR** — badge besar (S1 / S2 / S3 / S4) tunjuk piezo
   mana paling kuat mengesan tembakan terakhir, dengan warna zon yang
   sepadan + masa berlalu.
4. **S1..S4 / Zon 10..4** — kiraan per sensor (kuadran yang pulse
   semasa kena).

Di bawah tiga kad itu ada **🔴 Tembakan Terkini** — log 16 tembakan
terakhir merentas semua 3 sasaran, dalam bentuk:

```
T2   S3   Zon 6   +6   2s
T1   S1   Zon 10  +10  baru
```

Ini yang paling penting semasa bentang: juri dapat lihat *sensor mana*
(S1–S4) pada *sasaran mana* (T1–T3) yang mengesan setiap peluru, secara
langsung.

### Pendawaian piezo (setiap sasaran)

```
  Piezo S1 (+) → GPIO 32 ──── 1 MΩ ──── GND
  Piezo S2 (+) → GPIO 33 ──── 1 MΩ ──── GND
  Piezo S3 (+) → GPIO 34 ──── 1 MΩ ──── GND
  Piezo S4 (+) → GPIO 35 ──── 1 MΩ ──── GND
  Semua piezo (–) → GND
```

Susunan di belakang plat sasaran:

```
    S1 (Zon 10)       S2 (Zon 8)
              🎯
    S3 (Zon 6)        S4 (Zon 4)
```

### Pendawaian buzzer (pilihan, pada pusat sahaja)

```
  Buzzer (+) → GPIO 25
  Buzzer (–) → GND
```

Jika tiada buzzer, biar sahaja — tidak akan rosak.

---

## 🇬🇧 English

### What you need

- 4 × ESP32 DevKits (1 central + 3 targets)
- 12 × 27 mm piezo discs (4 per target)
- 12 × 1 MΩ resistors (piezo pull-down)
- 1 × passive 5 V buzzer (optional, for hit beep)
- Power source (USB power bank) for each board

### Steps (only 3)

**1. Flash the central (once):**
- Arduino IDE → open `demo/central_demo/central_demo.ino`.
- `Tools → Board → ESP32 Dev Module`, pick the port, **Upload**.
- Open Serial Monitor @ 115200. Note the **MAC** printed
  (e.g. `A0:B7:65:12:34:56`).

**2. Flash 3 target boards:**
- Open `demo/target_demo/target_demo.ino`.
- Edit the two lines at the top:
  ```cpp
  #define TARGET_ID    1
  static uint8_t RECEIVER_MAC[6] = { 0xA0, 0xB7, 0x65, 0x12, 0x34, 0x56 };
  ```
- Upload to Target 1.
- Change `TARGET_ID = 2`, upload to Target 2.
- Change `TARGET_ID = 3`, upload to Target 3.

**3. Test:**
- Power on the central first, then the 3 targets.
- Phone / tablet → Wi-Fi → **Target_System** (pwd **12345678**).
- Browser → **<http://192.168.4.1/>**.
- Tap a target — the hit count, score, and dot appear instantly.

### What the tablet shows

Each target has its own card with:

1. **🎯 Target X** + cumulative score (red pill, right).
2. **Bullseye** — every shot drops a colour dot in the triggered quadrant.
3. **SENSOR TERAKHIR / LAST SENSOR** — big badge (S1 / S2 / S3 / S4)
   showing which piezo detected the latest shot most strongly, coloured
   to match the zone, with "time ago".
4. **S1..S4 / Zone 10..4** — per-sensor counters (pulse on hit).

Below the three cards: **🔴 Tembakan Terkini (Recent shots)** — a log
of the last 16 hits across all targets:

```
T2   S3   Zone 6   +6   2s
T1   S1   Zone 10  +10  just now
```

That's the key judge-facing view during a demo: audience can see
*which sensor* (S1–S4) on *which target* (T1–T3) caught every bullet,
live.

### Piezo wiring (per target)

Same as the Bahasa Malaysia section above. Layout on the back of each
target plate:

```
    S1 (zone 10)      S2 (zone 8)
              🎯
    S3 (zone 6)       S4 (zone 4)
```

### Buzzer (optional, on the central only)

- `Buzzer (+) → GPIO 25`
- `Buzzer (–) → GND`

Leave disconnected if you don't have one — nothing bad happens.

---

## Troubleshooting

| Problem                         | Fix                                                |
|---------------------------------|----------------------------------------------------|
| Wi-Fi SSID `Target_System` not visible | Power-cycle the central; check serial monitor     |
| A target shows "Offline"         | Verify you set the correct `RECEIVER_MAC`, reboot |
| No hits registered              | Lower `THRESHOLD` in `target_demo.ino`, re-flash  |
| Too many false hits             | Raise `DEBOUNCE_MS` to 200–300 ms                 |
| Dashboard won't load            | Use IP `http://192.168.4.1/` (not `kik.local`)    |

When the demo works and you're ready for the full 15-target system with
OTA, sessions, leaderboard etc., follow the guide at
[`docs/SETUP_MY.md`](../docs/SETUP_MY.md) / [`docs/SETUP_EN.md`](../docs/SETUP_EN.md).
