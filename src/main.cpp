// ═══════════════════════════════════════════════════════════════════════════
//  Desky – The Companion  v1.0
//  A desk-pet that keeps time, runs work/break sessions, reminds you to
//  drink water, and reacts to your touch (button) and RFID card taps.
//
//  Hardware:
//    NodeMCU ESP-12E          – the brain
//    OLED A  (0x3C, I2C)     – face / expressions
//    OLED B  (0x3D, I2C)     – info panel (clock, mode, timers)
//    RC522   (SPI)           – RFID mode switching
//    SG90 servo              – physical gestures
//    Push button             – interaction trigger
//    Potentiometer (A0)      – session length dial (5-60 min)
//
//  Pin map (NodeMCU):
//    I2C  SDA = D2 (GPIO4)   SCL = D1 (GPIO5)
//    SPI  SCK = D5 (GPIO14)  MISO = D6 (GPIO12)  MOSI = D7 (GPIO13)
//    RC522 SS = D4 (GPIO2)   RST = tied to 3V3
//    Servo    = D8 (GPIO15)  ← change if you wire differently
//    Button   = D3 (GPIO0)   ← change if you wire differently
//    Pot      = A0
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <Servo.h>
#include <U8g2lib.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <time.h>

#include "Animation.h"

// ─── Wi-Fi credentials ─────────────────────────────────────────────────────
// Copy credentials_example.h → credentials.h and add your SSID/password.
// credentials.h is git-ignored so your password stays private.
#include "credentials.h"

// ─── Pin definitions ────────────────────────────────────────────────────────
#define PIN_SERVO D8    // SG90 signal – change if needed
#define PIN_BUTTON D3   // Push button  – change if needed
#define PIN_POT A0      // Potentiometer
#define PIN_RC522_SS D4 // RC522 SDA/SS (GPIO2)
// RC522 RST is tied to 3V3 (always enabled)

// ─── OLED displays (U8g2, I2C, hardware address) ───────────────────────────
// Face display at 0x3C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oledFace(U8G2_R0,
                                             /* reset */ U8X8_PIN_NONE);
// Info panel at 0x3D
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oledInfo(U8G2_R0,
                                             /* reset */ U8X8_PIN_NONE);

// ─── RFID ───────────────────────────────────────────────────────────────────
// RST_PIN = UINT8_MAX signals "no reset pin" to the library (tied to 3V3)
MFRC522 rfid(PIN_RC522_SS, UINT8_MAX);

// ─── Servo ──────────────────────────────────────────────────────────────────
Servo servo;

// ─── Animation player for OLED A (face) ─────────────────────────────────────
DeskyAnimation faceAnim;

// ─── NTP / Time ─────────────────────────────────────────────────────────────
// IST = UTC+5:30 = 19800 seconds
#define IST_OFFSET_SEC 19800

// ─── Desky modes ────────────────────────────────────────────────────────────
enum class DeskyMode : uint8_t {
  IDLE,
  WORK,
  BREAK_TIME,
  // Future:
  // SLEEP,
  // MILK
};

// ─── Global state ───────────────────────────────────────────────────────────
DeskyMode currentMode = DeskyMode::IDLE;
DeskyExpression currentFace = DeskyExpression::NEUTRAL;
bool sessionActive = false;
uint32_t sessionStartMs = 0;
uint32_t sessionDurationMs = 0; // set by potentiometer
uint32_t sessionElapsedMs = 0;
uint32_t totalWorkMs = 0;  // accumulated work time
uint32_t totalBreakMs = 0; // accumulated break time
uint8_t sessionsCompleted = 0;

// Water reminder: every 20 minutes during work
#define WATER_INTERVAL_MS (20UL * 60UL * 1000UL)
uint32_t lastWaterRemindMs = 0;
bool waterReminderActive = false;

// Button debounce
uint32_t lastButtonPressMs = 0;
#define BUTTON_DEBOUNCE_MS 300

// Info panel refresh
uint32_t lastInfoRefreshMs = 0;
#define INFO_REFRESH_MS 500

// Idle face change (periodic blink / look around)
uint32_t lastIdleAnimMs = 0;
#define IDLE_ANIM_INTERVAL 8000

