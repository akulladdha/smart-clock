#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <time.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "secrets.h"
#include <WebServer.h>
#include "tapoAPI/tapo_device.h"

//!Test communication that works
/*
RPiUART rpi;

void setup() {
    Serial.begin(9600);
    rpi.begin();
    delayMicroseconds(10000000);
    Serial.println("[BOOT] ESP32 starting...");
    delayMicroseconds(1000000);
    Serial.println("[BOOT] Starting cycle...");
    // Step 1: Tell Pi we are ready
    rpi.send_ready();

    // Step 2: Block until Pi sends a strategy
    Strategy strategy = rpi.recv_strategy();
    switch (strategy) {
        case STRAT_GENTLE:  Serial.println("[MAIN] Strategy: GENTLE");  break;
        case STRAT_NORMAL:  Serial.println("[MAIN] Strategy: NORMAL");  break;
        case STRAT_NUCLEAR: Serial.println("[MAIN] Strategy: NUCLEAR"); break;
        default:            Serial.println("[MAIN] Strategy: UNKNOWN"); break;
    }
    // Step 3: Build dummy wake report
    WakeOutcome outcome = {
        .woke             = true,
        .snooze_count     = 1,
        .response_time_s  = 300,   // 5 minutes in seconds
        .safety_override  = false
    };

    // Step 4: Send wake report back to Pi
    rpi.send_wake_report(outcome);

    // Step 5: Block until Pi ACKs
    rpi.wait_for_ack();

    Serial.println("[MAIN] Cycle complete.");
}
*/
// ---------------------------------------------------------------------------
// Hardware pins
// ---------------------------------------------------------------------------
#define OLED_SDA        21
#define OLED_SCL        22
#define PASSIVE_BUZZER  25   // PWM tone
#define ACTIVE_BUZZER   26   // digital HIGH/LOW
// Note: GPIO 34 & 35 are input-only on ESP32; no internal pull-up hardware.
// Use external 10k pull-up resistors to 3.3V on these pins.
#define SNOOZE_BTN      18
#define CONFIRM_BTN     19
#define SLIDEPOT_PIN    32   // ADC1 channel 4, 12-bit (0-4095)
#define TAPO_BTN 4

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
#define SCREEN_W 128
#define SCREEN_H  64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------------------------------------------------------------------
// WiFi / NTP
// ---------------------------------------------------------------------------
const char* WIFI_SSID      = "akulphone";
const char* WIFI_PASS      = "hillbilly";
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -21600;  // CST (UTC-6) — adjust as needed
const int   DST_OFFSET_SEC =   3600;

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum State { HOME, SET_HOUR, SET_MINUTE, DEMO, ALARM_RINGING, SNOOZED };
State state = HOME;

// ---------------------------------------------------------------------------
// Current time (updated from NTP each loop)
// ---------------------------------------------------------------------------
int  curHour = 0, curMinute = 0, curSecond = 0;
bool timeValid = false;

// ---------------------------------------------------------------------------
// Alarm settings
// ---------------------------------------------------------------------------
int alarmHour   = 7;
int alarmMinute = 30;

// Temporary values while navigating SET_HOUR / SET_MINUTE screens
int tempHour   = 0;
int tempMinute = 0;

// ---------------------------------------------------------------------------
// Button debounce
// ---------------------------------------------------------------------------
const unsigned long DEBOUNCE_MS = 50;

WebServer server(80);

TapoDevice bulb;
bool bulbOn = false;
bool bulbReady = false;

bool alarmActive = false;
int  snoozeCount = 0;

struct Button {
  int           pin;
  bool          lastRaw;        // last sampled GPIO level
  bool          stable;         // debounced level
  unsigned long lastChangeMs;   // millis() when lastRaw last changed
  bool          pressed;        // true for exactly one loop cycle on falling edge
};

Button snoozeBtn  = { SNOOZE_BTN,  HIGH, HIGH, 0, false };
Button confirmBtn = { CONFIRM_BTN, HIGH, HIGH, 0, false };
Button tapoBtn    = { TAPO_BTN,    HIGH, HIGH, 0, false };

