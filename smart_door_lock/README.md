# Smart Door Lock System — ESP32 + Telegram

> Design and Development of Smart Door Lock System Using ESP32
> with Telegram Notification for Ship Security Applications

**Door ID:** `SHIP-DR-01` &nbsp;|&nbsp; **Location:** Communication Room

---

## Features

| Feature | Description |
|---|---|
| PIN Keypad 4×4 | Enter security PIN to unlock |
| LCD 20×4 (I2C) | Real-time status display |
| Green LED | Access granted indicator |
| Red LED | Access denied indicator |
| Buzzer | Audio feedback & security alarm |
| 12 V Electronic Lock | Relay-driven solenoid lock |
| Telegram Notifications | Real-time alerts to your phone |
| Remote Control | Unlock/lock door via Telegram bot |
| Intrusion Detection | Lockout + alert after 3 wrong PINs |

## Telegram Commands

| Command | Action |
|---|---|
| `/unlock` | Unlock door remotely |
| `/lock` | Lock door remotely |
| `/status` | Get current door status |
| `/reset` | Reset failed attempt counter |
| `/setpin 5678` | Change the access PIN |
| `/help` | Show available commands |

## Telegram Notifications

The system sends automatic alerts for:

- **Access Granted** — door unlocked via correct PIN
- **Door Locked** — door re-locked after timeout
- **Access Denied** — wrong PIN entered
- **High Security Alert** — 3 consecutive wrong PINs

## Wiring Diagram

```
ESP32 Pin   Component              Notes
─────────   ─────────              ─────
GPIO 26     Relay IN               12 V electronic lock (active LOW)
GPIO 27     Green LED (+)          220 Ω resistor to GND
GPIO 14     Red LED (+)            220 Ω resistor to GND
GPIO 25     Buzzer (+)             passive buzzer to GND

GPIO 13     Keypad Row 1           4×4 membrane keypad
GPIO 12     Keypad Row 2
GPIO 15     Keypad Row 3
GPIO  2     Keypad Row 4
GPIO  4     Keypad Col 1
GPIO 16     Keypad Col 2
GPIO 17     Keypad Col 3
GPIO  5     Keypad Col 4

GPIO 21     LCD SDA (I2C)          20×4 LCD with I2C backpack
GPIO 22     LCD SCL (I2C)          address 0x27 (or 0x3F)

VIN (5 V)   LCD VCC, Relay VCC
GND         Common ground
External    12 V supply → Relay COM/NO → Lock solenoid
```

### Wiring Notes

1. **Relay module**: Use a single-channel 5 V relay module.
   Connect the 12 V lock between relay **COM** and **NO** terminals,
   with the 12 V external supply providing power.
2. **LEDs**: Connect anode (+) to GPIO through a 220 Ω resistor; cathode to GND.
3. **I2C LCD**: Default address `0x27`. If your display uses `0x3F`, change
   the address in the sketch (`LiquidCrystal_I2C lcd(0x3F, 20, 4);`).
4. **Keypad**: Connect the 8-pin ribbon directly to the GPIOs listed above.

## Required Libraries

Install these via the Arduino Library Manager:

| Library | Author |
|---|---|
| `Keypad` | Mark Stanley & Alexander Brevig |
| `LiquidCrystal_I2C` | Frank de Brabander |
| `UniversalTelegramBot` | Brian Lough |
| `ArduinoJson` | Benoit Blanchon |

## Setup

1. **Arduino IDE** → Board Manager → install **"ESP32 by Espressif Systems"**.
2. Install the four libraries listed above.
3. Open `smart_door_lock.ino`.
4. Edit the configuration section at the top:
   ```cpp
   const char *WIFI_SSID     = "YOUR_WIFI_SSID";
   const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char *BOT_TOKEN     = "YOUR_BOT_TOKEN";
   const char *CHAT_ID       = "YOUR_CHAT_ID";
   ```
5. Select board **ESP32 Dev Module** and the correct COM port.
6. Upload.

### Getting a Telegram Bot Token

1. Open Telegram → search for **@BotFather**.
2. Send `/newbot` → follow the prompts → copy the token.

### Getting Your Chat ID

1. Open Telegram → search for **@userinfobot**.
2. Send `/start` → it replies with your numeric Chat ID.

## How to Use

| Action | Steps |
|---|---|
| **Unlock** | Type PIN on keypad → press `#` |
| **Clear input** | Press `*` to clear and re-enter |
| **Remote unlock** | Send `/unlock` to the Telegram bot |
| **Remote lock** | Send `/lock` to the Telegram bot |
| **Change PIN** | Send `/setpin <new PIN>` via Telegram |

The door **automatically re-locks** after 5 seconds.

After **3 wrong PINs**, the keypad is locked out for 30 seconds
and a high-security alert is sent to Telegram.

## Project Objectives

1. Develop a smart access control system using ESP32.
2. Control the door lock electronically using a security PIN.
3. Send real-time notifications via Telegram.
4. Enhance security for critical spaces on ships.
5. Provide an affordable and easy-to-deploy monitoring system.

## License

MIT — see [LICENSE](../LICENSE).