// Servo gesture state
bool servoAttached = false;
uint32_t servoDetachAt = 0;

// Potentiometer smoothing
int potSmoothed = 512;

// RFID card UIDs (will be learned on first two taps)
uint8_t workCardUID[7] = {0};
uint8_t workCardUIDLen = 0;
uint8_t breakCardUID[7] = {0};
uint8_t breakCardUIDLen = 0;
bool workCardLearned = false;
bool breakCardLearned = false;

// Session summary display
bool showingSummary = false;
uint32_t summaryStartMs = 0;
#define SUMMARY_DISPLAY_MS 8000

// ─── Forward declarations ───────────────────────────────────────────────────
void connectWiFi();
void syncTime();
void setupOLEDs();
void setupRFID();
void setupServo();

void handleButton();
void handleRFID();
void handlePotentiometer();
void handleWaterReminder();
void handleIdleAnimation();
void handleServoTimeout();
void handleSessionTimer();

void setFace(DeskyExpression expr, uint16_t frameMs = 120);
void setMode(DeskyMode mode);
void startSession(DeskyMode mode);
void endSession();
void showSessionSummary();

void servoGesture(const char *type);
void servoNod();
void servoWiggle();
void servoNudge();

void drawInfoPanel();
void drawClock(uint8_t y);
void drawSessionTimer(uint8_t y);
void drawModeBadge(uint8_t y);
void drawPotBar(uint8_t y);
void drawSummary();
void drawCardSetup();

String getTimeStr();
uint32_t potToSessionMs();
bool uidMatch(const uint8_t *a, uint8_t aLen, const uint8_t *b, uint8_t bLen);

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\n=== Desky – The Companion v1.0 ==="));

  // ── Pin modes ──
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // ── I2C ──
  Wire.begin(D2, D1);    // SDA=D2, SCL=D1
  Wire.setClock(400000); // 400 kHz fast mode

  // ── OLEDs ──
  setupOLEDs();

  // ── Boot splash on face ──
  oledFace.clearBuffer();
  oledFace.setFont(u8g2_font_helvB14_tr);
  oledFace.drawStr(28, 30, "Desky");
  oledFace.setFont(u8g2_font_helvR08_tr);
  oledFace.drawStr(22, 48, "waking up...");
  oledFace.sendBuffer();

  // ── Boot info on panel ──
  oledInfo.clearBuffer();
  oledInfo.setFont(u8g2_font_helvR08_tr);
  oledInfo.drawStr(4, 12, "Connecting to WiFi...");
  oledInfo.sendBuffer();

  // ── Wi-Fi ──
  connectWiFi();

  // ── NTP time sync ──
  oledInfo.clearBuffer();
  oledInfo.setFont(u8g2_font_helvR08_tr);
  oledInfo.drawStr(4, 12, "Syncing time (IST)...");
  oledInfo.sendBuffer();
  syncTime();

  // ── SPI + RFID ──
  SPI.begin();
  setupRFID();

  // ── Servo ──
  setupServo();

  // ── Ready! ──
  Serial.println(F("Desky is ready!"));
  setFace(DeskyExpression::HAPPY);

  oledInfo.clearBuffer();
  oledInfo.setFont(u8g2_font_helvB10_tr);
  oledInfo.drawStr(30, 35, "Ready!");
  oledInfo.sendBuffer();
  delay(800);

  // ── Show card setup if cards not learned ──
  lastInfoRefreshMs = millis();
  lastIdleAnimMs = millis();
  lastWaterRemindMs = millis();
}

// ═══════════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════════
void loop() {
  uint32_t now = millis();

  // ── Face animation update ──
  faceAnim.update(oledFace, now);

  // ── Button ──
  handleButton();

  // ── RFID ──
  handleRFID();

  // ── Potentiometer ──
  handlePotentiometer();

  // ── Session timer ──
  handleSessionTimer();

  // ── Water reminder (work mode only) ──
  handleWaterReminder();

  // ── Idle animation (only when idle and not animating) ──
  if (currentMode == DeskyMode::IDLE && !faceAnim.isPlaying()) {
    handleIdleAnimation();
  }

  // ── Info panel refresh ──
  if (now - lastInfoRefreshMs >= INFO_REFRESH_MS) {
    lastInfoRefreshMs = now;
    if (showingSummary) {
      if (now - summaryStartMs >= SUMMARY_DISPLAY_MS) {
        showingSummary = false;
        setMode(DeskyMode::IDLE);
      } else {
        drawSummary();
      }
    } else {
      drawInfoPanel();
    }
  }

  // ── Servo auto-detach ──
  handleServoTimeout();

  yield(); // ESP8266 watchdog
}

