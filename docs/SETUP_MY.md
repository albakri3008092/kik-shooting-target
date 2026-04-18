# Panduan Pemasangan — Sistem Sasaran Menembak KIK v2

> Panduan langkah demi langkah dalam **Bahasa Malaysia**. Untuk versi
> Inggeris, lihat [`SETUP_EN.md`](SETUP_EN.md).

---

## 1. Ringkasan Sistem

Sistem KIK terdiri daripada:

| Bil. | Modul              | Bilangan | Peranan                                      |
|------|--------------------|----------|----------------------------------------------|
| 1    | Sasaran (target)   | **15**   | ESP32 + 4 piezo, hantar data tembakan        |
| 2    | Pusat (central)    | **1**    | ESP32, terima data, serve dashboard, buzzer  |
| 3    | Tablet / telefon   | 1+       | Paparkan dashboard di `http://kik.local/`    |

Komunikasi antara sasaran & pusat: **ESP-NOW Long Range** (tiada WiFi router).
Pusat menjadi **Access Point** sendiri (`SSID=Target_System`, `PSK=12345678`).

---

## 2. Senarai Barang (BOM)

**Setiap sasaran (×15):**
- 1 × ESP32 DevKit (30-pin) dengan antena
- 4 × Piezo disc 27 mm (siap tampal pada plat sasaran)
- 4 × Perintang 1 MΩ (pull-down)
- 1 × Perintang 100 kΩ × 2 (pembahagi bateri — pilihan)
- 1 × Power bank 5V / 10 000 mAh
- Wayar JST / Dupont, silikon glue, double-sided tape tebal

**Pusat (×1):**
- 1 × ESP32 DevKit
- 1 × Buzzer pasif 5V (atau speaker kecil)
- 1 × Casing + butang power
- 1 × Power bank atau 5V supply

**Tablet:**
- Android / iOS mana-mana pelayar moden (Chrome / Safari terkini)

---

## 3. Pemasangan Perisian (Sekali Sahaja)

### 3.1 Pasang Arduino IDE

