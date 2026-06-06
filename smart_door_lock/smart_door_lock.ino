/*
 * Smart Door Lock System Using ESP32 with Telegram Notification
 * For Ship Security Applications
 *
 * Door ID  : SHIP-DR-01
 * Location : Communication Room
 *
 * Hardware:
 *   - ESP32 Dev Module
 *   - 4x3 Membrane Matrix Keypad (12 keys) — Techmakers
 *   - 20x4 I2C LCD Display
 *   - Green LED  (access granted)
 *   - Red LED    (access denied)
 *   - Buzzer (audio feedback & alarm)
 *   - Relay Module (5V, single channel)
 *   - 12V DC Solenoid Door Lock
 *   - AC to DC Power Supply Adapter 12V 1A
 *   - Female DC Power Jack Socket DC-005 (5.5mm x 2.1mm)
 *
 * Features:
 *   - PIN access via 4x4 Keypad
 *   - 20x4 I2C LCD status display
 *   - Green LED  (access granted)
 *   - Red LED    (access denied)
 *   - Buzzer warning
 *   - 12V electronic lock via relay
 *   - Real-time Telegram notifications
 *   - Remote control via Telegram bot
 *   - Unauthorized access attempt detection
 *
 * Libraries required:
 *   - Keypad              (Mark Stanley, Alexander Brevig)
 *   - LiquidCrystal_I2C   (Frank de Brabander)
 *   - UniversalTelegramBot (Brian Lough)
 *   - ArduinoJson         (Benoit Blanchon)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

/* ───────────────────────── Configuration ───────────────────────── */

// WiFi credentials — change to match your ship's network
const char *WIFI_SSID     = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Telegram Bot token — obtain from @BotFather on Telegram
const char *BOT_TOKEN = "YOUR_BOT_TOKEN";

// Telegram Chat ID — send /start to @userinfobot to get yours
const char *CHAT_ID   = "YOUR_CHAT_ID";

// Door identification
const char *DOOR_ID   = "SHIP-DR-01";
const char *LOCATION  = "Communication Room";

// Security PIN (default). Change this before deployment.
String correctPIN = "1234";

// NTP server for real-time clock
const char *NTP_SERVER  = "pool.ntp.org";
const long  GMT_OFFSET  = 28800;   // UTC+8 (Malaysia)  — adjust as needed
const int   DST_OFFSET  = 0;

// Telegram bot poll interval (ms)
const unsigned long BOT_POLL_INTERVAL = 1500;

// Lock timing
const unsigned long LOCK_OPEN_DURATION = 5000;  // door stays unlocked (ms)

// Maximum wrong attempts before high-security alert
const int MAX_FAILED_ATTEMPTS = 3;

/* ───────────────────────── Pin assignments ──────────────────────── */

// Relay (12 V electronic lock)
#define RELAY_PIN    26

// *** RELAY POLARITY SETTING ***
// Most relay modules are active LOW (LOW signal = relay ON).
// If door OPENS when ESP32 boots, try changing true ↔ false.
#define RELAY_ACTIVE_LOW  true

// Derived constants — do not edit
#define RELAY_LOCK    (RELAY_ACTIVE_LOW ? HIGH : LOW)
#define RELAY_UNLOCK  (RELAY_ACTIVE_LOW ? LOW  : HIGH)

// LEDs
#define GREEN_LED    27
#define RED_LED      14

// Buzzer
#define BUZZER_PIN   25

/* ───────────────────────── Keypad setup ─────────────────────────── */

const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// Keypad row and column pins — adjust for your wiring
byte rowPins[ROWS] = {13, 12, 15, 2};
byte colPins[COLS] = {4,  16, 17};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/* ───────────────────────── LCD setup ────────────────────────────── */

// 20x4 I2C LCD at address 0x27 (common default).
// Change to 0x3F if your module uses that address.
LiquidCrystal_I2C lcd(0x27, 20, 4);

/* ───────────────────────── Custom LCD characters ────────────────── */

// Lock icon
byte lockChar[8] = {
  0b01110,
  0b10001,
  0b10001,
  0b11111,
  0b11011,
  0b11011,
  0b11111,
  0b00000
};

// Unlock icon
byte unlockChar[8] = {
  0b01110,
  0b10000,
  0b10000,
  0b11111,
  0b11011,
  0b11011,
  0b11111,
  0b00000
};