// ---------------------------------------------------------------------------
// Sunrise state
// ---------------------------------------------------------------------------
bool          sunriseActive   = false;
unsigned long sunriseStartMs  = 0;
const unsigned long SUNRISE_DURATION_MS = 30000; // 30 seconds

// ---------------------------------------------------------------------------
// Strategy tracking
// ---------------------------------------------------------------------------
enum Strategy { STRAT_GENTLE, STRAT_NORMAL, STRAT_NUCLEAR };
Strategy currentStrategy = STRAT_NORMAL;
bool sunriseFiredThisAlarm = false; // prevents sunrise replaying after snooze

// Strobe state (non-blocking, for NORMAL and NUCLEAR)
bool          strobeOn        = false;
unsigned long strobeToggleMs  = 0;
const unsigned long STROBE_INTERVAL_MS = 2500; // Tapo bulb needs ~2-3s between commands
int           strobeBrightness = 50; // 50 for NORMAL, 100 for NUCLEAR

// Demo modality state
bool          demoModalityActive   = false;
Strategy      demoModalityStrategy = STRAT_NORMAL;
bool          demoSunriseDone      = false; // tracks if demo sunrise finished

// Nuclear call tracking
bool nuclearCallMade = false;

// ---------------------------------------------------------------------------
// Sunrise tick and start
// ---------------------------------------------------------------------------
void tickSunrise() {
  if (!sunriseActive || !bulbReady) return;

  unsigned long elapsed = millis() - sunriseStartMs;
  if (elapsed >= SUNRISE_DURATION_MS) {
    bulb.set_color_temperature(5500);
    bulb.set_brightness(100);
    sunriseActive = false;
    Serial.println("Sunrise complete");
    return;
  }

  static unsigned long lastUpdateMs = 0;
  if (millis() - lastUpdateMs < 2000) return;
  lastUpdateMs = millis();

  float p = (float)elapsed / SUNRISE_DURATION_MS; // 0.0 → 1.0

  // Brightness: 5 → 100
  uint8_t brightness = (uint8_t)(5 + p * 95);

  // Color phases across 0.0 → 1.0:
  // 0.00 - 0.20 : purple    (hue ~280, sat 80)
  // 0.20 - 0.40 : pink      (hue ~320, sat 90)
  // 0.40 - 0.55 : red       (hue ~0,   sat 100)
  // 0.55 - 0.70 : orange    (hue ~25,  sat 100)
  // 0.70 - 0.85 : yellow    (hue ~50,  sat 90)
  // 0.85 - 1.00 : white     (color temp mode)

  Serial.printf("Sunrise %.0f%% brightness:%d\n", p * 100, brightness);

  bulb.set_brightness(brightness);

  if (p < 0.20f) {
    // Purple
    float sub = p / 0.20f;
    uint16_t hue = (uint16_t)(280 + sub * 10);  // 280 → 290
    uint8_t  sat = (uint8_t)(80 + sub * 5);      // 80  → 85
    bulb.set_color(hue, sat);

  } else if (p < 0.40f) {
    // Purple → Pink
    float sub = (p - 0.20f) / 0.20f;
    uint16_t hue = (uint16_t)(290 + sub * 40);  // 290 → 330
    uint8_t  sat = (uint8_t)(85 + sub * 10);     // 85  → 95
    bulb.set_color(hue, sat);

  } else if (p < 0.55f) {
    // Pink → Red
    float sub = (p - 0.40f) / 0.15f;
    uint16_t hue = (uint16_t)(330 + sub * 30);  // 330 → 360
    if (hue >= 360) hue = 0;
    uint8_t sat = 100;
    bulb.set_color(hue, sat);

  } else if (p < 0.70f) {
    // Red → Orange
    float sub = (p - 0.55f) / 0.15f;
    uint16_t hue = (uint16_t)(sub * 25);         // 0 → 25
    bulb.set_color(hue, 100);

  } else if (p < 0.85f) {
    // Orange → Yellow
    float sub = (p - 0.70f) / 0.15f;
    uint16_t hue = (uint16_t)(25 + sub * 30);   // 25 → 55
    uint8_t  sat = (uint8_t)(100 - sub * 15);    // 100 → 85
    bulb.set_color(hue, sat);

  } else {
    // Yellow → Bright White (switch to color temp mode)
    float sub = (p - 0.85f) / 0.15f;
    uint16_t kelvin = (uint16_t)(3000 + sub * 2500); // 3000 → 5500
    bulb.set_color_temperature(kelvin);
  }
}

