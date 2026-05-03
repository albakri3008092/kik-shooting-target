# Panduan Setup Lengkap — KIK Shooting Target V2 (15 Sasaran)

Panduan langkah-demi-langkah untuk pasang prototype V2 dari kosong hingga
boleh tembak. Anggaran masa: **3-4 jam** (termasuk solder).

---

## Senarai Bahan

| Komponen | Bilangan | Catatan |
|---|---|---|
| ESP32 DevKit board | **16** | 1 pusat + 15 sasaran |
| Piezo disc (27 mm) | **60** | 4 per sasaran |
| Perintang 1 MΩ | **60** | 4 per sasaran (toleransi ±5% OK) |
| Wayar jumper / wayar tembaga | secukupnya | Merah + hitam |
| Kabel USB (data) | 2-3 | untuk flash board |
| Power bank / USB charger | 16 | untuk power semua board |
| Buzzer 5V (opsyen) | 1 | untuk pusat |
| Tablet / telefon | 1 | dashboard |
| Papan sasaran (plywood ~30×30 cm) | 15 | untuk lekat piezo |
| Glue gun / epoxy | 1 set | untuk lekat piezo |

**Anggaran kos**: ~RM 600-800 untuk semua hardware.

---

## Bahagian 1: Solder Sensor Sasaran (paling lama)

### 1.1 Sediakan satu sasaran (ulang 15 kali)

Untuk **setiap sasaran**, anda perlu:
- 1 ESP32
- 4 piezo
- 4 perintang 1 MΩ
- ~2 meter wayar (merah + hitam)

### 1.2 Solder satu piezo (ulang 4 kali per sasaran = 60 kali total)

Rujuk gambar `piezo_resistor_wiring.png` yang saya hantar tadi.

1. Solder **wayar merah** pada terminal **(+) piezo** (cakera seramik dalam).
2. Solder **wayar hitam** pada terminal **(−) piezo** (cincin loyang luar).
3. Wayar merah → pin **GPIO 32** (untuk S1) pada ESP32.
4. Wayar hitam → pin **GND** pada ESP32.
5. **Solder perintang 1 MΩ merentang antara wayar merah dan wayar hitam**
   (bukan dalam siri — pull-down).
6. Ulang untuk piezo S2 (GPIO 33), S3 (GPIO 34), S4 (GPIO 35).

### 1.3 Lekatkan piezo pada papan sasaran

Susunan **WAJIB ikut** supaya triangulation betul:

```
   S1 (kiri-atas)         S2 (kanan-atas)
        ●                       ●

                ⊕ tengah

        ●                       ●
   S3 (kiri-bawah)         S4 (kanan-bawah)
```

- Letak piezo di **belakang papan** (tak nampak dari depan).
- Guna glue gun atau epoxy 5-min — pastikan piezo melekat **rapat** ke papan
  supaya getaran rambat dengan baik.
- Tinggalkan ESP32 di belakang papan juga (pasang dalam casing kecil atau
  velcro pada papan).

### 1.4 Nombor sasaran

Tulis **nombor sasaran (1-15)** dengan marker hitam besar pada **belakang**
setiap papan supaya tak tertukar masa flash.

---

## Bahagian 2: Setup Pusat (sekali sahaja)

### 2.1 Install Arduino IDE + ESP32 board package