// ═══════════════════════════════════════════════════════════════════════════
//  Wi-Fi
// ═══════════════════════════════════════════════════════════════════════════
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s ", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print('.');
    attempts++;

    // Animate dots on info panel
    oledInfo.clearBuffer();
    oledInfo.setFont(u8g2_font_helvR08_tr);
    String dots = "Connecting";
    for (int i = 0; i < (attempts % 4); i++)
      dots += ".";
    oledInfo.drawStr(4, 12, dots.c_str());
    oledInfo.drawStr(4, 28, WIFI_SSID);
    oledInfo.sendBuffer();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    oledInfo.clearBuffer();
    oledInfo.setFont(u8g2_font_helvR08_tr);
    oledInfo.drawStr(4, 12, "WiFi connected!");
    oledInfo.drawStr(4, 28, WiFi.localIP().toString().c_str());
    oledInfo.sendBuffer();
    delay(600);
  } else {
    Serial.println(F("\nWiFi failed! Running offline."));
    oledInfo.clearBuffer();
    oledInfo.setFont(u8g2_font_helvR08_tr);
    oledInfo.drawStr(4, 12, "WiFi failed!");
    oledInfo.drawStr(4, 28, "Running offline");
    oledInfo.sendBuffer();
    delay(1000);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  NTP Time Sync (IST)
// ═══════════════════════════════════════════════════════════════════════════
void syncTime() {
  // Use ESP8266's built-in SNTP
  configTime(IST_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print(F("Waiting for NTP sync"));
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 100000 && tries < 20) {
    delay(500);
    Serial.print('.');
    now = time(nullptr);
    tries++;
  }
  Serial.println();
  if (now > 100000) {
    struct tm *tm = localtime(&now);
    Serial.printf("Time synced: %02d:%02d:%02d IST\n", tm->tm_hour, tm->tm_min,
                  tm->tm_sec);
  } else {
    Serial.println(F("NTP sync failed, clock may be wrong."));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  OLED Setup
// ═══════════════════════════════════════════════════════════════════════════
void setupOLEDs() {
  // Face display at 0x3C
  oledFace.setI2CAddress(0x3C * 2); // U8g2 uses 8-bit address
  oledFace.begin();
  oledFace.setFontMode(1);
  oledFace.setBitmapMode(1);

  // Info panel at 0x3D
  oledInfo.setI2CAddress(0x3D * 2); // U8g2 uses 8-bit address
  oledInfo.begin();
  oledInfo.setFontMode(1);
  oledInfo.setBitmapMode(1);

  Serial.println(F("OLEDs initialized (0x3C face, 0x3D info)"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  RFID Setup
// ═══════════════════════════════════════════════════════════════════════════
void setupRFID() {
  rfid.PCD_Init();
  delay(50);
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 firmware: 0x%02X %s\n", v,
                (v == 0x91 || v == 0x92) ? "(OK)" : "(check wiring!)");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Servo Setup
// ═══════════════════════════════════════════════════════════════════════════
void setupServo() {
  // Don't attach yet — attach only when moving, detach after
  // This avoids the servo jitter that can reset the NodeMCU
  Serial.println(F("Servo on D8 (attach on demand)"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  BUTTON HANDLER
// ═══════════════════════════════════════════════════════════════════════════
void handleButton() {
  if (digitalRead(PIN_BUTTON) == LOW) {
    uint32_t now = millis();
    if (now - lastButtonPressMs < BUTTON_DEBOUNCE_MS)
      return;
    lastButtonPressMs = now;

    Serial.println(F("Button pressed!"));

    // Pick a random reaction
    uint8_t reaction = random(0, 5);
    switch (reaction) {
    case 0:
      setFace(DeskyExpression::HAPPY, 100);
      servoGesture("wiggle");
      break;
    case 1:
      setFace(DeskyExpression::WINK, 120);
      break;
    case 2:
      setFace(DeskyExpression::SURPRISED, 90);
      servoGesture("nod");
      break;
    case 3:
      setFace(DeskyExpression::LAUGHING, 100);
      servoGesture("wiggle");
      break;
    case 4:
      setFace(DeskyExpression::LOVE, 130);
      servoGesture("nod");
      break;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  RFID HANDLER — card learning + mode switching
// ═══════════════════════════════════════════════════════════════════════════
void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return;

  // Print UID
  Serial.print(F("Card UID: "));
  for (uint8_t i = 0; i < rfid.uid.size; i++) {
    Serial.printf("%02X ", rfid.uid.uidByte[i]);
  }
  Serial.println();

  // ── Card learning phase ──
  if (!workCardLearned) {
    // First card becomes WORK card
    memcpy(workCardUID, rfid.uid.uidByte, rfid.uid.size);
    workCardUIDLen = rfid.uid.size;
    workCardLearned = true;
    Serial.println(F(">> WORK card learned!"));

    setFace(DeskyExpression::HAPPY, 100);
    oledInfo.clearBuffer();
    oledInfo.setFont(u8g2_font_helvB10_tr);
    oledInfo.drawStr(10, 20, "WORK card");
    oledInfo.drawStr(10, 38, "learned!");
    oledInfo.setFont(u8g2_font_helvR08_tr);
    oledInfo.drawStr(10, 55, "Now tap BREAK card");
    oledInfo.sendBuffer();
    delay(1500);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  if (!breakCardLearned) {
    // Check it's not the same card
    if (uidMatch(rfid.uid.uidByte, rfid.uid.size, workCardUID,
                 workCardUIDLen)) {
      oledInfo.clearBuffer();
      oledInfo.setFont(u8g2_font_helvR08_tr);
      oledInfo.drawStr(10, 30, "Same card! Use a");
      oledInfo.drawStr(10, 44, "different one.");
      oledInfo.sendBuffer();
      delay(1000);
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      return;
    }
    memcpy(breakCardUID, rfid.uid.uidByte, rfid.uid.size);
    breakCardUIDLen = rfid.uid.size;
    breakCardLearned = true;
    Serial.println(F(">> BREAK card learned!"));

    setFace(DeskyExpression::BIG_GRIN, 100);
    oledInfo.clearBuffer();
    oledInfo.setFont(u8g2_font_helvB10_tr);
    oledInfo.drawStr(10, 20, "BREAK card");
    oledInfo.drawStr(10, 38, "learned!");
    oledInfo.setFont(u8g2_font_helvR08_tr);
    oledInfo.drawStr(10, 55, "Desky is ready!");
    oledInfo.sendBuffer();
    servoGesture("nod");
    delay(1500);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  // ── Mode switching ──
  if (uidMatch(rfid.uid.uidByte, rfid.uid.size, workCardUID, workCardUIDLen)) {
    Serial.println(F(">> WORK card tapped"));
    if (currentMode == DeskyMode::WORK) {
      // Already working — show encouragement
      setFace(DeskyExpression::HAPPY, 100);
    } else {
      if (sessionActive)
        endSession();
      startSession(DeskyMode::WORK);
    }
  } else if (uidMatch(rfid.uid.uidByte, rfid.uid.size, breakCardUID,
                      breakCardUIDLen)) {
    Serial.println(F(">> BREAK card tapped"));
    if (currentMode == DeskyMode::BREAK_TIME) {
      setFace(DeskyExpression::WINK, 100);
    } else {
      if (sessionActive)
        endSession();
      startSession(DeskyMode::BREAK_TIME);
    }
  } else {
    // Unknown card
    Serial.println(F(">> Unknown card"));
    setFace(DeskyExpression::CONFUSED, 100);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ═══════════════════════════════════════════════════════════════════════════
//  POTENTIOMETER HANDLER
// ═══════════════════════════════════════════════════════════════════════════
void handlePotentiometer() {
  int raw = analogRead(PIN_POT);
  // Exponential smoothing
  potSmoothed = potSmoothed + ((raw - potSmoothed) >> 2);
}

uint32_t potToSessionMs() {
  // Map 0-1023 → 5-60 minutes in 5-minute steps
  int minutes = map(potSmoothed, 0, 1023, 1, 12); // 1-12 steps
  minutes = constrain(minutes, 1, 12) * 5;        // 5,10,...,60
  return (uint32_t)minutes * 60UL * 1000UL;
}

int potToSessionMinutes() {
  int minutes = map(potSmoothed, 0, 1023, 1, 12);
  return constrain(minutes, 1, 12) * 5;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SESSION MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════
void startSession(DeskyMode mode) {
  currentMode = mode;
  sessionActive = true;
  sessionStartMs = millis();
  sessionDurationMs = potToSessionMs();
  sessionElapsedMs = 0;
  lastWaterRemindMs = millis();
  waterReminderActive = false;
  showingSummary = false;

  uint32_t mins = sessionDurationMs / 60000UL;
  Serial.printf("Session started: %s for %lu min\n",
                mode == DeskyMode::WORK ? "WORK" : "BREAK", mins);

  if (mode == DeskyMode::WORK) {
    setFace(DeskyExpression::CURIOUS, 100); // focused look
    servoGesture("nod");
  } else {
    setFace(DeskyExpression::HAPPY, 120); // relaxed
    servoGesture("wiggle");
  }
}

void endSession() {
  if (!sessionActive)
    return;

  uint32_t elapsed = millis() - sessionStartMs;

  if (currentMode == DeskyMode::WORK) {
    totalWorkMs += elapsed;
  } else if (currentMode == DeskyMode::BREAK_TIME) {
    totalBreakMs += elapsed;
  }
  sessionsCompleted++;

  Serial.printf("Session ended. Total sessions: %d\n", sessionsCompleted);

  sessionActive = false;
  setFace(DeskyExpression::BIG_GRIN, 100);
  servoGesture("nod");

  // Show summary
  showingSummary = true;
  summaryStartMs = millis();
}

void handleSessionTimer() {
  if (!sessionActive)
    return;

  uint32_t now = millis();
  sessionElapsedMs = now - sessionStartMs;

  if (sessionElapsedMs >= sessionDurationMs) {
    // Session complete!
    Serial.println(F("Session time is up!"));
    setFace(DeskyExpression::SURPRISED, 80);
    servoGesture("wiggle");
    endSession();
  }
}

void showSessionSummary() {
  // Called by drawSummary() on the info panel
}

// ═══════════════════════════════════════════════════════════════════════════
//  WATER REMINDER (during WORK mode)
// ═══════════════════════════════════════════════════════════════════════════
void handleWaterReminder() {
  if (currentMode != DeskyMode::WORK || !sessionActive)
    return;

  uint32_t now = millis();
  if (now - lastWaterRemindMs >= WATER_INTERVAL_MS) {
    lastWaterRemindMs = now;
    waterReminderActive = true;

    Serial.println(F("Water reminder! 💧"));

    // Save current face, show reminder
    setFace(DeskyExpression::LOVE, 130); // caring face
    servoGesture("nudge");

    // The info panel will show "Drink Water!" overlay
    // It auto-clears after a few seconds via the face animation ending
  }

  // Clear water reminder after animation completes
  if (waterReminderActive && !faceAnim.isPlaying()) {
    waterReminderActive = false;
    // Restore work face
    setFace(DeskyExpression::CURIOUS, 100);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  IDLE ANIMATION (blink, look around)
// ═══════════════════════════════════════════════════════════════════════════
void handleIdleAnimation() {
  uint32_t now = millis();
  if (now - lastIdleAnimMs < IDLE_ANIM_INTERVAL)
    return;
  lastIdleAnimMs = now;

  // Random idle behavior
  uint8_t action = random(0, 6);
  switch (action) {
  case 0:
    setFace(DeskyExpression::WINK, 120);
    break;
  case 1:
    setFace(DeskyExpression::LOOK_LEFT, 150);
    break;
  case 2:
    setFace(DeskyExpression::LOOK_RIGHT, 150);
    break;
  case 3:
    setFace(DeskyExpression::NEUTRAL, 100);
    break;
  case 4:
    setFace(DeskyExpression::SLEEPY, 180);
    break;
  case 5:
    setFace(DeskyExpression::CURIOUS, 120);
    break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE CONTROL
// ═══════════════════════════════════════════════════════════════════════════
void setFace(DeskyExpression expr, uint16_t frameMs) {
  currentFace = expr;
  faceAnim.play(expr, millis(), frameMs);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SERVO GESTURES
// ═══════════════════════════════════════════════════════════════════════════
void servoAttach() {
  if (!servoAttached) {
    servo.attach(PIN_SERVO);
    servoAttached = true;
    delay(20);
  }
}

void servoScheduleDetach(uint32_t afterMs) {
  servoDetachAt = millis() + afterMs;
}

void handleServoTimeout() {
  if (servoAttached && millis() >= servoDetachAt && servoDetachAt != 0) {
    servo.detach();
    servoAttached = false;
    servoDetachAt = 0;
  }
}

void servoGesture(const char *type) {
  servoAttach();

  if (strcmp(type, "nod") == 0) {
    servoNod();
  } else if (strcmp(type, "wiggle") == 0) {
    servoWiggle();
  } else if (strcmp(type, "nudge") == 0) {
    servoNudge();
  }

  servoScheduleDetach(500); // detach 500ms after last move
}

void servoNod() {
  servo.write(90);
  delay(100);
  servo.write(60);
  delay(150);
  servo.write(90);
  delay(150);
  servo.write(60);
  delay(150);
  servo.write(90);
}

void servoWiggle() {
  servo.write(90);
  delay(80);
  for (int i = 0; i < 3; i++) {
    servo.write(70);
    delay(100);
    servo.write(110);
    delay(100);
  }
  servo.write(90);
}

void servoNudge() {
  // Gentle single push for water reminder
  servo.write(90);
  delay(100);
  servo.write(50);
  delay(300);
  servo.write(90);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MODE CONTROL
// ═══════════════════════════════════════════════════════════════════════════
void setMode(DeskyMode mode) {
  currentMode = mode;

  switch (mode) {
  case DeskyMode::IDLE:
    setFace(DeskyExpression::NEUTRAL, 100);
    sessionActive = false;
    break;
  case DeskyMode::WORK:
    setFace(DeskyExpression::CURIOUS, 100);
    break;
  case DeskyMode::BREAK_TIME:
    setFace(DeskyExpression::HAPPY, 120);
    break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  INFO PANEL DRAWING (OLED B)
// ═══════════════════════════════════════════════════════════════════════════
void drawInfoPanel() {
  oledInfo.clearBuffer();

  // ── Top: Mode badge ──
  drawModeBadge(0);

  // ── Middle: Clock ──
  drawClock(18);

  // ── Card setup prompt if cards not learned ──
  if (!workCardLearned || !breakCardLearned) {
    drawCardSetup();
    oledInfo.sendBuffer();
    return;
  }

  // ── Session timer or pot dial ──
  if (sessionActive) {
    drawSessionTimer(36);
  } else {
    drawPotBar(40);
  }

  // ── Water reminder overlay ──
  if (waterReminderActive) {
    // Draw a centered "Drink Water!" banner
    oledInfo.setFont(u8g2_font_helvB10_tr);
    oledInfo.setDrawColor(0);
    oledInfo.drawBox(10, 24, 108, 20);
    oledInfo.setDrawColor(1);
    oledInfo.drawFrame(10, 24, 108, 20);
    oledInfo.drawStr(16, 39, "\x07 Drink Water!");
  }

  oledInfo.sendBuffer();
}

void drawModeBadge(uint8_t y) {
  const char *label;
  switch (currentMode) {
  case DeskyMode::WORK:
    label = "[ WORK ]";
    break;
  case DeskyMode::BREAK_TIME:
    label = "[ BREAK ]";
    break;
  default:
    label = "~ Desky ~";
    break;
  }

  oledInfo.setFont(u8g2_font_helvB10_tr);
  // Center the text
  int w = oledInfo.getStrWidth(label);
  oledInfo.drawStr((128 - w) / 2, y + 11, label);

  // Divider line
  oledInfo.drawHLine(0, y + 14, 128);
}

void drawClock(uint8_t y) {
  String timeStr = getTimeStr();
  oledInfo.setFont(u8g2_font_helvB14_tr);
  int w = oledInfo.getStrWidth(timeStr.c_str());
  oledInfo.drawStr((128 - w) / 2, y + 15, timeStr.c_str());
}

void drawSessionTimer(uint8_t y) {
  uint32_t remainMs = 0;
  if (sessionElapsedMs < sessionDurationMs) {
    remainMs = sessionDurationMs - sessionElapsedMs;
  }
  uint32_t remainSec = remainMs / 1000;
  uint32_t mins = remainSec / 60;
  uint32_t secs = remainSec % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", mins, secs);

  oledInfo.setFont(u8g2_font_helvB12_tr);
  int w = oledInfo.getStrWidth(buf);
  oledInfo.drawStr((128 - w) / 2, y + 13, buf);

  // Progress bar
  uint8_t barY = y + 18;
  uint8_t barW = 100;
  uint8_t barX = (128 - barW) / 2;
  oledInfo.drawFrame(barX, barY, barW, 6);

  float progress = 0;
  if (sessionDurationMs > 0) {
    progress = (float)sessionElapsedMs / (float)sessionDurationMs;
  }
  progress = constrain(progress, 0.0f, 1.0f);
  uint8_t fillW = (uint8_t)(progress * (barW - 2));
  if (fillW > 0) {
    oledInfo.drawBox(barX + 1, barY + 1, fillW, 4);
  }
}

void drawPotBar(uint8_t y) {
  int mins = potToSessionMinutes();
  char buf[24];
  snprintf(buf, sizeof(buf), "Session: %d min", mins);

  oledInfo.setFont(u8g2_font_helvR08_tr);
  int w = oledInfo.getStrWidth(buf);
  oledInfo.drawStr((128 - w) / 2, y + 10, buf);

  // Small bar showing pot position
  uint8_t barY = y + 14;
  uint8_t barW = 80;
  uint8_t barX = (128 - barW) / 2;
  oledInfo.drawFrame(barX, barY, barW, 4);
  uint8_t fillW = map(potSmoothed, 0, 1023, 0, barW - 2);
  if (fillW > 0) {
    oledInfo.drawBox(barX + 1, barY + 1, fillW, 2);
  }
}

void drawSummary() {
  oledInfo.clearBuffer();

  oledInfo.setFont(u8g2_font_helvB10_tr);
  oledInfo.drawStr(20, 12, "Session Done!");
  oledInfo.drawHLine(0, 15, 128);

  oledInfo.setFont(u8g2_font_helvR08_tr);

  char buf[32];
  uint32_t workMins = totalWorkMs / 60000UL;
  uint32_t breakMins = totalBreakMs / 60000UL;

  snprintf(buf, sizeof(buf), "Total work:  %lu min", workMins);
  oledInfo.drawStr(8, 30, buf);

  snprintf(buf, sizeof(buf), "Total break: %lu min", breakMins);
  oledInfo.drawStr(8, 42, buf);

  snprintf(buf, sizeof(buf), "Sessions: %d", sessionsCompleted);
  oledInfo.drawStr(8, 54, buf);

  oledInfo.sendBuffer();
}

void drawCardSetup() {
  oledInfo.setFont(u8g2_font_helvR08_tr);
  if (!workCardLearned) {
    oledInfo.drawStr(8, 42, "Tap your WORK card");
    oledInfo.drawStr(8, 54, "on the reader...");
  } else if (!breakCardLearned) {
    oledInfo.drawStr(8, 42, "Now tap BREAK card");
    oledInfo.drawStr(8, 54, "on the reader...");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TIME UTILITIES
// ═══════════════════════════════════════════════════════════════════════════
String getTimeStr() {
  time_t now = time(nullptr);
  struct tm *tm = localtime(&now);

  if (now < 100000) {
    // Time not synced yet
    return String("--:--");
  }

  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min,
           tm->tm_sec);
  return String(buf);
}

// ═══════════════════════════════════════════════════════════════════════════
//  UID COMPARISON
// ═══════════════════════════════════════════════════════════════════════════
bool uidMatch(const uint8_t *a, uint8_t aLen, const uint8_t *b, uint8_t bLen) {
  if (aLen != bLen || aLen == 0)
    return false;
  return memcmp(a, b, aLen) == 0;
}