/* ───────────────────────── Globals ──────────────────────────────── */

WiFiClientSecure secured;
UniversalTelegramBot bot(BOT_TOKEN, secured);

String enteredPIN     = "";
int    failedAttempts  = 0;
bool   doorLocked      = true;
bool   systemLocked    = false;   // lockout after MAX_FAILED_ATTEMPTS

unsigned long lastBotPoll    = 0;
unsigned long unlockTime     = 0;  // millis() when door was unlocked
unsigned long lockoutStart   = 0;
const unsigned long LOCKOUT_DURATION = 30000;  // 30 s lockout

/* ───────────────────────── Helper: timestamp ────────────────────── */

String getDate() {
  struct tm t;
  if (!getLocalTime(&t)) return "??/??/????";
  char buf[11];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  return String(buf);
}

String getTime() {
  struct tm t;
  if (!getLocalTime(&t)) return "??:??";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

/* ───────────────────────── Telegram helpers ─────────────────────── */

void sendTelegram(const String &msg) {
  bot.sendMessage(CHAT_ID, msg, "");
}

void sendAccessGranted() {
  String msg = "\xF0\x9F\x94\x93 ACCESS GRANTED\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Status   : UNLOCKED\n\n";
  msg += "Date     : " + getDate() + "\n";
  msg += "Time     : " + getTime();
  sendTelegram(msg);
}

void sendDoorLocked() {
  String msg = "\xF0\x9F\x94\x92 DOOR LOCKED\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Status   : LOCKED\n\n";
  msg += "Date     : " + getDate() + "\n";
  msg += "Time     : " + getTime();
  sendTelegram(msg);
}

void sendAccessDenied() {
  String msg = "\xE2\x9A\xA0\xEF\xB8\x8F ACCESS DENIED\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Unauthorized Access Attempt\n\n";
  msg += "Date     : " + getDate() + "\n";
  msg += "Time     : " + getTime();
  sendTelegram(msg);
}

void sendHighSecurityAlert() {
  String msg = "\xF0\x9F\x9A\xA8 HIGH SECURITY ALERT\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Failed Attempts : " + String(MAX_FAILED_ATTEMPTS) + "\n\n";
  msg += "Immediate Inspection Required";
  sendTelegram(msg);
}

void sendStatusReport() {
  String msg = "\xF0\x9F\x93\x8B STATUS REPORT\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Status   : " + String(doorLocked ? "LOCKED" : "UNLOCKED") + "\n";
  msg += "Failed   : " + String(failedAttempts) + "\n\n";
  msg += "Date     : " + getDate() + "\n";
  msg += "Time     : " + getTime();
  sendTelegram(msg);
}

/* ───────────────────────── Telegram bot commands ────────────────── */

void handleTelegramMessages(int numMessages) {
  for (int i = 0; i < numMessages; i++) {
    String chatId = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;

    // Only respond to the authorized chat
    if (chatId != CHAT_ID) {
      bot.sendMessage(chatId, "\xE2\x9B\x94 Unauthorized user.", "");
      continue;
    }

    text.trim();

    if (text == "/unlock") {
      failedAttempts = 0;
      systemLocked   = false;
      unlockDoor();
      sendAccessGranted();
      bot.sendMessage(chatId, "\xE2\x9C\x85 Door unlocked remotely.", "");
    }
    else if (text == "/lock") {
      lockDoor();
      sendDoorLocked();
      bot.sendMessage(chatId, "\xE2\x9C\x85 Door locked remotely.", "");
    }
    else if (text == "/status") {
      sendStatusReport();
    }
    else if (text == "/reset") {
      failedAttempts = 0;
      systemLocked   = false;
      bot.sendMessage(chatId, "\xE2\x9C\x85 Failed attempts reset to 0.\nSystem unlocked.", "");
      lcdShowReady();
    }
    else if (text.startsWith("/setpin ")) {
      String newPin = text.substring(8);
      newPin.trim();
      bool allDigits = newPin.length() >= 4 && newPin.length() <= 8;
      for (unsigned int ci = 0; allDigits && ci < newPin.length(); ci++) {
        if (newPin[ci] < '0' || newPin[ci] > '9') allDigits = false;
      }
      if (allDigits) {
        correctPIN = newPin;
        bot.sendMessage(chatId, "\xE2\x9C\x85 PIN changed successfully.", "");
      } else {
        bot.sendMessage(chatId, "\xE2\x9D\x8C PIN must be 4-8 digits (0-9 only).", "");
      }
    }
    else if (text == "/help" || text == "/start") {
      String help = "\xF0\x9F\x94\x90 *Smart Door Lock*\n";
      help += "Door: " + String(DOOR_ID) + "\n\n";
      help += "/unlock  - Unlock door remotely\n";
      help += "/lock    - Lock door remotely\n";
      help += "/status  - Door status report\n";
      help += "/reset   - Reset failed attempts\n";
      help += "/setpin  - Change PIN (e.g. /setpin 5678)\n";
      help += "/help    - Show this help";
      bot.sendMessage(chatId, help, "Markdown");
    }
    else {
      bot.sendMessage(chatId, "\xE2\x9D\x93 Unknown command. Send /help", "");
    }
  }
}

/* ───────────────────────── Lock control ─────────────────────────── */

void unlockDoor() {
  doorLocked = false;
  digitalWrite(RELAY_PIN, RELAY_UNLOCK);  // energise relay — unlock
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED,   LOW);
  unlockTime = millis();

  lcdShowUnlocked();
  toneGranted();
}

