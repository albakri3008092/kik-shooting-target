# Prototype V2 — Double-tap + Triangulation

Penambahbaikan dari demo v1 untuk **kegunaan ujinilai sebenar** (latihan menembak,
penilaian double-tap, posisi tembakan).

## Apa yang berubah dari demo v1

| Kategori | Demo v1 | Prototype V2 |
|---|---|---|
| Pengesanan tembakan | 1 sensor pilih (peak tertinggi) | 4 amplitude dikumpul dalam **window 30ms** |
| Debounce | 250 ms | **60 ms** (catch double-tap split 0.10s) |
| Posisi | Suku (S1/S2/S3/S4) | **(X, Y) anggaran** dari weighted-centroid |
| Skor | S1=10, S2=8, S3=6, S4=4 (zon piezo) | **5-ring** dari jarak ke pusat (5/4/3/2/1 mata) |
| Split-time | — | **Real-time** + min + average |
| Visualisasi | Kotak warna per piezo | **Canvas 2D** dengan dot per tembakan + ring scoring |

## Hardware (cadangan)

Prototype V2 menyokong **15 sasaran** + 1 pusat (16 ESP32). Setiap sasaran
perlu **perintang 1 MΩ pull-down** pada setiap piezo signal pin (jumlah 60
untuk 15 sasaran, 4 piezo per sasaran). Tanpa perintang, GPIO 34/35 tidak
stabil dan triangulation akan bias ke S1/S2.

Nota ESP-NOW: pusat **tidak** perlu daftar peer untuk receive — semua 15
sasaran terus boleh hantar selagi `RECEIVER_MAC` mereka padan dengan MAC
softAP pusat. Had ESP-NOW peer (~20) hanya berkenaan apabila *menghantar*.

```
Piezo S1 (+) ── GPIO 32 ── 1 MΩ ── GND
Piezo S2 (+) ── GPIO 33 ── 1 MΩ ── GND
Piezo S3 (+) ── GPIO 34 ── 1 MΩ ── GND
Piezo S4 (+) ── GPIO 35 ── 1 MΩ ── GND
```

Susunan piezo di belakang papan sasaran (saiz 30cm x 30cm cadangan):

```
   S1 (kiri-atas)        S2 (kanan-atas)
        ●                       ●

                   ⊕ tengah

        ●                       ●
   S3 (kiri-bawah)        S4 (kanan-bawah)
```

## Pin / GPIO

| Sasaran (target_v2) | Pusat (central_v2) |
|---|---|
| GPIO 32 = S1 piezo | GPIO 25 = buzzer (opsyen) |
| GPIO 33 = S2 piezo | LED 2 = built-in status |
| GPIO 34 = S3 piezo | |
| GPIO 35 = S4 piezo | |
| LED 2 = ESP-NOW send status | |

## Flash

### 1. Pusat (sekali sahaja)

1. Buka `prototype_v2/central_v2/central_v2.ino` dalam Arduino IDE.
2. Upload ke ESP32 pusat.
3. Buka Serial Monitor (115200), tekan EN. Catat baris `MAC: ...`.
4. SSID akan keluar: `Target_System_V2`, password `12345678`.

### 2. Sasaran (ulang sehingga 15 kali)

1. Buka `prototype_v2/target_v2/target_v2.ino`.
2. Edit 2 baris atas:
   ```cpp
   #define TARGET_ID    1                 // 1..15, unik per board
   static uint8_t RECEIVER_MAC[6] = { ... };  // MAC pusat dari step 1
   ```
3. Upload ke sasaran #1.
4. Ulang untuk `TARGET_ID = 2, 3, ... 15`. Pastikan tiada dua board guna ID
   yang sama (sasaran ID-bertindih akan kelihatan "berkelip" pada dashboard).

Tip: untuk flash banyak board, simpan satu salinan sketsa per ID supaya
awak hanya perlu pilih port lalu Upload, tanpa edit semula `TARGET_ID`.

### 3. Tablet

WiFi → `Target_System_V2` / `12345678` → buka `http://192.168.4.1/`.

## Protokol ESP-NOW

3 jenis paket dari sasaran ke pusat:

### HitPacketV2 (type=1) — 16 bytes
```cpp
struct HitPacketV2 {
  uint8_t  type;            // 1
  uint8_t  targetID;
  uint16_t hitSeq;          // counter monotonic
  uint32_t timestampMs;     // millis() pada hit
  uint16_t peak[4];         // amplitude max S1..S4 dalam window 30ms
  uint8_t  triggerSensor;   // 1..4 yang trigger pertama
  uint8_t  windowMs;        // 30 (untuk diagnostic)
};
```

### HeartbeatPacket (type=2) — 4 bytes
Setiap 5 saat untuk sahkan sasaran online.

### HealthPacket (type=3) — 18 bytes
Setiap 2 saat — baseline + peak per sensor untuk auto-detect sensor rosak.

## Algoritma

### Hit-window state machine (target)

```
IDLE ───[rising edge on any sensor]───→ CAPTURING (30ms)
                                           │
                                           ▼ (window expires)
                                       send HitPacketV2
                                           │
                                           ▼
                                       DEBOUNCING (60ms)
                                           │
                                           ▼ (debounce expires)
                                         IDLE
```

- IDLE: scan semua 4 sensor, cari rising edge (sample - prev > 600 AND sample > baseline + 800)
- CAPTURING: scan semua 4 sensor pada kelajuan penuh, simpan peak[i] = max(peak[i], v)
- DEBOUNCING: ignore trigger, biar piezo ringing tenang
- Min interval antara hit: 30 + 60 = 90 ms → boleh detect split 0.10s+

