# 🎯 KIK Target — Panduan Bentang (1 muka surat)

Sistem prototaip **Shooting Target Detecting Bullet** — 1 ESP32 pusat +
3 ESP32 sasaran (4 piezo setiap satu) + tablet dashboard.

---

## Keperluan prototaip (check-list malam sebelum bentang)

- [ ] **4 papan ESP32 DevKit** sudah di-flash (1 pusat + 3 sasaran)
- [ ] **12 piezo** (4 setiap sasaran) dilekat belakang plat sasaran,
      tersambung ke GPIO 32/33/34/35 dengan pull-down 1 MΩ
- [ ] **Bekalan kuasa** untuk 4 board (power bank / USB)
- [ ] **Tablet** sudah set Wi-Fi **Target_System / 12345678** dan
      bookmark **<http://192.168.4.1/>**
- [ ] **Buzzer** pada pusat (pilihan, GPIO 25) — bunyi bila kena
- [ ] Printout README (pendawaian + MAC)

## Skrip bentang — 3 minit

1. **Hidupkan pusat** (LED kelip → Wi-Fi AP aktif).
2. **Hidupkan 3 sasaran** — tablet papar "Sasaran 1/2/3 · 3/3 aktif".
3. Tunjuk tablet ke juri — **3 kad sasaran** + bullseye kosong.
4. **Tembak / ketuk Sasaran 1 di kuadran S1 (atas kiri)**:
   - Bullseye: titik merah muncul di zon 10
   - Badge **SENSOR TERAKHIR: S1** (merah) + "baru"
   - Kotak **S1 · Zon 10** nombor naik, berkelip
   - Feed bawah: `T1  S1  Zon 10  +10  baru`
   - Buzzer bunyi (jika dipasang)
5. Ulang dengan Sasaran 2 (S2 atas kanan) dan Sasaran 3 (S3/S4 bawah).
6. Tekan **↻ Reset** — semua kiraan zero, feed kosong.

## Apa yang kita papar

| Kawasan dashboard | Data yang ditunjuk |
|-------------------|-------------------|
| Atas (3 kad besar) | Jumlah Tembakan, Jumlah Skor, Sasaran Aktif |
| Kad sasaran | Nombor sasaran, jumlah skor, bullseye dengan titik |
| **Badge Sensor Terakhir** | **Piezo mana (S1–S4) mengesan tembakan itu** |
| 4 kotak S1..S4 | Jumlah tembakan tiap-tiap piezo (+ zon) |
| Feed "Tembakan Terkini" | 16 tembakan terakhir: **T#  S#  Zon  skor  masa** |

## Jawapan soalan lazim juri

| Soalan | Jawapan ringkas |
|--------|-----------------|
| "Macam mana tahu peluru kena?" | Piezo (penderia getaran) pada plat sasaran — bila peluru hentam, piezo hasilkan voltan >1500 ADC, ESP32 kesan & hantar. |
| "Kenapa 4 piezo?" | Tiap piezo bacaan satu kuadran / zon (10, 8, 6, 4). Yang paling kuat respon = zon kena. |
| "Bagaimana sasaran hantar ke pusat?" | **ESP-NOW** (peer-to-peer Wi-Fi) — tak perlu router, latency <5 ms. |
| "Kenapa tablet nampak bukan laptop?" | ESP32 pusat buat Wi-Fi AP & host web sendiri. Sambung terus, tiada Internet diperlukan. |
| "Boleh tambah sasaran lagi?" | Ya — sistem `firmware/` v2 sokong 15 sasaran + sesi + leaderboard + OTA. Demo ini versi ringkas. |
| "Ketepatan?" | ±25 ms dari hentakan ke paparan. Ambang `THRESHOLD` & `DEBOUNCE_MS` boleh dilaras dalam `target_demo.ino`. |

## Fallback — kalau sesuatu tak jalan

| Masalah di atas pentas | Langkah pantas |
|------------------------|----------------|
| Tablet tak nampak Wi-Fi `Target_System` | Kitar kuasa pusat. Tunggu 10 s. |
| Sasaran "Offline" | Semak bateri, tekan butang reset ESP32. MAC dalam `target_demo.ino` mesti padan MAC pusat. |
| Tiada tembakan didaftar | Turunkan `THRESHOLD` (mis. 1000) dan re-flash. Atau ketuk lebih kuat. |
| Terlalu banyak false hit | Naikkan `DEBOUNCE_MS` ke 250-300. |
| Dashboard beku | Refresh pelayar. `/reset` untuk kosongkan data. |
| **Hardware rosak terus** | Buka preview simulator di laptop: `node demo/simulator.js` → <http://localhost:8081/>. |

## URL penting

- Dashboard tablet: **<http://192.168.4.1/>**
- Preview simulator (laptop, tanpa hardware):
  ```
  node demo/simulator.js
  # http://localhost:8081/
  ```
- Status mentah (untuk troubleshoot): `http://192.168.4.1/status`

---

_Selamat bentang! Kalau ada isu tengah malam, buka
[`demo/README.md`](README.md) untuk wiring + flashing penuh._