void startSunrise() {
  if (!bulbReady) return;
  sunriseActive  = true;
  sunriseStartMs = millis();
  bulb.on();
  bulb.set_brightness(5);
  bulb.set_color(280, 80);  // start: deep purple, low brightness
  Serial.println("Sunrise started");
}

// ---------------------------------------------------------------------------
// Twilio wake-up call
// ---------------------------------------------------------------------------
void makeWakeUpCall() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = String("https://api.twilio.com/2010-04-01/Accounts/")
                 + TWILIO_ACCOUNT_SID + "/Calls.json";

    http.begin(client, url);
    http.setAuthorization(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "To=%2B14694066614"
    "&From=%2B17325270427"
    "&Url=http://demo.twilio.com/docs/voice.xml";

    int code = http.POST(body);
    Serial.printf("Twilio response: %d\n", code);
    Serial.printf("Twilio message: %d\n", http.getString());
    http.end();
}

// ---------------------------------------------------------------------------
// Button debounce helper
// ---------------------------------------------------------------------------
void updateButton(Button& btn) {
  btn.pressed = false;
  bool raw = digitalRead(btn.pin);
  unsigned long now = millis();
  if (raw != btn.lastRaw) {
    btn.lastRaw      = raw;
    btn.lastChangeMs = now;
  }
  if ((now - btn.lastChangeMs) >= DEBOUNCE_MS) {
    bool newStable = btn.lastRaw;
    if (newStable == LOW && btn.stable == HIGH) btn.pressed = true;
    btn.stable = newStable;
  }
}

// ---------------------------------------------------------------------------
// Passive buzzer (LEDC channel 0)
// ---------------------------------------------------------------------------
#define LEDC_CH   0
#define LEDC_RES  8

// Beep pattern state (non-blocking)
bool          beepOn       = false;
unsigned long beepToggleMs = 0;
const unsigned long BEEP_ON_MS  = 400;
const unsigned long BEEP_OFF_MS = 200;
const int           BEEP_FREQ   = 1000;

void startTone(int freq) {
  ledcAttachPin(PASSIVE_BUZZER, LEDC_CH);
  ledcWriteTone(LEDC_CH, freq);
}

void stopTone() {
  ledcWriteTone(LEDC_CH, 0);
}

void stopActiveBuzzer() {
  digitalWrite(ACTIVE_BUZZER, LOW);
}

void stopAllBuzzers() {
  stopTone();
  stopActiveBuzzer();
  beepOn = false;
}

// ---------------------------------------------------------------------------
// Nokia ringtone (non-blocking)
// ---------------------------------------------------------------------------
void tickNokiaRingtone() {
  static const int notes[]  = {659, 587, 370, 415, 554, 494, 294, 330,
                                523, 466, 277, 330, 440, 392, 262};
  static const int NUM_NOTES = 15;
  static int           idx        = 0;
  static unsigned long lastNoteMs = 0;
  static bool          started    = false;

  unsigned long now = millis();

  if (!started) {
    started    = true;
    lastNoteMs = now;
    ledcWriteTone(LEDC_CH, notes[0]);
    return;
  }

  unsigned long elapsed = now - lastNoteMs;

  if (elapsed >= 200) {       // 150 ms note + 50 ms gap cycle complete
    idx = (idx + 1) % NUM_NOTES;
    ledcWriteTone(LEDC_CH, notes[idx]);
    lastNoteMs = now;
  } else if (elapsed >= 150) { // in the 50 ms gap
    ledcWriteTone(LEDC_CH, 0);
  }
  // 0–149 ms: note is already playing, nothing to do
}

