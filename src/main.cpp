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



void makeWakeUpCall() {
    WiFiClientSecure client;
    client.setInsecure();
  
    HTTPClient http;
    String url = String("https://api.twilio.com/2010-04-01/Accounts/")
                 + TWILIO_ACCOUNT_SID + "/Calls.json";
  
    http.begin(client, url);
    http.setAuthorization(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
    String body = "To=%2B" + String(CALL_TO).substring(1)
            + "&From=%2B" + String(TWILIO_FROM).substring(1)
            + "&Url=http://demo.twilio.com/docs/voice.xml";
  
    int code = http.POST(body);
    Serial.printf("Twilio response: %d\n", code);
    http.end();
  }

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

// Call every loop while passive buzzer should be beeping
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
int           escMode      = 1;   // 1 = tone, 2 = active buzzer, 3 = calling

const unsigned long ESC_2_MS = 2UL * 60 * 1000;   // 2 min → mode 2
const unsigned long ESC_3_MS = 4UL * 60 * 1000;   // 4 min → mode 3

// Call every loop while state == ALARM_RINGING
void tickAlarmEscalation() {
  unsigned long elapsed = millis() - alarmStartMs;
  int newMode = 1;
  if      (elapsed >= ESC_3_MS) newMode = 3;
  else if (elapsed >= ESC_2_MS) newMode = 2;

  if (newMode != escMode) {
    stopAllBuzzers();
    escMode = newMode;
    if (escMode == 3) {
        makeWakeUpCall();  // add this line
        Serial.println("calling using twilio");
    }

  }

  if      (escMode == 1) tickBeepPattern();
  else if (escMode == 2) digitalWrite(ACTIVE_BUZZER, HIGH);

  // escMode == 3: no buzzer; OLED shows "Calling..."
}

// ---------------------------------------------------------------------------
// Snooze helper
// ---------------------------------------------------------------------------
unsigned long snoozeShowMs = 0;

void doSnooze() {
  stopAllBuzzers();
  snoozeCount++;
  alarmMinute += 5;
  if (alarmMinute >= 60) { alarmMinute -= 60; alarmHour = (alarmHour + 1) % 24; }
  state        = SNOOZED;
  snoozeShowMs = millis();
}

// ---------------------------------------------------------------------------
// Demo mode
// ---------------------------------------------------------------------------
int  demoSel           = 0;      // 0,1,2 → modes 1,2,3
bool demoActive        = false;
int  demoActiveMode    = 0;

void stopDemo() {
  stopAllBuzzers();
  demoActive     = false;
  demoActiveMode = 0;
}

void renderDisplay() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
  
    switch (state) {
  
      // ---- HOME ---------------------------------------------------------------
      case HOME: {
        // Time centered, large
        display.setTextSize(3);
        char timeBuf[6];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", curHour, curMinute);
        int timeX = (128 - (5 * 18)) / 2;  // center 5 chars at size 3 (18px each)
        display.setCursor(timeX, 4);
        display.print(timeBuf);
  
        // Seconds on its own line, smaller, right-aligned
        display.setTextSize(1);
        char secBuf[4];
        snprintf(secBuf, sizeof(secBuf), ":%02d", curSecond);
        display.setCursor(104, 30);
        display.print(secBuf);
  
        // Divider
        display.drawFastHLine(0, 40, 128, SSD1306_WHITE);
  
        // Alarm line
        display.setTextSize(1);
        display.setCursor(0, 45);
        display.printf("Alarm: %02d:%02d", alarmHour, alarmMinute);
  
        // Hints right-aligned
        display.setCursor(72, 45);
        display.print(" SET");
        display.setCursor(66, 55);
        //display.print("DEMO");
        display.setCursor(72, 45);
  
        // Two hint labels bottom right
        display.setCursor(0, 55);
        display.print("CNF=set  SNZ=demo");
        break;
      }
  
      // ---- SET ALARM HOUR -----------------------------------------------------
      case SET_HOUR: {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("-- SET ALARM --");
        display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  
        display.setCursor(0, 16);
        display.print("HOUR");
  
        display.setTextSize(4);
        char buf[3];
        snprintf(buf, sizeof(buf), "%02d", tempHour);
        display.setCursor(44, 22);
        display.print(buf);
  
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.print("Slide  |  CNF to next");
        break;
      }
  
      // ---- SET ALARM MINUTE ---------------------------------------------------
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
  
      // ---- DEMO ---------------------------------------------------------------
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
  
      // ---- ALARM RINGING ------------------------------------------------------
      case ALARM_RINGING: {
        // Flashing header
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
  
        // Escalation progress bar
        unsigned long elapsed = millis() - alarmStartMs;
        int progress = (int)((float)elapsed / ESC_3_MS * 128);
        if (progress > 128) progress = 128;
        display.drawRect(0, 38, 128, 6, SSD1306_WHITE);
        display.fillRect(0, 38, progress, 6, SSD1306_WHITE);
  
        display.setTextSize(1);
        display.setCursor(0, 48);
        display.printf("%02d:%02d", curHour, curMinute);
        display.setCursor(50, 48);
        display.print("SNZ+5m CNF=off");
        break;
      }
  
      // ---- SNOOZED ------------------------------------------------------------
      case SNOOZED: {
        display.drawRoundRect(10, 15, 108, 34, 4, SSD1306_WHITE);
        display.setTextSize(2);
        display.setCursor(18, 20);
        display.print("Snoozed");
        display.setTextSize(1);
        display.setCursor(38, 44);
        display.print("+5 min");
        break;
      }
    }
  
    display.display();
  }