1. Muat turun Arduino IDE 2.x dari <https://www.arduino.cc/en/software>
2. Buka `File → Preferences → Additional board manager URLs`, tambah:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools → Board → Boards Manager` → cari **esp32** → klik **Install**
   (pilih versi 2.0.17 atau lebih baru).

### 3.2 Pasang Library

`Sketch → Include Library → Manage Libraries`, kemudian cari & pasang:

- **ArduinoJson** (oleh Benoit Blanchon) — ≥ 7.0
- **AsyncTCP** (oleh me-no-dev)
- **ESPAsyncWebServer** (oleh me-no-dev)

LittleFS sudah termasuk dalam ESP32 core — **tidak perlu** install berasingan.

### 3.3 Pasang "Data Upload" Tool (untuk muat naik dashboard ke LittleFS)

1. Muat turun plugin: <https://github.com/earlephilhower/arduino-esp32fs-plugin/releases>
2. Extract, letak folder `tools/` dalam folder sketchbook Arduino
   (biasanya `~/Arduino/tools/`). Fail: `ESP32FS.jar`.
3. Restart Arduino IDE. Anda akan nampak menu baru:
   `Tools → ESP32 Sketch Data Upload`.

---

## 4. Klon Repo

```bash
git clone https://github.com/albakri3008092/kik-shooting-target.git
cd kik-shooting-target
```

Susunan folder penting:
- `firmware/target_node/` — kod sasaran
- `firmware/central_receiver/` — kod pusat
- `firmware/get_mac/` — utiliti baca MAC
- `dashboard/src/` — fail web (akan disalin ke LittleFS)
- `scripts/build_data.sh` — skrip salin dashboard ke `data/`

---

## 5. Langkah 1: Dapatkan MAC Setiap Papan

Setiap ESP32 ada **MAC Address** unik. Pusat perlu tahu MAC semua 15 sasaran.

1. Buka Arduino IDE.
2. Buka `firmware/get_mac/get_mac.ino`.
3. Pilih `Tools → Board → ESP32 Dev Module`, port COM yang betul.
4. Upload (tekan anak panah →).
5. Buka `Tools → Serial Monitor` pada **115200 baud**.
6. MAC akan dipaparkan, contoh:
   ```
   MAC Address: 24:6F:28:A1:B2:C3
   Label this board (Target1..15 or Central).
   ```
7. **Tulis label** pada papan itu dengan masking tape:
   `T01`, `T02`, ... `T15`, `CENTRAL`.
8. Catat semua MAC dalam jadual (anda akan perlu nanti):

| Label    | MAC Address         |
|----------|---------------------|
| Central  | 24:6F:28:XX:XX:XX   |
| Target 1 | 24:6F:28:AA:BB:01   |
| Target 2 | 24:6F:28:AA:BB:02   |
| ...      | ...                 |
| Target 15| 24:6F:28:AA:BB:15   |

---

## 6. Langkah 2: Flash Pusat (Central Receiver)

1. Buka `firmware/central_receiver/central_receiver.ino`.
2. Cari jadual `targetMACs[]` (di bahagian atas fail).
3. Masukkan MAC 15 sasaran mengikut urutan:
   ```cpp
   static uint8_t targetMACs[NUM_TARGETS][6] = {
     {0x24,0x6F,0x28,0xAA,0xBB,0x01}, // Target 1
     {0x24,0x6F,0x28,0xAA,0xBB,0x02}, // Target 2
     // ... sampai Target 15
   };
   ```
4. Sambung ESP32 **pusat** ke PC, pilih port COM yang betul.
5. Klik **Upload**.
6. Tunggu sampai `Done uploading`.

### Muat Naik Dashboard ke LittleFS

1. Jalankan skrip salinan:
   ```bash
   ./scripts/build_data.sh
   ```
   Ia akan salin `dashboard/src/` → `firmware/central_receiver/data/`.
2. Dalam Arduino IDE, pastikan `central_receiver.ino` yang dibuka.
3. `Tools → Partition Scheme → Default 4MB with spiffs` (atau
   `Huge APP (3MB No OTA/1MB SPIFFS)` jika sketch terlalu besar).
4. `Tools → ESP32 Sketch Data Upload`.
5. Tunggu selesai (~1 min).

---

## 7. Langkah 3: Flash 15 Sasaran

Untuk **setiap** papan sasaran (1 hingga 15):

1. Buka `firmware/target_node/target_node.ino`.
2. Ubah **dua** baris di atas sketch:
   ```cpp
   #define TARGET_ID    1                       // 1..15 — ubah ikut papan
   static uint8_t RECEIVER_MAC[6] = { 0x24,0x6F,0x28,0xXX,0xXX,0xXX };
   // ^^^ MAC CENTRAL yang anda catatkan
   ```
3. Sambung papan sasaran 1, pilih port, **Upload**.
4. **Tukar** `TARGET_ID` kepada `2`, sambung papan sasaran 2, **Upload**.
5. Ulang sampai Target 15.

> **Tips**: Tulis nombor besar pada casing setiap sasaran. Kalau tertukar,
> dashboard akan menunjukkan hit pada sasaran yang salah.

---

## 8. Langkah 4: Wayar Piezo

Ikut gambar rajah dalam [`docs/wiring.md`](wiring.md). Ringkas:

```
  Piezo Sn (+) ─── GPIO (32/33/34/35) ──── 1 MΩ ──── GND
  Piezo Sn (–) ─── GND
```

Susunan piezo pada belakang plat sasaran:

```
        S1 (10)       S2 (8)
              🎯
        S3 (6)        S4 (4)
```

> Tampal piezo guna **double-sided tape tebal** supaya getaran sampai.
> Jangan tampal di atas skru atau lubang — nanti sensitiviti tak konsisten.

---

## 9. Langkah 5: Uji Sistem

1. Hidupkan **pusat** dahulu (ia akan buat LED ESP32 nyala dan buzzer bunyi sekejap).
2. Hidupkan semua **15 sasaran**. Dalam 5 saat, masing-masing akan mula hantar
   heartbeat ke pusat.
3. Dengan tablet, sambung WiFi ke SSID `Target_System`, password `12345678`.
4. Buka pelayar, pergi ke **<http://kik.local/>** (atau `http://192.168.4.1/`).
5. Dashboard akan dipaparkan. Tab **Kesihatan** patut tunjuk ke-15 sasaran
   **Online** dengan bateri & RSSI.