void lockDoor() {
  doorLocked = true;
  digitalWrite(RELAY_PIN, RELAY_LOCK);    // de-energise relay — lock
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);
  unlockTime = 0;

  lcdShowReady();
}

/* ───────────────────────── Feedback: buzzer ─────────────────────── */

void toneGranted() {
  tone(BUZZER_PIN, 1000, 150);
  delay(200);
  tone(BUZZER_PIN, 1500, 150);
  delay(200);
  noTone(BUZZER_PIN);
}

void toneDenied() {
  tone(BUZZER_PIN, 400, 300);
  delay(400);
  tone(BUZZER_PIN, 300, 300);
  delay(400);
  noTone(BUZZER_PIN);
}

void toneAlert() {
  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, 2000, 200);
    delay(250);
    tone(BUZZER_PIN, 1000, 200);
    delay(250);
  }
  noTone(BUZZER_PIN);
}

/* ───────────────────────── LCD helpers ──────────────────────────── */

void lcdShowReady() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  SMART DOOR LOCK   ");
  lcd.setCursor(0, 1);
  lcd.print("  " + String(DOOR_ID) + "          ");
  lcd.setCursor(0, 2);
  lcd.print("Enter PIN:          ");
  lcd.setCursor(0, 3);
  lcd.print("PIN> ");
}

void lcdShowUnlocked() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(1);  // unlock icon
  lcd.print(" ACCESS GRANTED  ");
  lcd.setCursor(0, 1);
  lcd.print("  Door Unlocked     ");
  lcd.setCursor(0, 2);
  lcd.print("  " + String(DOOR_ID) + "          ");
  lcd.setCursor(0, 3);
  lcd.print("  " + getTime() + "              ");
}

void lcdShowDenied() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(0);  // lock icon
  lcd.print(" ACCESS DENIED   ");
  lcd.setCursor(0, 1);
  lcd.print("  Wrong PIN!        ");
  lcd.setCursor(0, 2);
  lcd.print("  Attempts: ");
  lcd.print(failedAttempts);
  lcd.print("/");
  lcd.print(MAX_FAILED_ATTEMPTS);
  lcd.setCursor(0, 3);
  lcd.print("                    ");
}

void lcdShowLockout() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("!! SECURITY ALERT !!");
  lcd.setCursor(0, 1);
  lcd.print(" System Locked Out  ");
  lcd.setCursor(0, 2);
  lcd.print(" Too Many Attempts  ");
  lcd.setCursor(0, 3);
  lcd.print(" Contact Security   ");
}

void lcdShowConnecting() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  SMART DOOR LOCK   ");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi...  ");
  lcd.setCursor(0, 2);
  lcd.print("SSID: ");
  lcd.print(WIFI_SSID);
  lcd.setCursor(0, 3);
  lcd.print("                    ");
}

/* ───────────────────────── PIN processing ───────────────────────── */