// ---------------------------------------------------------------------------
// Strobe (non-blocking)
// ---------------------------------------------------------------------------
void tickStrobe() {
  if (!bulbReady) return;
  unsigned long now = millis();
  if (now - strobeToggleMs < STROBE_INTERVAL_MS) return;
  strobeToggleMs = now;
  strobeOn = !strobeOn;
  if (strobeOn) {
    bulb.set_brightness(strobeBrightness);
    bulb.on();
  } else {
    bulb.off();
  }
}

void stopStrobe() {
  strobeOn = false;
  if (bulbReady) bulb.off();
}

// ---------------------------------------------------------------------------
// Combined output stop
// ---------------------------------------------------------------------------
void stopAllOutputs() {
  stopAllBuzzers();
  stopStrobe();
}

// ---------------------------------------------------------------------------
// Beep pattern tick (passive buzzer)
// ---------------------------------------------------------------------------
void tickBeepPattern() {
  unsigned long now = millis();
  if (beepOn) {
    if (now - beepToggleMs >= BEEP_ON_MS) {
      stopTone();
      beepOn       = false;
      beepToggleMs = now;
    }
  } else {
    if (now - beepToggleMs >= BEEP_OFF_MS) {
      startTone(BEEP_FREQ);
      beepOn       = true;
      beepToggleMs = now;
    }
  }
}

// ---------------------------------------------------------------------------
// Slidepot helpers
// ---------------------------------------------------------------------------
#define POT_SAMPLES 8
int potBuffer[POT_SAMPLES] = {0};
int potIndex = 0;

int slidepotRaw() {
  potBuffer[potIndex] = analogRead(SLIDEPOT_PIN);
  potIndex = (potIndex + 1) % POT_SAMPLES;
  int sum = 0;
  for (int i = 0; i < POT_SAMPLES; i++) sum += potBuffer[i];
  return sum / POT_SAMPLES;
}

int slidepotMap(int lo, int hi) {
  return map(slidepotRaw(), 0, 4095, lo, hi);
}

int slidepotThird() {
  int raw = slidepotRaw();
  if (raw < 1365) return 0;
  if (raw < 2730) return 1;
  return 2;
}

// ---------------------------------------------------------------------------
// Alarm escalation
// ---------------------------------------------------------------------------
unsigned long alarmStartMs = 0;
int           escMode      = 1;   // 1 = ringtone/gentle, 2 = buzzer/normal/nuclear

const unsigned long ESC_2_MS = 2UL * 60 * 1000;   // kept for OLED progress bar
const unsigned long ESC_3_MS = 4UL * 60 * 1000;   // kept for OLED progress bar

