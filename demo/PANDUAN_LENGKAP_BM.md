# 🎯 PANDUAN LENGKAP — KIK Shooting Target (Prototaip Demo)

**Projek inovasi: Sistem Sasaran Menembak Pintar dengan Pengesanan Peluru**

- 1 ESP32 **Pusat** (Central) — WiFi AP + dashboard tablet
- 3 ESP32 **Sasaran** — tiap satu dengan **4 piezo**
- **Jumlah: 12 piezo** (4 × 3 sasaran)
- Komunikasi wireless: **ESP-NOW** (tak perlu router)
- Paparan: mana-mana tablet/telefon pada Wi-Fi `Target_System`

> **Bentang esok?** Ikut seksyen **[✅ Check-list sebelum bentang](#-check-list-sebelum-bentang)** dan **[🎬 Skrip bentang 3 minit](#-skrip-bentang-3-minit)** di hujung.

---

## 📋 Isi Kandungan

1. [Barang yang diperlukan (BOM)](#-barang-yang-diperlukan-bom)
2. [Perisian yang perlu dipasang](#-perisian-yang-perlu-dipasang)
3. [Pendawaian setiap sasaran (4 piezo)](#-pendawaian-setiap-sasaran-4-piezo)
4. [Pendawaian pusat (buzzer pilihan)](#-pendawaian-pusat-buzzer-pilihan)
5. [Langkah 1 — Dapatkan MAC address setiap ESP32](#langkah-1--dapatkan-mac-address-setiap-esp32)
6. [Langkah 2 — Flash ESP32 PUSAT (1 kali sahaja)](#langkah-2--flash-esp32-pusat-1-kali-sahaja)
7. [Langkah 3 — Flash ESP32 SASARAN (3 kali)](#langkah-3--flash-esp32-sasaran-3-kali)
8. [Langkah 4 — Hidupkan sistem & sambung tablet](#langkah-4--hidupkan-sistem--sambung-tablet)
9. [Langkah 5 — Uji tembakan](#langkah-5--uji-tembakan)
10. [Apa yang dipapar pada tablet](#-apa-yang-dipapar-pada-tablet)
11. [Check-list sebelum bentang](#-check-list-sebelum-bentang)
12. [Skrip bentang 3 minit](#-skrip-bentang-3-minit)
13. [Troubleshooting (penyelesaian masalah)](#-troubleshooting-penyelesaian-masalah)
14. [Soalan juri & jawapan](#-soalan-juri--jawapan)

---

## 🛒 Barang yang diperlukan (BOM)

### Elektronik
| Bil. | Barang | Kuantiti | Nota |
|------|--------|----------|------|
| 1 | ESP32 DevKit v1 (atau setara) | **4 papan** | 1 pusat + 3 sasaran |
| 2 | Piezo disc 27 mm (atau 20-35 mm) | **12 biji** | **4 setiap sasaran × 3 = 12** |
| 3 | Perintang 1 MΩ | 12 biji | Pull-down untuk setiap piezo |
| 4 | Kabel USB Micro-B (ESP32) | 4 batang | Untuk flash & bekalan kuasa |
| 5 | Power bank / bateri 5V | 4 biji | Bekalan kuasa setiap board |
| 6 | Buzzer pasif 5V | 1 biji *(pilihan)* | Bunyi bila kena (pusat) |
| 7 | Wayar jumper (F-F, M-F) | Satu set | Penyambungan piezo ke GPIO |
| 8 | Breadboard kecil | 3 biji *(pilihan)* | Pasang perintang |
| 9 | Solder + flux | 1 set | Untuk piezo disc (pin kecil) |

### Mekanikal (papan sasaran)
| Bil. | Barang | Kuantiti |
|------|--------|----------|
| 10 | Plat plywood / akrilik 30×30 cm | 3 keping |
| 11 | Gam panas / gam epoksi | 1 tiub |
| 12 | Skru & stand (rak kayu / tripod) | 3 set |

### Lain-lain
| Bil. | Barang | Kuantiti |
|------|--------|----------|
| 13 | **Tablet / telefon** dengan pelayar web | 1 unit |
| 14 | Laptop untuk flash Arduino IDE | 1 unit |

---

## 💻 Perisian yang perlu dipasang

### 1. Arduino IDE
- Muat turun dari **<https://www.arduino.cc/en/software>**
- Windows, macOS, atau Linux — semua boleh.

### 2. ESP32 Board Manager
1. Buka Arduino IDE → `File → Preferences`
2. Dalam **Additional Board Manager URLs** tampal:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Klik **OK**.
4. `Tools → Board → Boards Manager…`
5. Cari **`esp32`** → pilih oleh *Espressif Systems* → **Install** (boleh versi 2.0.17 ATAU 3.1.3 — kedua-dua support).

### 3. Pilih board
- `Tools → Board → ESP32 Arduino → ESP32 Dev Module`
- `Tools → Upload Speed → 921600`
- `Tools → Port → (COM# yang muncul bila ESP32 dicucuk)`

> 💡 **Tiada library luar diperlukan** untuk demo ini. Semua guna pustaka dalaman ESP32.

---

## 🔌 Pendawaian setiap sasaran (4 piezo)

**Setiap sasaran** (ulang 3 kali untuk 3 papan):

```
                    ┌─────────────────────┐
   Piezo S1 (+) ───→│ GPIO 32             │
                    │         ESP32       │
   Piezo S2 (+) ───→│ GPIO 33   SASARAN   │
                    │                     │
   Piezo S3 (+) ───→│ GPIO 34             │
                    │                     │
   Piezo S4 (+) ───→│ GPIO 35             │
                    │                     │
                    │ GND ────┬───────────│
                    └─────────┼───────────┘
                              │
        Piezo 1..4 (–) ───────┤
                              │
       Setiap piezo (+):      │
       piezo(+) ── 1 MΩ ──────┘  (pull-down)
```

### Susunan 4 piezo di belakang plat sasaran

Lekatkan piezo pada **4 kuadran** plat sasaran dengan gam panas:

```
    ┌────────────────────────┐
    │                        │
    │   S1              S2   │
    │  (Zon 10)       (Zon 8) │
    │                        │
    │          🎯            │
    │       (tengah)         │
    │                        │
    │   S3              S4   │
    │  (Zon 6)        (Zon 4) │
    │                        │
    └────────────────────────┘
           (belakang plat)
```

### Jadual pin GPIO (setiap sasaran)

| Piezo | Zon | Skor | GPIO | Perintang pull-down |
|-------|-----|------|------|---------------------|
| S1    | 10  | 10   | **32** | 1 MΩ ke GND |
| S2    | 8   | 8    | **33** | 1 MΩ ke GND |
| S3    | 6   | 6    | **34** | 1 MΩ ke GND |
| S4    | 4   | 4    | **35** | 1 MΩ ke GND |

> ⚠️ GPIO 34 & 35 adalah **input sahaja** pada ESP32 — kita hanya baca (analogRead), jadi OK.

### Cara sambung fizikal (ringkas)

1. Solder wayar pendek pada **kedua-dua muka piezo disc**:
   - Muka kuprum (besar) = **(+)** → ke GPIO
   - Muka perak tengah   = **(–)** → ke GND
2. Untuk setiap piezo, pasang **1 MΩ** antara GPIO ↔ GND (pull-down).
3. Lekat piezo dengan gam panas pada belakang plat, di posisi 4 kuadran.

> 💡 **Total pendawaian**: 3 sasaran × (4 piezo + 4 perintang + 1 wayar GND) = **12 piezo, 12 perintang**.

---

## 🔊 Pendawaian pusat (buzzer pilihan)

Jika awak ada buzzer pasif 5V:

```
   Buzzer (+) ───→ GPIO 25 (ESP32 PUSAT)
   Buzzer (–) ───→ GND
```

Jika tiada buzzer — biarkan sahaja, tak rosak apa-apa.

---

## Langkah 1 — Dapatkan MAC address setiap ESP32

Kita perlu **MAC** pusat supaya sasaran tahu kepada siapa nak hantar data.

### 1.1 Flash sketsa `get_mac` ke **setiap** ESP32 (4 kali)

1. Buka Arduino IDE → `File → New`
2. **COPY kod di bawah, PASTE dalam window baru:**

```cpp
// =====================================================================
//  get_mac.ino — Print MAC address ke Serial Monitor
// =====================================================================
//  Flash sketsa ini sekali ke setiap ESP32 (pusat + 3 sasaran),
//  catat MAC yang dipaparkan, lekat label pada board (Pusat / T1 / T2 / T3).
// =====================================================================
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(100);
  Serial.println();
  Serial.println("=================================");
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=================================");
  Serial.println("Labelkan board ini (Pusat / T1 / T2 / T3).");
}

void loop() {}
```

3. Simpan (`Ctrl+S`) sebagai `get_mac.ino`.
4. Cucuk ESP32 pertama ke USB laptop.
5. `Tools → Port → COM#` (port yang muncul baru).
6. Klik **→** (Upload). Tunggu "Done uploading".
7. `Tools → Serial Monitor` (baud **115200**).
8. Tekan butang **EN** (reset) pada ESP32.
9. Catat MAC yang dipaparkan, contoh:
   ```
   MAC Address: A0:B7:65:12:34:56
   ```
10. Tampal pelekat pada board → tulis `Pusat A0:B7:65:12:34:56`.

**Ulang langkah 4-10 untuk 3 ESP32 sasaran** → label T1 / T2 / T3.

### 1.2 Susun 4 MAC dalam jadual

| Role   | MAC Address              |
|--------|--------------------------|
| Pusat  | `A0:B7:65:__:__:__` ← isi |
| T1     | `A0:B7:65:__:__:__`      |
| T2     | `A0:B7:65:__:__:__`      |
| T3     | `A0:B7:65:__:__:__`      |

**Yang penting:** MAC **Pusat** sahaja — kita akan tampal dalam sketsa sasaran nanti.

---

## Langkah 2 — Flash ESP32 PUSAT (1 kali sahaja)

### 2.1 Dapatkan kod pusat

Fail penuh ada di repo: **[`demo/central_demo/central_demo.ino`](central_demo/central_demo.ino)**.

Cara paling mudah untuk cut-and-paste:

1. Buka <https://github.com/albakri3008092/kik-shooting-target/blob/devin/1776839563-demo-show-sensor/demo/central_demo/central_demo.ino>
2. Klik butang **`Raw`** (kanan atas pembaca fail).
3. `Ctrl+A` → `Ctrl+C` (salin semua).
4. Arduino IDE → `File → New` → `Ctrl+A` → `Ctrl+V` (tampal menggantikan).
5. `File → Save As…` → nama folder **`central_demo`** → simpan sebagai `central_demo.ino`.

> Alternatif: klon repo: `git clone https://github.com/albakri3008092/kik-shooting-target.git`, kemudian buka `demo/central_demo/central_demo.ino` terus dalam Arduino IDE.

### 2.2 Upload ke ESP32 pusat

1. Cucuk ESP32 **Pusat** ke USB laptop.
2. `Tools → Port → COM#`.
3. `Tools → Board → ESP32 Arduino → ESP32 Dev Module`.
4. Klik **→** (Upload). Tunggu "Done uploading" (kira-kira 30-60 saat).
5. `Tools → Serial Monitor` (baud **115200**).
6. Tekan **EN** pada ESP32. Akan muncul:
   ```
   KIK Central DEMO ready.
   MAC:      A0:B7:65:12:34:56
   AP SSID:  Target_System
   AP IP:    192.168.4.1
   Open http://192.168.4.1/ on your phone.
   ```
7. **SAH: MAC pusat sama seperti yang dicatat di Langkah 1.2.**

Cabut ESP32 pusat — tak perlu sentuh lagi.

---

## Langkah 3 — Flash ESP32 SASARAN (3 kali)

Sketsa **sama** untuk 3 sasaran. Cuma **2 baris perlu ubah** setiap kali flash.

### 3.1 COPY kod sasaran — tampal dalam Arduino IDE

```cpp
// =====================================================================
//  target_demo.ino — ESP32 SASARAN (flash 3 kali, tukar TARGET_ID)
// =====================================================================
//  Flash SKETSA SAMA ini ke 3 papan ESP32 sasaran. Sebelum setiap upload,
//  tukar HANYA DUA perkara:
//     1. TARGET_ID       (1, 2, 3 — unik setiap board)
//     2. RECEIVER_MAC[]  (MAC pusat dari Langkah 1.2)
//  Tiada library luar diperlukan — ESP32 core sahaja.
// =====================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- TUKAR 2 BARIS INI SETIAP KALI FLASH ----------
#define TARGET_ID    1                                     // 1, 2, atau 3
static uint8_t RECEIVER_MAC[6] = { 0xA0, 0xB7, 0x65, 0x00, 0x00, 0x00 };
// ---------------------------------------------------------

// Pendawaian piezo: satu piezo per GPIO, 1 MΩ pull-down ke GND.
static const uint8_t  PIEZO_PINS[4] = { 32, 33, 34, 35 };
static const uint16_t THRESHOLD     = 1500;   // Ambang ADC untuk detect hit
static const uint16_t DEBOUNCE_MS   = 150;    // Debounce per sensor
static const uint16_t HB_INTERVAL   = 5000;   // Heartbeat setiap 5 s

// Skor zon per sensor (S1=10, S2=8, S3=6, S4=4).
static const uint8_t ZONE_SCORES[4] = { 10, 8, 6, 4 };

#pragma pack(push, 1)
struct HitPacket {
  uint8_t  type;        // 1 = hit, 2 = heartbeat
  uint8_t  targetID;
  uint8_t  sensorID;    // 1..4
  uint8_t  zone;        // 1..4
  uint8_t  score;       // 10 / 8 / 6 / 4
  uint16_t total;       // Jumlah hit papan ini
  uint16_t amp;         // Amplitud piezo yang trigger
};
#pragma pack(pop)

static uint16_t hitTotal = 0;
static uint32_t lastFired[4] = {0, 0, 0, 0};
static uint32_t lastHeartbeat = 0;

static void onSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
  digitalWrite(2, status == ESP_NOW_SEND_SUCCESS ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);
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
  p.zone     = sensorID;
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
```

### 3.2 Format MAC pusat

Sebelum flash, **tukar baris 14** dengan MAC pusat awak.

MAC awak dari Langkah 1.2, contoh `A0:B7:65:12:34:56`, perlu ditulis dalam format:

```cpp
static uint8_t RECEIVER_MAC[6] = { 0xA0, 0xB7, 0x65, 0x12, 0x34, 0x56 };
```

> Setiap pasangan hex tukar jadi `0xXX`, dipisahkan koma. Huruf besar/kecil OK.

### 3.3 Flash 3 kali

**Sasaran 1:**
1. Ubah `#define TARGET_ID    1`
2. Pastikan `RECEIVER_MAC[]` sudah betul (MAC pusat).
3. Cucuk ESP32 T1 → `Tools → Port → COM#` → Upload.
4. Serial Monitor: akan nampak `Target 1 ready. MAC=...`
5. Cabut T1.

**Sasaran 2:**
1. Ubah `#define TARGET_ID    2`
2. Cucuk ESP32 T2 → pilih port → Upload.
3. Serial Monitor: `Target 2 ready...` → cabut.

**Sasaran 3:**
1. Ubah `#define TARGET_ID    3`
2. Cucuk ESP32 T3 → Upload.
3. `Target 3 ready...` → cabut.

> 🎉 Sampai sini, 4 board semua sudah di-flash. Jangan sentuh laptop lagi.

---

## Langkah 4 — Hidupkan sistem & sambung tablet

### 4.1 Hidupkan board

**Susunan hidupkan PENTING:**

1. **Hidupkan PUSAT dahulu** (cucuk power bank/USB).
   - LED biru ESP32 menyala.
   - Dalam 2 saat, WiFi AP `Target_System` akan aktif.
2. **Hidupkan 3 SASARAN** (boleh bersama atau seorang-seorang).
   - LED GPIO 2 akan kelip bila paket dihantar berjaya.

### 4.2 Sambung tablet

1. Buka **Tetapan Wi-Fi** pada tablet.
2. Cari rangkaian **`Target_System`**.
3. Tekan Connect → kata laluan: **`12345678`**
4. Tablet akan kata "**No internet**" — ini NORMAL (ESP32 bukan router).
5. Buka pelayar (Chrome / Safari).
6. Pergi ke: **`http://192.168.4.1/`**

> 💡 **Jika dashboard tak muncul**: pastikan awak taip `http://` di depan. Kadangkala pelayar anggap nama carian Google.

### 4.3 Sahkan semua 3 sasaran online

Pada dashboard, awak akan nampak:
- Header: **🟢 Bersambung**
- Kad "Sasaran Aktif" papar **3/3**
- 3 kad sasaran (Sasaran 1 / 2 / 3) berwarna terang (tidak pudar).

Jika ada sasaran "pudar" (off), ia Offline — semak bekalan kuasa atau MAC pusat.

---

## Langkah 5 — Uji tembakan

Mula-mula **uji dengan ketuk** (bukan tembak peluru sebenar dulu):

1. **Ketuk plat Sasaran 1 di kawasan S1 (atas kiri)** dengan hujung jari / kunci.
2. Dalam <300 ms, awak akan nampak pada tablet:
   - Kad Sasaran 1 **berkedip hijau** (hit flash).
   - Titik merah **(+dot)** muncul di kuadran atas kiri bullseye.
   - Badge **SENSOR TERAKHIR: S1** (merah) + "baru".
   - Kotak **S1 · Zon 10** nombor naik 0 → 1, kotak berkelip.
   - Feed bawah tambah baris: `T1  S1  Zon 10  +10  baru`
   - Buzzer bunyi "beep" 40 ms (jika dipasang).
3. **Ketuk S2** (atas kanan) — nombor **S2 · Zon 8** naik, badge kuning.
4. **Ketuk S3** (bawah kiri) — **S3 · Zon 6**, badge biru.
5. **Ketuk S4** (bawah kanan) — **S4 · Zon 4**, badge hijau.
6. Ulang untuk Sasaran 2 dan 3.

### Masalah biasa pada peringkat ini

| Masalah | Penyelesaian |
|---------|--------------|
| Tiada hit langsung | Pukul lebih kuat, atau turunkan `THRESHOLD` dari 1500 ke **800** dalam sketsa dan re-flash. |
| 1 ketuk didaftar 2-3 kali | Naikkan `DEBOUNCE_MS` dari 150 ke **250-300** dan re-flash. |
| Ketuk S1 tapi daftar S2 | Piezo S1 dan S2 terlalu dekat. Tambah getah/busa antara piezo dan plat. |
| Kad pudar ("Offline") | Bateri habis, atau MAC pusat salah dalam target_demo. |

> ✅ Bila semua 12 piezo bertindak balas dengan betul → **sistem sedia untuk bentang**.

---

## 📱 Apa yang dipapar pada tablet

![dashboard](https://app.devin.ai/attachments/8c353ab4-3a5c-4788-b469-ccb12ef46dc1/dashboard-preview.png)

### Kawasan atas (ringkasan keseluruhan)
- **JUMLAH TEMBAKAN** — kiraan peluru kena rentas semua 3 sasaran
- **JUMLAH SKOR** — jumlah skor (10/8/6/4) terkumpul
- **SASARAN AKTIF** — `X/3` bilangan sasaran online

### 3 kad sasaran (setiap satu)
- **🎯 Sasaran N** + skor kumulatif (pill merah)
- **Bullseye SVG** — titik berwarna setiap peluru, diletakkan di kuadran sensor yang trigger
- **🆕 SENSOR TERAKHIR** — badge besar S1/S2/S3/S4 dengan warna zon padan + masa berlalu
- **S1..S4 · Zon 10..4** — 4 kotak kiraan per piezo, berkelip bila trigger

### Feed bawah
- **🔴 Tembakan Terkini** — 16 tembakan terakhir, format:
  ```
  T2  S3  Zon 6   +6   2s
  T1  S1  Zon 10  +10  baru
  ```
- Juri boleh lihat secara langsung **sensor mana** (S1–S4) pada **sasaran mana** (T1–T3) detect setiap peluru.

### Butang Reset
- Tekan **↻ Reset** (kanan atas) → kosongkan semua kiraan + feed.

---

## ✅ Check-list sebelum bentang

Malam sebelum / pagi hari bentang, semak semua:

- [ ] **4 ESP32** semua sudah di-flash dengan sketsa betul
- [ ] **TARGET_ID** unik setiap sasaran (1, 2, 3)
- [ ] **RECEIVER_MAC** dalam sasaran = MAC pusat sebenar
- [ ] **12 piezo** sudah dilekat dengan betul (4 kuadran × 3 plat)
- [ ] **12 perintang 1 MΩ** sudah pull-down setiap piezo
- [ ] **Bekalan kuasa** (4 power bank) dicas penuh
- [ ] **Tablet** sudah bookmark `http://192.168.4.1/`
- [ ] **Tablet** dah pernah sambung ke `Target_System` (auto-connect)
- [ ] Uji live sekali: hidupkan + ketuk setiap piezo → sahkan badge + feed jalan
- [ ] **Buzzer** (pilihan) dipasang pada pusat GPIO 25 & GND
- [ ] Printout panduan ini (pelan B kalau laptop mati)

---

## 🎬 Skrip bentang 3 minit

### Minit 1: Pengenalan (30s)
> "Sistem kami **KIK Shooting Target** mengesan peluru pada sasaran
> dan papar hasilnya secara langsung pada tablet — tanpa internet,
> tanpa wayar antara sasaran."

**Tunjuk** pengaturcara: 3 sasaran bersusun, setiap satu ada 4 piezo di
belakang = **12 sensor keseluruhan**.

### Minit 1.5: Hidup sistem (20s)

1. Hidupkan pusat → tunggu 2 saat.
2. Hidupkan 3 sasaran.
3. Tablet → sambung `Target_System` → buka `http://192.168.4.1/`.
4. Tunjuk kepada juri: **3/3 Sasaran Aktif** di atas.

### Minit 2: Demonstrasi (60s)

1. **Ketuk Sasaran 1 di S1** (atas kiri plat).
   > "Piezo S1 detect — badge **SENSOR TERAKHIR: S1** warna merah muncul,
   > feed bawah rekod: **T1 S1 Zon 10 +10 baru**. Skor +10."
2. **Ketuk Sasaran 2 di S2**.
   > "Sensor S2 sasaran 2, zon 8, +8 markah. Lihat feed — sekarang ada
   > dua baris. Tablet boleh jejak semua 12 piezo serentak."
3. **Ketuk Sasaran 3 di S4**.
4. Tunjuk bullseye: titik muncul di 4 kuadran berlainan.

### Minit 3: Inovasi & nilai tambah (30s)

> "Inovasi utama:
> - **Wireless ESP-NOW** — latency <5 ms, tak perlu WiFi router.
> - **Identifikasi sensor** — tablet nyatakan **piezo mana yang kena**,
>   bukan setakat 'kena sasaran'. Juri boleh audit setiap tembakan.
> - **Skalabiliti** — boleh kembang ke 15 sasaran, OTA firmware
>   update, sesi senaman tempoh masa, dan eksport CSV/JSON."

### Reset antara sesi
Tekan butang **↻ Reset** — kiraan kembali 0, feed kosong.

---

## 🛠️ Troubleshooting (penyelesaian masalah)

| Masalah | Sebab berkemungkinan | Penyelesaian |
|---------|---------------------|--------------|
| Wi-Fi `Target_System` tiada | Pusat belum hidup / crashed | Kitar kuasa pusat. Tunggu 10 s. Semak Serial Monitor. |
| Dashboard tak load di tablet | URL salah, atau cache pelayar | Taip `http://192.168.4.1/` (termasuk `http://`). `Ctrl+Shift+R` refresh. |
| Sasaran papar "Offline" (pudar) | MAC pusat salah, atau sasaran mati | Semak `RECEIVER_MAC` dalam target_demo. Semak bateri. Reset ESP32 sasaran. |
| Tiada tembakan direkod | Piezo longgar, atau `THRESHOLD` terlalu tinggi | Pastikan piezo rapat plat. Turunkan `THRESHOLD` ke 800-1000, re-flash. |
| Terlalu banyak false hit | `DEBOUNCE_MS` terlalu rendah | Naikkan ke 250-300, re-flash. |
| Ketuk di S1 daftar sebagai S2 | Piezo terlalu dekat, getaran meresap | Tambah getah/busa antara piezo. Atau gunakan plat yang lebih tebal. |
| Dashboard beku | Pelayar cache | Refresh `Ctrl+Shift+R`. Klik **↻ Reset** untuk kosongkan data ESP32. |
| Badge **SENSOR TERAKHIR** kekal "—" | Tembakan belum didaftar | Ketuk lebih kuat. Jika masih tiada, lihat "Tiada tembakan direkod" di atas. |
| Feed "Tembakan Terkini" tak muncul | Pelayar lama (tak support CSS grid) | Guna Chrome/Safari/Firefox versi 2019+. |
| Upload Arduino gagal "A fatal error occurred..." | Port salah, atau perlu tekan butang BOOT | Tekan dan tahan butang **BOOT** pada ESP32 semasa "Connecting...". |

### Plan B — Hardware rosak ditengah-tengah bentang

Jika hardware betul-betul rosak, awak boleh **preview dashboard dari laptop**:

```bash
cd demo
node simulator.js
# buka http://localhost:8081 pada laptop
```

Simulator menjana tembakan rawak setiap 900 ms — semua paparan sama
seperti dashboard sebenar. Tunjuk laptop kepada juri + terangkan hardware
terpaksa diganti (bukan ideal, tapi lebih baik dari kosong).

---

## ❓ Soalan juri & jawapan

### "Bagaimana sistem tahu peluru kena?"
> Kami guna **piezo disc** (penderia getaran piezoelektrik). Bila peluru
> (atau pukulan) hentam plat sasaran, gelombang mekanikal merambat
> melalui papan. Piezo menukarnya jadi voltan > 1500 ADC (dari 12-bit
> ADC 3.3V). ESP32 membaca 4 piezo setiap gelung `loop()` dan pilih
> yang **paling tinggi amplitudnya** sebagai sensor yang trigger.

### "Kenapa 4 piezo setiap sasaran?"
> Setiap piezo mewakili satu **kuadran / zon**. Piezo paling dekat
> dengan titik impak akan hasilkan voltan paling tinggi → itulah zon
> yang kena. Zon 10 (S1) = paling tinggi skor, Zon 4 (S4) = paling
> rendah — simulasi sasaran menembak ISSF.

### "Macam mana sasaran hantar data ke pusat?"
> **ESP-NOW** — protokol peer-to-peer terbina dalam ESP32, guna WiFi
> MAC layer tanpa TCP/IP. Latency <5 ms, jarak sehingga 100 m (line of
> sight), tanpa router. Paket adalah struct C padat 9 bait: `{type,
> targetID, sensorID, zone, score, total, amp}`.

### "Kenapa pakai tablet bukan laptop?"
> ESP32 pusat hos Wi-Fi AP **`Target_System`** dan web server sendiri
> (WebServer library built-in). Dashboard HTML/CSS/JS tersemat dalam
> sketsa C++ sebagai PROGMEM — tiada pelayan luaran, tiada internet
> diperlukan. Tablet/telefon apa pun yang ada pelayar moden boleh guna.

### "Boleh tambah lebih sasaran?"
> Ya. Dalam repo ini ada **sistem v2** (`firmware/` + `dashboard/`)
> yang sokong **15 sasaran**, sesi PAR timer, leaderboard, OTA firmware
> update, dan paparan TV mode fullscreen. Demo ini adalah versi ringkas
> — 3 sasaran sahaja, tiada library luar.

### "Apa ketepatan sistem?"
> - **Deteksi hit**: ambang piezo 1500 ADC (boleh dilaraskan).
> - **Masa hit → paparan tablet**: ~25-50 ms (ESP-NOW + polling 300 ms).
> - **Jarak antara sasaran & pusat**: sehingga 100 m (LR protocol).
> - **Debounce**: 150 ms per piezo (elak "ringing" dari satu hit).

### "Berapa kos prototaip ini?"
> | Barang | Kuantiti | Kos anggaran (RM) |
> |--------|----------|-------------------|
> | ESP32 DevKit | 4 | 80 |
> | Piezo disc 27 mm | 12 | 24 |
> | Perintang 1 MΩ | 12 | 2 |
> | Power bank | 4 | 80 |
> | Kabel + wayar | — | 15 |
> | Plywood + gam | — | 20 |
> | **Jumlah** | | **~RM 220** |

### "Bolehkah dikembangkan secara komersial?"
> Ya. Aplikasi sasaran:
> - **Latihan tentera / polis** (ketepatan, penjejakan sesi).
> - **Sukan menembak** (ISSF, PAR timer).
> - **Hiburan** (arkad, karnival).
> Dengan v2 ada **OTA** — firmware boleh update jauh tanpa buka kes.

---

**Semoga berjaya dengan bentang esok!** 🎯🔥

Rujukan:
- Demo README: [`demo/README.md`](README.md)
- Presentation cheat sheet: [`demo/PRESENTATION.md`](PRESENTATION.md)
- Repo utama: <https://github.com/albakri3008092/kik-shooting-target>