### Triangulation (central)

Weighted centroid dalam normalized [-1, +1] x [-1, +1]:

```
total = peak[0] + peak[1] + peak[2] + peak[3]
X = sum(peak[i] * Sx[i]) / total * 1.6
Y = sum(peak[i] * Sy[i]) / total * 1.6
```

Posisi sensor `Sx, Sy`:
- S1 = (-1, +1)
- S2 = (+1, +1)
- S3 = (-1, -1)
- S4 = (+1, -1)

Faktor `1.6` adalah empirical stretch supaya hit pada sudut sebenar (yang bagi
weight ~0.6 ke sensor terdekat) menghasilkan koordinat ~1.0 pada sudut visualisasi.

### Zone scoring (central) — 5 ring

```
r = sqrt(X^2 + Y^2)
zone = first ring where r <= RING_R[i]
score = RING_SCORE[zone]
```

Ring radii (normalized 0..1):

| Zone | Radius | Mata |
|---|---|---|
| Tengah | 0.20 | **5** |
| Ring 2 | 0.40 | **4** |
| Ring 3 | 0.60 | **3** |
| Ring 4 | 0.80 | **2** |
| Ring 5 (luar) | 1.30 | **1** |
| Miss | > 1.30 | 0 |

### Split-time

Bila hit baharu diterima:
```
splitMs = now - lastHitMs   (atau 0 untuk hit pertama)
splitSum += splitMs
splitCount++
minSplit = min(minSplit, splitMs)
average = splitSum / splitCount
```

Dipaparkan di dashboard sebagai "Last Split", "Avg Split", "Best Split".

### Calibration (Stage 5)

Setiap sasaran simpan **4 nilai rujukan peak** (satu per sensor) dalam NVS
Preferences. Triangulation membahagikan tiap-tiap raw peak dengan nilai
rujukan sensor itu sebelum kira centroid:

```
peak_norm[i] = peak[i] / refPeak[i]
X = sum(peak_norm[i] * Sx[i]) / sum(peak_norm) * 1.6
Y = sum(peak_norm[i] * Sy[i]) / sum(peak_norm) * 1.6
```

Hasil: piezo dengan sensitiviti berbeza (atau dipasang dengan jarak
berlainan ke pusat papan) akan beri sumbangan setara ke posisi.

**UX kalibrasi (4 langkah, ~30 saat):**
1. Tablet → tekan butang "Kalibrasi" pada kad sasaran.
2. Banner kuning timbul: *"Ketuk S1 sekarang"*. Ketuk piezo S1 satu kali.
3. Banner update: *"Ketuk S2 sekarang"*. Ulang untuk S3, S4.
4. Selesai → banner hilang, badge `CAL: ON` hijau muncul. Nilai disimpan
   dalam NVS — kekal selepas reboot.

**Endpoint HTTP:**
- `POST /cal/start?target=N` — masuk mod kalibrasi sasaran N
- `POST /cal/cancel` — keluar mod kalibrasi tanpa simpan
- `POST /cal/clear?target=N` — padam kalibrasi sasaran N
- `GET  /status` → JSON `cal: {active, target, step, capture[]}` +
  per-target `calValid` dan `calRef[]`

Hits semasa mod kalibrasi **tidak dikira** ke skor — ia training samples
sahaja. Buzzer tetap chirp sebagai feedback ketukan.

### Session log (Stage 6)

Setiap hit yang diterima diqueue (FreeRTOS xQueue) dan ditulis ke
`/session.csv` dalam SPIFFS. Format:

```
ts_ms,target,trigger,zone,score,x,y,split_ms,peak1,peak2,peak3,peak4
12345678,1,1,1,10,-0.123,0.456,0,3120,1820,560,440
12345789,1,2,2,9,0.234,0.667,111,1240,3380,520,510
...
```

Tablet boleh download CSV terus dari pautan dashboard:

- `GET  /log.csv` — stream fail penuh (Content-Disposition: attachment)
- `POST /log/clear` — truncate + tulis semula header sahaja

Queue depth = 64 entri; pada loop, central drain sehingga 4 baris setiap
iterasi supaya HTTP server tetak responsif. SPIFFS append ~5 ms per
baris, jadi 4 hit sekaligus = ~20 ms — masih dalam bajet 200 ms poll.

## Limitasi semasa

| Isu | Workaround |
|---|---|
| Triangulation weighted-centroid bias ke sudut bila hit jauh dari pusat (sensor terdekat dominate) | Calibration Stage 5 normalize per-sensor; untuk fit yang lebih baik perlu polynomial 2D dengan 9-point grid |
| ISSF zone radii fixed ke unit normalized (bukan cm sebenar) | Tambah field `target_diameter_cm` dalam Preferences → convert (X,Y) ke cm |
| SPIFFS bersize ~1.5 MB → boleh simpan ~30k baris CSV. Tiada auto-rotate | Padam log selepas eksport ke laptop / SD-card |
| 60ms debounce → tak boleh handle split < 0.10s (world-class IPSC) | Phase 3: TDoA dengan i2s_adc DMA |

## Roadmap

- ✅ Stage 1: Lower debounce + ringing filter
- ✅ Stage 2: Split-time tracking + dashboard
- ✅ Stage 3: Amplitude triangulation core
- ✅ Stage 4: Target visualization (canvas dots)
- ✅ Stage 5: 4-point calibration mode + persistent storage (NVS)
- ✅ Stage 6: SPIFFS session log + CSV export