// Strategy-aware alarm escalation — no cross-mode escalation, strategies are fixed at fire time
void tickAlarmEscalation() {
  switch (currentStrategy) {
    case STRAT_GENTLE:
      escMode = 1;
      tickNokiaRingtone();
      break;

    case STRAT_NORMAL:
      escMode = 2;
      digitalWrite(ACTIVE_BUZZER, HIGH);
      strobeBrightness = 50;
      tickStrobe();
      break;

    case STRAT_NUCLEAR:
      escMode = 2;
      digitalWrite(ACTIVE_BUZZER, HIGH);
      strobeBrightness = 100;
      tickStrobe();
      if (!nuclearCallMade) {
        nuclearCallMade = true;
        makeWakeUpCall();
        Serial.println("Nuclear: calling via Twilio");
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Snooze helper
// ---------------------------------------------------------------------------
unsigned long snoozeShowMs = 0;

void doSnooze() {
  stopAllOutputs();
  snoozeCount++;
  alarmMinute += 1;
  if (alarmMinute >= 60) { alarmMinute -= 60; alarmHour = (alarmHour + 1) % 24; }
  state        = SNOOZED;
  snoozeShowMs = millis();
  // sunriseFiredThisAlarm intentionally NOT reset here — snooze re-ring
  // should not replay sunrise
}

// ---------------------------------------------------------------------------
// Demo mode (legacy OLED demo, separate from web demo modalities)
// ---------------------------------------------------------------------------
int  demoSel        = 0;      // 0,1,2 → modes 1,2,3
bool demoActive     = false;
int  demoActiveMode = 0;

void stopDemo() {
  stopAllBuzzers();
  demoActive     = false;
  demoActiveMode = 0;
}

// ---------------------------------------------------------------------------
// OLED render
// ---------------------------------------------------------------------------
void renderDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  switch (state) {

    case HOME: {
      int dispHour = curHour % 12;
      if (dispHour == 0) dispHour = 12;
      const char* ampm = (curHour < 12) ? "AM" : "PM";

      display.setTextSize(3);
      char timeBuf[6];
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", dispHour, curMinute);
      int timeX = (128 - (5 * 18)) / 2;
      display.setCursor(timeX, 4);
      display.print(timeBuf);

      display.setTextSize(1);
      char secBuf[4];
      snprintf(secBuf, sizeof(secBuf), ":%02d", curSecond);
      display.setCursor(110, 12);
      display.print(secBuf);

      display.setTextSize(1);
      display.setCursor(114, 4);
      display.print(ampm);

      display.drawFastHLine(0, 40, 128, SSD1306_WHITE);

      display.setTextSize(1);
      display.setCursor(0, 45);
      int aDisp = alarmHour % 12;
      if (aDisp == 0) aDisp = 12;
      display.printf("Alarm: %02d:%02d %s", aDisp, alarmMinute, alarmHour < 12 ? "AM" : "PM");

      display.setCursor(0, 55);
      display.print("CNF=set  SNZ=demo");
      break;
    }

    case SET_HOUR: {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("-- SET ALARM --");
      display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

      display.setCursor(0, 16);
      display.print("HOUR");

      // AM/PM indicator
      display.setCursor(90, 16);
      display.print(tempHour < 12 ? "AM" : "PM");

      display.setTextSize(4);
      char buf[3];
      snprintf(buf, sizeof(buf), "%02d", tempHour==12 ? 12 : tempHour%12);

      display.setCursor(44, 22);
      display.print(buf);

      display.setTextSize(1);
      display.setCursor(0, 56);
      display.print("Slide  |  CNF to next");
      break;
    }

    case SET_MINUTE: {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("-- SET ALARM --");
      display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

      display.setCursor(0, 16);
      display.print("MINUTE");

      display.setTextSize(4);
      char buf[3];
      snprintf(buf, sizeof(buf), "%02d", tempMinute);
      display.setCursor(44, 22);
      display.print(buf);

      display.setTextSize(1);
      display.setCursor(0, 56);
      display.print("Slide  |  CNF to save");
      break;
    }

    case DEMO: {
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("-- DEMO MODE --");
      display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

      const char* labels[3] = { "1: Ringtone", "2: Buzzer", "3: Calling..." };
      int yPos[3] = { 16, 30, 44 };

      for (int i = 0; i < 3; i++) {
        if (i == demoSel) {
          display.fillRect(0, yPos[i] - 1, 128, 11, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
        } else {
          display.setTextColor(SSD1306_WHITE);
        }
        display.setCursor(4, yPos[i]);
        display.print(labels[i]);
      }

      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 56);
      display.print("CNF=run  SNZ=back");
      break;
    }

    case ALARM_RINGING: {
      if ((millis() / 500) % 2 == 0) {
        display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setTextSize(1);
      display.setCursor(34, 3);
      display.print("!! ALARM !!");
      display.setTextColor(SSD1306_WHITE);

      display.drawFastHLine(0, 14, 128, SSD1306_WHITE);

      display.setTextSize(2);
      display.setCursor(0, 18);
      if      (escMode == 1) display.print("Ringtone");
      else if (escMode == 2) display.print("Buzzer");
      else                   display.print("Calling..");

      unsigned long elapsed = millis() - alarmStartMs;
      int progress = (int)((float)elapsed / ESC_3_MS * 128);
      if (progress > 128) progress = 128;
      display.drawRect(0, 38, 128, 6, SSD1306_WHITE);
      display.fillRect(0, 38, progress, 6, SSD1306_WHITE);

      display.setTextSize(1);
      display.setCursor(0, 48);
      int aHour = curHour % 12;
      if (aHour == 0) aHour = 12;
      display.printf("%02d:%02d %s", aHour, curMinute, curHour < 12 ? "AM" : "PM");
      display.setCursor(50, 48);
      display.print("SNZ+1m CNF=off");
      break;
    }

    case SNOOZED: {
      display.drawRoundRect(10, 15, 108, 34, 4, SSD1306_WHITE);
      display.setTextSize(2);
      display.setCursor(18, 20);
      display.print("Snoozed");
      display.setTextSize(1);
      display.setCursor(38, 44);
      display.print("+1 min");
      break;
    }
  }

  display.display();
}

// ---------------------------------------------------------------------------
// NTP sync
// ---------------------------------------------------------------------------
void syncNTP() {
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  struct tm t;
  int retries = 0;
  while (!getLocalTime(&t) && retries < 20) {
    delay(500);
    retries++;
  }
  timeValid = retries < 20;
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------
void handleButtons() {
  bool snz = snoozeBtn.pressed;
  bool cnf = confirmBtn.pressed;

  switch (state) {

    case HOME:
      if (cnf) {
        tempHour = alarmHour;
        state    = SET_HOUR;
      } else if (snz) {
        demoSel    = 0;
        demoActive = false;
        stopAllBuzzers();
        state = DEMO;
      }
      break;

    case SET_HOUR:
      if (cnf) {
        state = SET_MINUTE;
        tempMinute = alarmMinute;
      }
      break;

    case SET_MINUTE:
      if (cnf) {
        alarmHour   = tempHour;
        alarmMinute = tempMinute;
        state       = HOME;
      }
      break;

    case DEMO:
      if (snz) {
        stopDemo();
        state = HOME;
      } else if (cnf) {
        stopDemo();
        demoActiveMode = demoSel + 1;
        demoActive     = true;
        if (demoActiveMode == 1) {
          beepOn       = false;
          beepToggleMs = 0;
        } else if (demoActiveMode == 2) {
          digitalWrite(ACTIVE_BUZZER, HIGH);
        } else if (demoActiveMode == 3) {
          makeWakeUpCall();
        }
      }
      break;

    case ALARM_RINGING:
      if (snz) {
        doSnooze();
      } else if (cnf) {
        stopAllOutputs();
        alarmActive          = false;
        snoozeCount          = 0;
        demoModalityActive   = false;
        demoSunriseDone      = false;
        sunriseFiredThisAlarm = false;
        nuclearCallMade      = false;
        sunriseActive        = false;
        state                = HOME;
      }
      break;

    case SNOOZED:
      break;
  }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[220];
  snprintf(json, sizeof(json),
    "{\"hour\":%d,\"minute\":%d,\"alarm_active\":%s,\"esc_mode\":%d,\"snooze_count\":%d,\"sunrise_active\":%s,\"strategy\":%d,\"demo_active\":%s}",
    alarmHour, alarmMinute,
    (state == ALARM_RINGING) ? "true" : "false",
    escMode, snoozeCount,
    sunriseActive ? "true" : "false",
    (int)currentStrategy,
    demoModalityActive ? "true" : "false");
  server.send(200, "application/json", json);
}

void handleSet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("hour") || !server.hasArg("minute")) {
    server.send(400, "text/plain", "Missing params"); return;
  }
  alarmHour   = server.arg("hour").toInt();
  alarmMinute = server.arg("minute").toInt();
  server.send(200, "text/plain", "OK");
}

void handleDismiss() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  stopAllOutputs();
  alarmActive          = false;
  snoozeCount          = 0;
  demoModalityActive   = false;
  demoSunriseDone      = false;
  sunriseFiredThisAlarm = false;
  nuclearCallMade      = false;
  sunriseActive        = false;
  state                = HOME;
  server.send(200, "text/plain", "OK");
}

void handleTrigger() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  alarmStartMs = millis();
  escMode      = 1;
  beepOn       = false;
  beepToggleMs = 0;
  alarmActive  = true;
  state        = ALARM_RINGING;
  server.send(200, "text/plain", "OK");
}