void processKey(char key) {
  if (systemLocked) return;  // ignore keypad during lockout

  if (key == '#') {
    // '#' = confirm / submit PIN
    if (enteredPIN.length() == 0) {
      return;
    }
    if (enteredPIN == correctPIN) {
      // Correct PIN
      failedAttempts = 0;
      unlockDoor();
      sendAccessGranted();
    } else {
      // Wrong PIN
      failedAttempts++;
      digitalWrite(RED_LED, HIGH);
      lcdShowDenied();
      toneDenied();
      sendAccessDenied();

      if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
        systemLocked = true;
        lockoutStart = millis();
        lcdShowLockout();
        toneAlert();
        sendHighSecurityAlert();
      } else {
        delay(2000);
        digitalWrite(RED_LED, LOW);
        lcdShowReady();
      }
    }
    enteredPIN = "";
  }
  else if (key == '*') {
    // '*' = clear / backspace
    enteredPIN = "";
    lcdShowReady();
  }
  else if (key >= '0' && key <= '9') {
    // Digit entry (max 8 digits)
    if (enteredPIN.length() < 8) {
      enteredPIN += key;

      // Show masked PIN on LCD
      lcd.setCursor(5, 3);
      for (unsigned int j = 0; j < enteredPIN.length(); j++) {
        lcd.print('*');
      }
      lcd.print("               ");

      // Short beep for key press feedback
      tone(BUZZER_PIN, 800, 50);
      delay(60);
      noTone(BUZZER_PIN);
    }
  }
  // A, B, C, D keys are ignored
}

/* ───────────────────────── setup() ──────────────────────────────── */

void setup() {
  // *** CRITICAL: Set relay pin FIRST to keep door locked during boot ***
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_LOCK);

  Serial.begin(115200);
  Serial.println("\n[Smart Door Lock] Booting...");
  Serial.print("[Relay] Active LOW = ");
  Serial.println(RELAY_ACTIVE_LOW ? "true" : "false");
  Serial.print("[Relay] LOCK signal = ");
  Serial.println(RELAY_LOCK ? "HIGH" : "LOW");

  // GPIO setup
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED,   OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Start locked
  digitalWrite(RELAY_PIN, RELAY_LOCK);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   LOW);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, lockChar);
  lcd.createChar(1, unlockChar);
  lcdShowConnecting();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    lcd.setCursor(0, 2);
    lcd.print("WiFi OK             ");
    lcd.setCursor(0, 3);
    lcd.print(WiFi.localIP().toString() + "        ");
  } else {
    Serial.println("\n[WiFi] Connection failed — offline mode");
    lcd.setCursor(0, 2);
    lcd.print("WiFi FAILED         ");
    lcd.setCursor(0, 3);
    lcd.print("Offline mode        ");
  }

  // NTP time sync
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  Serial.println("[NTP] Syncing...");

  // TLS — skip certificate verification for maximum compatibility
  secured.setInsecure();

  delay(2000);
  lcdShowReady();

  // Startup notification
  String msg = "\xF0\x9F\x9F\xA2 SYSTEM ONLINE\n\n";
  msg += "Door ID  : " + String(DOOR_ID) + "\n";
  msg += "Location : " + String(LOCATION) + "\n\n";
  msg += "Status   : LOCKED\n";
  msg += "IP       : " + WiFi.localIP().toString() + "\n\n";
  msg += "Date     : " + getDate() + "\n";
  msg += "Time     : " + getTime();
  sendTelegram(msg);

  Serial.println("[System] Ready.");
}

/* ───────────────────────── loop() ───────────────────────────────── */

void loop() {
  // --- Lockout timer ---
  if (systemLocked) {
    if (millis() - lockoutStart >= LOCKOUT_DURATION) {
      systemLocked   = false;
      failedAttempts = 0;
      digitalWrite(RED_LED, LOW);
      lcdShowReady();
    }
  }

  // --- Auto-lock after LOCK_OPEN_DURATION ---
  if (!doorLocked && unlockTime > 0) {
    if (millis() - unlockTime >= LOCK_OPEN_DURATION) {
      lockDoor();
      sendDoorLocked();
    }
  }

  // --- Keypad ---
  char key = keypad.getKey();
  if (key) {
    Serial.print("[Key] ");
    Serial.println(key);
    processKey(key);
  }

  // --- Telegram bot polling ---
  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastBotPoll > BOT_POLL_INTERVAL) {
      int numMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numMessages) {
        handleTelegramMessages(numMessages);
        numMessages = bot.getUpdates(bot.last_message_received + 1);
      }
      lastBotPoll = millis();
    }
  }

  // --- WiFi reconnect ---
  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 30000) {
    Serial.println("[WiFi] Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastReconnect = millis();
  }
}