6. Ketuk setiap sasaran (ringan sahaja) untuk pastikan semua 4 piezo berfungsi.

---

## 10. Penggunaan Dashboard

### 10.1 Tab Langsung
- Grid 15 kad sasaran. Setiap kad menunjukkan skor, bateri, sensor status.
- Titik akan muncul pada bullseye setiap kali tembakan didaftarkan
  (posisi dihitung daripada 4 piezo).

### 10.2 Tab Skor
- Podium **🥇🥈🥉** + senarai sasaran disusun mengikut jumlah mata.
- Kolum `%` = ketepatan (skor / skor maksimum mungkin).

### 10.3 Tab Kesihatan
- Bar bateri, RSSI, versi firmware.
- Status setiap sensor: hijau = OK, kuning = bacaan pelik, merah = gagal.

### 10.4 Tab Sesi
- Isi nama sesi + penembak, tetapkan masa PAR.
- Tanda **Mula Rawak (buzzer)** untuk delay rawak 1.5-4 saat sebelum buzzer.
- Klik **Mula Sesi**. Kaunter akan reset dan timer mula.
- Log semua tembakan dalam jadual.
- Graf skor merentasi masa + taburan zon.
- **Eksport CSV / JSON** untuk simpan di luar peranti.

### 10.5 Tab Sejarah
- Semua sesi yang sudah disimpan pada LittleFS — muat turun sebagai JSON.

### 10.6 Tab Tetapan
- Ubah ambang (threshold), debounce, markah zon.
- **Hantar ke Sasaran** → semua 15 sasaran terima kemaskini serta-merta
  (tanpa perlu flash semula).
- Tambah senarai penembak.
- **Kenalpasti** — bunyikan buzzer pada sasaran tertentu (untuk
  pengesahan labeling).

### 10.7 Mod TV
- Buka `/tv.html` pada TV besar. Skrin penuh leaderboard.

---

## 11. Kemas Kini Firmware Jarak Jauh (OTA)

Selepas sekali flash melalui USB, anda boleh update **wayarles** guna OTA:

### 11.1 Dari Arduino IDE

1. Pastikan komputer anda sambung ke WiFi `Target_System`.
2. `Tools → Port`. Tunggu senarai muncul — akan ada:
   - `kik-central at 192.168.4.1 (ESP32)`
   - `kik-target-01 at 192.168.4.xx (ESP32)` ... hingga 15.
3. Pilih port network yang anda nak flash.
4. Apabila diminta password, masukkan: **`kik-ota`**.
5. Upload seperti biasa.

### 11.2 Dari Command Line (python script)

```bash
pip install esptool
python -m espota -i 192.168.4.1 -p 3232 -r -f \
  -a kik-ota \
  ~/Arduino/build/esp32.esp32.esp32/central_receiver.ino.bin
```

---

## 12. Troubleshooting

| Masalah                                 | Penyelesaian                                    |
|----------------------------------------|-------------------------------------------------|
| `kik.local` tidak dibuka               | Guna IP: `http://192.168.4.1/`                  |
| Sasaran offline selepas 15 saat        | Reset kuasa, semak MAC, semak antena            |
| Tembakan tidak didaftarkan             | Naikkan ambang dalam `Tetapan`; semak wayar piezo |
| Dashboard kosong                       | Jalankan `build_data.sh` dan upload data semula |
| Tembakan terlalu banyak (bising)       | Naikkan `Debounce` ke 200-300 ms                |
| Bateri cepat habis                     | Semak pembahagi voltan; deep-sleep mode default 30 min idle |

---

## 13. Rujukan

- Fail perkakasan / 3D casing: `hardware/` (WIP)
- Dokumentasi pin lengkap: [`docs/wiring.md`](wiring.md)
- Kod sumber: [GitHub repository](https://github.com/albakri3008092/kik-shooting-target)

Selamat menembak! 🎯
