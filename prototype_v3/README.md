# KIK Shooting Target — Prototype V3

V3 = V2 stable (commit `6753cef`) + redesigned dashboard.

## What changes from V2

- **Dashboard (HTML/CSS/JS in `central_v3.ino`)**: tablet-friendly redesign. Less clutter, larger touch targets, clearer OK / MATI status.
- **Firmware logic**: unchanged. Hit detection, heartbeat, Health packet (baseline + peak) format are byte-for-byte identical to V2 stable, so a V3 central can pair with a V2 stable target during a staged rollout.

## What is NOT in V3

- Autonomous self-test (capacitance probe every 5 s) — deferred. See PR #3 (`devin/1777645033-prototype-v2`) for in-progress work on that path.
- Hardware changes — none. Same wiring as V2: piezo on GPIO 32/33/34/35, 1 MΩ pull-down to GND on each pin.

## Layout

```
prototype_v3/
├── target_v3/
│   └── target_v3.ino     # one per board, edit TARGET_ID + RECEIVER_MAC
├── central_v3/
│   └── central_v3.ino    # single central, hosts dashboard at http://192.168.4.1/
└── README.md
```

## Quick flash guide

1. **Central** (one board, MAC ends in `F1:61` on this rig):
   - Open `prototype_v3/central_v3/central_v3.ino` in Arduino IDE
   - Tools → Board → ESP32 Dev Module
   - Tools → Upload Speed → 115200 (reliable on flaky USB)
   - Tools → Erase All Flash → Disabled (faster, lower risk of fail mid-write)
   - Upload, then open Serial Monitor at 115200 to confirm `KIK Central V3 ready` and copy the printed MAC.

2. **Targets** (1..15 boards):
   - Open `prototype_v3/target_v3/target_v3.ino`
   - Edit two lines per board:
     - `#define TARGET_ID 1` (1..15, must be unique across all boards)
     - `RECEIVER_MAC[]` (MAC printed by central in step 1)
   - Same upload settings as central.

3. **Tablet**:
   - Connect to SSID `KIK-Target` (password in `central_v3.ino`)
   - Open `http://192.168.4.1/`

## Power supply (important)

The target's WiFi TX draws ~600 mA spikes. A laptop USB-2 port (~500 mA budget) browns out and crashes — you'll see `TG1WDT_SYS_RESET` in the loop. Use one of:

- 5 V / 1 A or 2 A phone charger (recommended)
- Power bank with `5V 2A` output
- USB 3.0 port on the laptop (900 mA budget)

If you must run on USB-2, add a bulk capacitor (470–1000 µF) on the 5 V rail.

## Branch / PR

Tracking branch: `devin/1777818815-prototype-v3` (this branch).
PR: opened separately, dashboard redesign lands in follow-up commits.