void handleSnooze() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  doSnooze();
  server.send(200, "text/plain", "OK");
}

void handleEscalate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "Missing mode"); return;
  }
  int mode = server.arg("mode").toInt();
  if (mode < 1 || mode > 3) {
    server.send(400, "text/plain", "Mode must be 1-3"); return;
  }
  stopAllBuzzers();
  escMode      = mode;
  alarmStartMs = millis() - (mode == 2 ? ESC_2_MS : mode == 3 ? ESC_3_MS : 0);
  alarmActive  = true;
  state        = ALARM_RINGING;
  if (mode == 3) makeWakeUpCall();
  server.send(200, "text/plain", "OK");
}

void handleSunrise() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  startSunrise();
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Demo modality handlers (web-triggered, no RPi UART)
// ---------------------------------------------------------------------------
void handleDemoModality(Strategy strat) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  stopAllOutputs();
  demoModalityActive   = true;
  demoModalityStrategy = strat;
  demoSunriseDone      = false;
  sunriseActive        = false;
  startSunrise(); // sunrise always plays first in demo
  alarmActive          = true;
  alarmStartMs         = millis() + SUNRISE_DURATION_MS; // placeholder; overwritten after sunrise
  escMode              = 1;
  beepOn               = false;
  beepToggleMs         = 0;
  nuclearCallMade      = false;
  strobeOn             = false;
  strobeToggleMs       = 0;
  strobeBrightness     = (strat == STRAT_NUCLEAR) ? 100 : 50;
  // Do NOT set state = ALARM_RINGING yet — wait for sunrise to finish in loop()
  // TODO: RPi UART integration (natural alarm path only):
  // rpi.send_ready();
  // Strategy s = rpi.recv_strategy();
  // currentStrategy = s;
  server.send(200, "text/plain", "OK");
}