1. Download Arduino IDE 2.x dari [arduino.cc](https://arduino.cc).
2. `File` → `Preferences` → tambah dalam **"Additional Board Manager URLs"**:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools` → `Board` → `Boards Manager` → cari **"esp32"** → install
   versi **2.0.17** (paling stabil) atau 3.x.

### 2.2 Clone / download repo

```bash
git clone https://github.com/albakri3008092/kik-shooting-target.git
```

Atau download ZIP dari GitHub.

Cabang aktif: `devin/1777645033-prototype-v2`

### 2.3 Flash pusat

1. Sambung ESP32 **PUSAT** ke laptop dengan kabel USB.
2. Buka `prototype_v2/central_v2/central_v2.ino` dalam Arduino IDE.
3. `Tools` → `Board` → **ESP32 Dev Module**.
4. `Tools` → `Port` → pilih port yang muncul (COM3, /dev/ttyUSB0, dll).
5. Klik **Upload** (anak panah →).
6. Tunggu sampai "Done uploading" muncul.
7. Buka **Serial Monitor** (baud **115200**), tekan butang **EN** pada ESP32.
8. **CATAT BARIS INI:**
   ```
   MAC:  XX:XX:XX:XX:XX:XX   <-- paste this into target_v2 RECEIVER_MAC
   ```

   ⚠️ **PENTING**: MAC ini perlu dimasukkan dalam kod sasaran.

### 2.4 Sahkan WiFi pusat

1. Tablet → WiFi settings.
2. Cari `Target_System_V2` → password `12345678`.
3. Buka browser → `http://192.168.4.1/`.
4. Patut nampak dashboard "KIK Target — Prototype V2" dengan
   **Sasaran Aktif: 0/15**.

---

## Bahagian 3: Flash Sasaran (ulang 15 kali)

### 3.1 Edit kod sasaran (hanya 2 baris)

Buka `prototype_v2/target_v2/target_v2.ino`. Lihat 2 baris atas:

```cpp
#define TARGET_ID    1                                      // 1..15
static uint8_t RECEIVER_MAC[6] = { 0xB0, 0xCB, 0xD8, 0xCF, 0xF1, 0x61 };
```

1. Tukar `TARGET_ID` ikut nombor sasaran (1, 2, 3, ... 15).
2. Tukar `RECEIVER_MAC[]` kepada MAC pusat yang dicatat tadi (Bahagian 2.3).

   Contoh: kalau MAC pusat = `A1:B2:C3:D4:E5:F6`, baris jadi:
   ```cpp
   static uint8_t RECEIVER_MAC[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
   ```

### 3.2 Flash sasaran #1

1. Cabut pusat dari laptop (biar pusat jalan guna power bank).
2. Sambung ESP32 **sasaran #1** ke laptop.
3. `Tools` → `Port` → pilih port baharu.
4. **Tutup Serial Monitor** sebelum upload (kalau buka, upload gagal).
5. Klik Upload.
6. Tunggu "Done uploading".
7. Buka Serial Monitor → tekan EN → patut keluar:
   ```
   Target 1 ready. MAC=...
   Hit detection ready.
   ```
8. Cabut sasaran #1 dari laptop, pasang ke power bank.

### 3.3 Ulang untuk sasaran #2 hingga #15

Setiap kali:
1. Edit kod: tukar `#define TARGET_ID 1` → `2`, `3`, ... `15`.
2. Sambung sasaran ke laptop, upload, tunggu siap.
3. Cabut, pasang power bank.

**Tip cepat**: Buat 15 salinan folder sketsa (`target_v2_id1`, `target_v2_id2`, dll)
dengan `TARGET_ID` masing-masing sudah disetup. Lepas tu hanya pilih port
dan klik upload — tak perlu edit setiap kali.

---

## Bahagian 4: Sahkan Semua Online

1. Hidupkan **semua 16 board** (1 pusat + 15 sasaran) — guna power bank.
2. Tunggu ~10 saat (heartbeat hantar setiap 5 saat).
3. Tablet → buka `http://192.168.4.1/`.
4. **Sasaran Aktif: 15/15** patut muncul.
5. Setiap kad sasaran (1-15) patut bertukar dari **offline kelabu** ke
   **online hijau**.

### Kalau ada sasaran tak online

Periksa step demi step:

**A — Sasaran offline kelabu (badge tidak hijau)**
- Power bank kurang? Cuba tukar.
- ESP32 sasaran tak boot? Tekan EN, lihat LED dalaman berkedip.
- MAC pusat salah dimasukkan? Periksa hex `0x` setiap byte.
- Channel ESP-NOW tak padan? Pusat kunci channel 1 — sasaran auto match.

**B — Beberapa sasaran online tetapi sama ID**
- Ada 2 board guna `TARGET_ID` yang sama → kelihatan "berkelip" pada satu kad.
- Flash semula salah satu dengan ID berbeza.

**C — Semua offline**
- Pusat tak running? Tablet WiFi list, periksa SSID `Target_System_V2`
  masih ada.
- Pusat reboot? Tekan EN pada pusat.

---

## Bahagian 5: Kalibrasi (cadangan, sekali per sasaran)

Tanpa kalibrasi, triangulation guna raw amplitude — boleh jadi bias kalau
piezo sensitiviti berbeza. Kalibrasi membetulkan ini.

### 5.1 Mula kalibrasi sasaran #1

1. Pada dashboard, kad sasaran #1 → klik butang **"Kalibrasi"**.
2. Confirm dialog → klik OK.
3. Bar kalibrasi kuning muncul di atas: **"Mod Kalibrasi T1 — Ketuk S1 sekarang"**.

### 5.2 Ketuk 4 sensor satu demi satu

1. **Ketuk piezo S1** (kiri-atas) dengan jari → bar maju ke S2.
2. **Ketuk piezo S2** (kanan-atas) → bar maju ke S3.
3. **Ketuk piezo S3** (kiri-bawah) → bar maju ke S4.
4. **Ketuk piezo S4** (kanan-bawah) → kalibrasi selesai.
5. Badge **"CAL: ON"** hijau muncul pada kad sasaran.

### 5.3 Sahkan kalibrasi kekal selepas reboot

1. Cabut power pusat, sambung balik (reboot).
2. Tunggu ~10 saat.
3. Badge **"CAL: ON"** patut masih hijau (tersimpan di NVS Preferences).

### 5.4 Ulang untuk sasaran #2 hingga #15

Setiap sasaran perlu kalibrasi sendiri sebab piezo individu mungkin berbeza.

### Padam kalibrasi

Kalau salah ketuk:
- Klik **"Padam Kalibrasi"** pada kad sasaran → reset ke uncalibrated
- Atau **"Batal"** semasa proses kalibrasi → batal tanpa simpan.

---

## Bahagian 6: Test Tembakan

### 6.1 Test pertama (ringan)

1. Pastikan dashboard buka, semua sasaran online.
2. Ketuk piezo S1 sasaran #1 dengan jari kuat.
3. Patut nampak:
   - Dot merah pada canvas sasaran #1 (suku kiri-atas)
   - **Hit count: 1**
   - **Skor**: 1-5 mata (bergantung jarak dari pusat)
   - **Last Trig: S1**
   - Buzzer pusat berbunyi
   - Feed "Tembakan Terkini" tambah satu baris

### 6.2 Test double-tap

1. Ketuk S1 cepat 2 kali (split ~200ms).
2. Patut nampak **2 hits**, dengan **Last Split: ~200 ms**.
3. Kalau hanya 1 hit detect, ketuk lebih kuat / lebih cepat.

### 6.3 Test 5 zon skor

Ketuk:
- **Tengah papan** → patut score **5**
- **Antara tengah dan tepi** → patut score **4** atau **3**
- **Hujung tepi** → patut score **2** atau **1**
- **Luar papan** → patut score **0** (M, miss)

### 6.4 Test sensor health

Sistem ada 4 status sensor:

| Status | Warna | Maksud |
|---|---|---|
| **OK** | Hijau | Baseline stabil < 1000 |
| **NOISY** | Kuning | Baseline 1000-2999 (terapung / interference) |
| **ROSAK** | Merah berkelip | Baseline ≥ 3000 (pin shorted to 3.3V) |
| **MATI** | Merah berkelip | Wayar tercabut — sensor tak respond pada hits walaupun sasaran ditembak |
| **?** | Kelabu | Tiada paket > 8 saat (sasaran offline) |

#### Test NOISY (sentuhan)
1. Pegang wayar signal S2 dengan jari (sentuh metal langsung).
2. Dalam 2-3 saat, badge **S2** patut tukar **NOISY** kuning.
3. Lepas jari → kembali **OK** hijau.

#### Test MATI (wayar tercabut)
1. Cabut wayar signal piezo S3 dari header GPIO 34.
2. Tembak / ketuk sasaran 3-5 kali (ketuk papan, sensor lain akan register).
3. Selepas 3 hits dengan S3 silent → badge **S3** tukar ke **MATI** merah berkelip.
4. Sambung balik wayar → ketuk lagi → kembali **OK** selepas 1 hit yang register.

---

## Bahagian 7: Eksport Sesi (CSV)

Selepas tembak beberapa pusingan:

1. Pada dashboard footer, klik **"Muat Turun CSV"** → fail `session.csv` download.
2. Buka dengan Excel / Google Sheets.
3. Format kolum:
   ```
   ts_ms, target, trigger, zone, score, x, y, split_ms,
   peak1, peak2, peak3, peak4
   ```
4. Boleh analisis: group size, average score per sasaran, split times, dll.

### Padam log

Klik **"Padam Log"** untuk reset CSV (header sahaja). Kalibrasi tidak terjejas.

### Reset sesi (kira-kira)

Klik **"Reset Sesi"** untuk reset hit count + score per sasaran (tetapi log
CSV dan kalibrasi kekal).

---

## Bahagian 8: Troubleshooting

| Masalah | Punca | Penyelesaian |
|---|---|---|
| Sasaran kelihatan offline | Power kurang / MAC salah | Tukar power bank / periksa RECEIVER_MAC |
| Piezo tak detect | Wayar longgar / perintang tiada | Periksa solder / pasang perintang 1 MΩ |
| Hit registered S1 tetapi ketuk S3 | GPIO 34/35 floating | Periksa perintang pull-down betul-betul tersambung |
| Skor selalu 1 mata | Triangulation luar | Kalibrasi semula sasaran |
| Double-tap terlepas | Debounce kuat | Kod V2 dah set 60ms — kalau masih terlepas, ketuk lebih kuat |
| Dashboard "Terputus" | WiFi pusat down | Reboot pusat (tekan EN) |
| CSV tak download | SPIFFS tak mount | Reboot pusat — auto format |

---

## Bahagian 9: Selepas Setup

Selepas semua jalan:

1. **Backup MAC pusat** — tulis di tempat yang tak hilang.
2. **Backup salinan kod sasaran 15** dengan TARGET_ID masing-masing.
3. **Foto pendawaian** untuk rujukan masa depan.
4. **Casing** — letak setiap board dalam casing kecil (ABS / 3D print)
   untuk lindung dari geseran lapangan.

---

## Rujukan Pantas

| Item | Nilai |
|---|---|
| WiFi SSID | `Target_System_V2` |
| WiFi Password | `12345678` |
| Dashboard URL | `http://192.168.4.1/` |
| Channel ESP-NOW | 1 (kunci) |
| Heartbeat | setiap 5s |
| Health packet | setiap 2s |
| Debounce | 60 ms |
| Hit window | 30 ms |
| TRIG_DELTA | 800 ADC |
| FAST_RISE | 600 ADC |

---

**Selamat menembak! 🎯**

Sebarang isu, lihat repo: https://github.com/albakri3008092/kik-shooting-target