// ---------------------------------------------------------------------------
// NTP sync — called once on boot and can be recalled on reconnect
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
// Button handling — returns early once action taken this cycle
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
      // Slidepot scrubs 0-23 continuously (read in loop)
      if (cnf) {
        state = SET_MINUTE;
        tempMinute = alarmMinute;
      }
      break;

    case SET_MINUTE:
      // Slidepot scrubs 0-59 continuously (read in loop)
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
        demoActiveMode = demoSel + 1;  // 1,2,3
        demoActive     = true;
        if (demoActiveMode == 1) {
          beepOn       = false;
          beepToggleMs = 0;
        } else if (demoActiveMode == 2) {
          digitalWrite(ACTIVE_BUZZER, HIGH);
        }
        else if(demoActiveMode==3){
          makeWakeUpCall();
        }
        // mode 3: just shows "Calling..." text (handled in renderDisplay)
      }
      break;

    case ALARM_RINGING:
      if (snz) {
        doSnooze();
      } else if (cnf) {
        stopAllBuzzers();
        state = HOME;
      }
      break;

    case SNOOZED:
      // Auto-transitions in loop after 2 s; no button action needed
      break;
  }
}



void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[128];
  snprintf(json, sizeof(json),
    "{\"hour\":%d,\"minute\":%d,\"alarm_active\":%s,\"esc_mode\":%d,\"snooze_count\":%d}",
    alarmHour, alarmMinute,
    (state == ALARM_RINGING) ? "true" : "false",
    escMode, snoozeCount);
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
  stopAllBuzzers();
  alarmActive = false;
  snoozeCount = 0;
  state       = HOME;
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
  //snoozeCount++;
  server.send(200, "text/plain", "OK");
}

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

  // Replace everything from WiFi.begin(...) to the syncNTP() call with:
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

    server.on("/status",  HTTP_GET, handleStatus);
    server.on("/set",     HTTP_GET, handleSet);
    server.on("/dismiss", HTTP_GET, handleDismiss);
    server.on("/trigger", HTTP_GET, handleTrigger);
    server.on("/snooze",  HTTP_GET, handleSnooze);
    server.begin();
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

  // ---- Read buttons ---------------------------------------------------------
  updateButton(snoozeBtn);
  updateButton(confirmBtn);

  // ---- Continuous slidepot reads for SET screens ----------------------------
  if (state == SET_HOUR)   tempHour   = slidepotMap(0, 23);
  if (state == SET_MINUTE) tempMinute = slidepotMap(0, 59);
  if (state == DEMO)       demoSel    = slidepotThird();

  // ---- Button handling ------------------------------------------------------
  handleButtons();

  // ---- Alarm trigger (only from HOME when time matches) --------------------
  if (state == HOME && timeValid &&
      curHour == alarmHour && curMinute == alarmMinute && curSecond == 0) {
    alarmStartMs = millis();
    escMode      = 1;
    beepOn       = false;
    beepToggleMs = 0;
    alarmActive = true;
    state        = ALARM_RINGING;
  }

  // ---- Alarm escalation tick ------------------------------------------------
  if (state == ALARM_RINGING) tickAlarmEscalation();

  // ---- Demo buzzer tick -----------------------------------------------------
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