void handleDemoGentle()  { handleDemoModality(STRAT_GENTLE);  }
void handleDemoNormal()  { handleDemoModality(STRAT_NORMAL);  }
void handleDemoNuclear() { handleDemoModality(STRAT_NUCLEAR); }

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  Serial.println(WiFi.macAddress());

  // Hardware init
  pinMode(ACTIVE_BUZZER, OUTPUT);
  digitalWrite(ACTIVE_BUZZER, LOW);
  pinMode(SNOOZE_BTN,  INPUT_PULLUP);
  pinMode(CONFIRM_BTN, INPUT_PULLUP);
  // GPIO 34/35 are input-only; INPUT_PULLUP is accepted but has no effect —
  // use external 10k resistors pulled to 3.3V.

  ledcSetup(LEDC_CH, BEEP_FREQ, LEDC_RES);
  ledcAttachPin(PASSIVE_BUZZER, LEDC_CH);
  ledcWriteTone(LEDC_CH, 0);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
    while (1) delay(1000);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print("Connecting to WiFi...");
  display.display();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("WiFi setup:");
  display.println("Connect to AP:");
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.println("RiseIQ");
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.println("-> 192.168.4.1");
  display.display();

  if (!wm.autoConnect("RiseIQ")) {
    Serial.println("WiFi failed");
    display.clearDisplay();
    display.setCursor(0, 28);
    display.print("WiFi failed.");
    display.display();
    delay(1500);
  } else {
    display.clearDisplay();
    display.setCursor(0, 20);
    display.print("WiFi OK");
    display.display();
    Serial.println(WiFi.macAddress());
    delay(1000);
    syncNTP();

    server.on("/status",       HTTP_GET, handleStatus);
    server.on("/set",          HTTP_GET, handleSet);
    server.on("/dismiss",      HTTP_GET, handleDismiss);
    server.on("/trigger",      HTTP_GET, handleTrigger);
    server.on("/snooze",       HTTP_GET, handleSnooze);
    server.on("/escalate",     HTTP_GET, handleEscalate);
    server.on("/sunrise",      HTTP_GET, handleSunrise);
    server.on("/demo/gentle",  HTTP_GET, handleDemoGentle);
    server.on("/demo/normal",  HTTP_GET, handleDemoNormal);
    server.on("/demo/nuclear", HTTP_GET, handleDemoNuclear);
    server.begin();

    pinMode(TAPO_BTN, INPUT_PULLUP);
    bulbReady = bulb.begin("10.159.67.114", "akul.laddha@gmail.com", "water1234");
    Serial.println(bulbReady ? "Bulb connected" : "Bulb failed");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("IP:");
    display.setCursor(0, 36);
    display.print(WiFi.localIP().toString());
    display.display();
    delay(3000);
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();

  // ---- Update current time --------------------------------------------------
  struct tm t;
  if (getLocalTime(&t)) {
    curHour   = t.tm_hour;
    curMinute = t.tm_min;
    curSecond = t.tm_sec;
    timeValid = true;
  }

  tickSunrise();

  // ---- When demo sunrise finishes, start the alarm sequence ----------------
  if (demoModalityActive && !sunriseActive && !demoSunriseDone) {
    demoSunriseDone  = true;
    currentStrategy  = demoModalityStrategy;
    alarmStartMs     = millis();
    state            = ALARM_RINGING;
    alarmActive      = true;
  }

  // ---- Read buttons ---------------------------------------------------------
  updateButton(snoozeBtn);
  updateButton(confirmBtn);
  updateButton(tapoBtn);
  if (tapoBtn.pressed && bulbReady) {
    bulbOn = !bulbOn;
    if (bulbOn) bulb.on();
    else        bulb.off();
  }

  // ---- Continuous slidepot reads for SET screens ----------------------------
  if (state == SET_HOUR)   tempHour   = slidepotMap(0, 23);
  if (state == SET_MINUTE) tempMinute = slidepotMap(0, 59);
  if (state == DEMO)       demoSel    = slidepotThird();

  // ---- Button handling ------------------------------------------------------
  handleButtons();

  // ---- Natural alarm: fire sunrise 30 s before alarm time ------------------
  if (state == HOME && timeValid && !sunriseFiredThisAlarm) {
    int alarmTotalSec    = alarmHour * 3600 + alarmMinute * 60;
    int sunriseTriggerSec = alarmTotalSec - 30;
    if (sunriseTriggerSec < 0) sunriseTriggerSec += 86400; // handle midnight rollover
    int curTotalSec = curHour * 3600 + curMinute * 60 + curSecond;

    if (curTotalSec == sunriseTriggerSec) {
      sunriseFiredThisAlarm = true;
      startSunrise();
    }
  }

  // ---- Natural alarm: fire at alarm time ------------------------------------
  if (state == HOME && timeValid &&
      curHour == alarmHour && curMinute == alarmMinute && curSecond == 0) {
    // TODO: uncomment RPi UART when hardware connected:
    // rpi.send_ready();
    // Strategy s = rpi.recv_strategy();
    // currentStrategy = s;
    currentStrategy  = STRAT_NORMAL; // stub: default strategy until RPi connected
    alarmStartMs     = millis();
    escMode          = 1;
    beepOn           = false;
    beepToggleMs     = 0;
    alarmActive      = true;
    nuclearCallMade  = false;
    strobeOn         = false;
    strobeToggleMs   = 0;
    strobeBrightness = (currentStrategy == STRAT_NUCLEAR) ? 100 : 50;
    state            = ALARM_RINGING;
  }

  // ---- Alarm escalation tick ------------------------------------------------
  if (state == ALARM_RINGING) {
    tickAlarmEscalation();
    if (currentStrategy == STRAT_NORMAL || currentStrategy == STRAT_NUCLEAR) {
      tickStrobe();
    }
  }

  // ---- Demo buzzer tick (legacy OLED demo) ----------------------------------
  if (state == DEMO && demoActive) {
    if (demoActiveMode == 1) tickBeepPattern();
    // mode 2: active buzzer already driven HIGH in handleButtons
    // mode 3: display only
  }

  // ---- Snoozed auto-return after 2 s ----------------------------------------
  if (state == SNOOZED && millis() - snoozeShowMs >= 2000) {
    state = HOME;
  }

  // ---- OLED render ----------------------------------------------------------
  renderDisplay();

  delay(30);  // ~33 fps; keeps display smooth without burning CPU
}
