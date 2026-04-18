# Wiring

## Target node (1 of 15)

```
  ESP32 30-Pin Dev Board
  ┌───────────────────────────────┐
  │ GPIO32 ──┬─ 1 MΩ ─ GND        │
  │          └─ Piezo 1 (+)       │
  │ GPIO33 ──┬─ 1 MΩ ─ GND        │
  │          └─ Piezo 2 (+)       │
  │ GPIO34 ──┬─ 1 MΩ ─ GND        │
  │          └─ Piezo 3 (+)       │
  │ GPIO35 ──┬─ 1 MΩ ─ GND        │
  │          └─ Piezo 4 (+)       │
  │ GPIO36 (VP) ─ Battery divider │
  │     (2×100 kΩ → Vbatt to 3V3) │
  │ GND    ─ Piezo (–) (all)      │
  │ 5V USB ─ Power bank           │
  └───────────────────────────────┘
```

### Piezo positions on target plate

Looking at the **front** of the target:

```
  ┌───────────────────────────┐
  │   ●  S1          S2  ●    │
  │   (zone 10)   (zone 8)    │
  │                           │
  │            🎯             │
  │                           │
  │   ●  S3          S4  ●    │
  │   (zone 6)    (zone 4)    │
  └───────────────────────────┘
```

Stick piezos on the **back** of the plate with thick double-sided tape on
a clean surface. Piezo wires: **black → GND**, **red → signal**.

## Central receiver (1 unit)

```
  ESP32 30-Pin Dev Board
  ┌───────────────────────────────┐
  │ GPIO25 ── Buzzer (+)          │
  │ GND    ── Buzzer (–)          │
  │ 5V USB ── Power bank          │
  └───────────────────────────────┘
```

Use a passive piezo buzzer or small speaker; ledc driver sets the tone
frequency in firmware.

## Networking

- Central runs an **access point**: `SSID=Target_System`, `PSK=12345678`.
- mDNS: dashboard reachable at `http://kik.local/`.
- Targets → central: **ESP-NOW** (Long Range mode, no WiFi channel change).
- Range test: works up to ~200 m with line-of-sight; indoors expect ~40 m.
