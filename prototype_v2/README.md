# Prototype V2 — Double-tap + Triangulation

Penambahbaikan dari demo v1 untuk **kegunaan ujinilai sebenar** (latihan menembak,
penilaian double-tap, posisi tembakan).

## Apa yang berubah dari demo v1

| Kategori | Demo v1 | Prototype V2 |
|---|---|---|
| Pengesanan tembakan | 1 sensor pilih (peak tertinggi) | 4 amplitude dikumpul dalam **window 30ms** |
| Debounce | 250 ms | **60 ms** (catch double-tap split 0.10s) |
| Posisi | Suku (S1/S2/S3/S4) | **(X, Y) anggaran** dari weighted-centroid |
| Skor | S1=10, S2=8, S3=6, S4=4 (zon piezo) | **ISSF 10-ring** dari jarak ke pusat |
| Split-time | — | **Real-time** + min + average |
| Visualisasi | Kotak warna per piezo | **Canvas 2D** dengan dot per tembakan + ring scoring |

## Hardware (cadangan)

Berbanding demo, prototype memerlukan **perintang 1 MΩ pull-down** pada setiap
piezo signal pin (jumlah 12 untuk 3 sasaran). Tanpa perintang, GPIO 34/35
tidak stabil dan triangulation akan bias ke S1/S2.

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

### 2. Sasaran (ulang 3 kali)

1. Buka `prototype_v2/target_v2/target_v2.ino`.
2. Edit 2 baris atas:
   ```cpp
   #define TARGET_ID    1                 // 1, 2, atau 3
   static uint8_t RECEIVER_MAC[6] = { ... };  // MAC pusat dari step 1
   ```
3. Upload ke sasaran #1.
4. Ulang dengan `TARGET_ID = 2`, lalu `= 3`.

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

### ISSF zone scoring (central)

```
r = sqrt(X^2 + Y^2)
zone = first ring where r <= RING_R[i]
score = RING_SCORE[zone]
```

Ring radii (normalized):
| Zone | Radius | Skor |
|---|---|---|
| 10X (bullseye) | 0.05 | 11 |
| 10 | 0.10 | 10 |
| 9 | 0.20 | 9 |
| 8 | 0.30 | 8 |
| 7 | 0.40 | 7 |
| 6 | 0.50 | 6 |
| 5 | 0.65 | 5 |
| 4 | 0.80 | 4 |
| 3 | 0.95 | 3 |
| 2 | 1.00 | 2 |
| 1 (luar) | 1.30 | 1 |
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

## Limitasi semasa

| Isu | Workaround |
|---|---|
| Triangulation guna weighted-centroid simple → bias ke sudut bila hit jauh dari pusat | Stage 5: tambah calibration mode (5-point fit) |
| ISSF zone radii adalah default; tak kalibrasi ke saiz sebenar papan | Stage 5: input saiz papan + offset gain |
| Tiada storan persistent → score reset bila reboot | Stage 6: SPIFFS log |
| 60ms debounce → tak boleh handle split < 0.10s (world-class IPSC) | Phase 3: TDoA dengan i2s_adc DMA |

## Roadmap

- ✅ Stage 1: Lower debounce + ringing filter
- ✅ Stage 2: Split-time tracking + dashboard
- ✅ Stage 3: Amplitude triangulation core
- ✅ Stage 4: Target visualization (canvas dots)
- ⏳ Stage 5: ISSF score zones + 5-point calibration
- ⏳ Stage 6: SPIFFS session log + CSV export
