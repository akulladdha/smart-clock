#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_log.h"

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -21600;   // CST (UTC-6)
const int   DST_OFFSET_SEC = 3600;

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define BUZZER_PIN      25
#define BUTTON_PIN      26
#define LOUD_BUZZER_PIN 33   // active buzzer — drive HIGH to sound
#define LED_RED_PIN     27   // RGB LED red channel (PWM)
#define LED_GREEN_PIN   14   // RGB LED green channel (PWM)
#define LED_BLUE_PIN    12   // RGB LED blue channel (PWM)
#define SLEEP_BTN_PIN   32   // 'going to sleep' button (INPUT_PULLUP)

// ---------------------------------------------------------------------------
// Flask / LinUCB
// ---------------------------------------------------------------------------
const char* FLASK_IP = "http://10.146.157.13:5001";  // *** UPDATE THIS ***

float         sleepHours     = 6.5f;
float         minsUntilClass = 60.0f;
int           semesterWeek   = 8;
float         pastSnoozeRate = 0.0f;
int           snoozeCount    = 0;
int           currentStrategy  = 1;
unsigned long alarmStartMs     = 0;

// ---------------------------------------------------------------------------
// Sleep tracking
// ---------------------------------------------------------------------------
unsigned long sleepStartMs   = 0;
bool          sleeping       = false;
float         sleepDebtHours = 0.0f;  // positive = in debt, negative = surplus
const float   TARGET_SLEEP_HRS = 8.0f;

// ---------------------------------------------------------------------------
// Puzzle
// ---------------------------------------------------------------------------
bool  puzzleActive   = false;
int   puzzleAnswer   = 0;
int   puzzleAttempts = 0;
char  puzzleQuestion[32] = "";

// ---------------------------------------------------------------------------
// Alarm settings (persisted in NVS)
// ---------------------------------------------------------------------------
int alarmHour   = 7;
int alarmMinute = 30;
int ringtone    = 0;   // 0 = Nokia, 1 = Beeps
int SNOOZE_MINS = 5;

// ---------------------------------------------------------------------------
// Clock state
// ---------------------------------------------------------------------------
volatile int  currentHour   = 0;
volatile int  currentMinute = 0;
volatile int  currentSecond = 0;

// ---------------------------------------------------------------------------
// Alarm state
// ---------------------------------------------------------------------------
volatile bool alarmActive = false;
volatile bool snoozed     = false;
int snoozeHour, snoozeMinute;

// ---------------------------------------------------------------------------
// FreeRTOS mutex
// ---------------------------------------------------------------------------
SemaphoreHandle_t stateMutex;

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
#define NOTE_E5  659
#define NOTE_D5  587
#define NOTE_FS4 370
#define NOTE_GS4 415
#define NOTE_CS5 554
#define NOTE_B4  494
#define NOTE_AS5 932
#define NOTE_A5  880
#define REST     0

int nokiaMelody[] = {
  NOTE_E5,  NOTE_D5,  NOTE_FS4, NOTE_GS4,
  NOTE_CS5, NOTE_B4,  NOTE_D5,  NOTE_E5,
  NOTE_B4,  NOTE_A5,  NOTE_CS5, NOTE_E5,
  NOTE_A5,  REST
};
int nokiaDurations[] = {
  8, 8, 4, 4,
  8, 8, 4, 4,
  8, 8, 4, 4,
  2, 4
};

Preferences prefs;
WebServer   server(80);

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(1, r);
  ledcWrite(2, g);
  ledcWrite(3, b);
}

// Gradually brighten warm white over durationMs
void ledSunrise(int durationMs) {
  int steps = 100;
  int stepDelay = durationMs / steps;
  for (int i = 0; i <= steps; i++) {
    if (alarmActive) return;  // stop if alarm already triggered
    uint8_t brightness = (uint8_t)((i / 100.0f) * 255);
    // warm white: full red, 60% green, 10% blue
    setLED(brightness, (uint8_t)(brightness * 0.6f), (uint8_t)(brightness * 0.1f));
    vTaskDelay(pdMS_TO_TICKS(stepDelay));
  }
}

// Pulse LED for alarm active state
void ledAlarmPulse() {
  for (int i = 0; i < 10; i++) {
    setLED(255, 0, 0);   // red
    vTaskDelay(pdMS_TO_TICKS(200));
    setLED(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void playBeep(int freq, int durationMs) {
  ledcSetup(0, freq, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 128);
  vTaskDelay(pdMS_TO_TICKS(durationMs));
  ledcWrite(0, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void playMelody(int* melody, int* durations, int length) {
  for (int i = 0; i < length; i++) {
    if (digitalRead(BUTTON_PIN) == LOW) return;
    int noteDuration = 1000 / durations[i];
    if (melody[i] == REST) {
      ledcWrite(0, 0);
    } else {
      ledcSetup(0, melody[i], 8);
      ledcAttachPin(BUZZER_PIN, 0);
      ledcWrite(0, 128);
    }
    vTaskDelay(pdMS_TO_TICKS(noteDuration));
    ledcWrite(0, 0);
    vTaskDelay(pdMS_TO_TICKS((int)(noteDuration * 0.3)));
  }
}

void loudBuzzerOn()  { digitalWrite(LOUD_BUZZER_PIN, HIGH); }
void loudBuzzerOff() { digitalWrite(LOUD_BUZZER_PIN, LOW);  }

void playBeepPattern() {
  for (int i = 0; i < 3; i++) {
    if (digitalRead(BUTTON_PIN) == LOW) return;
    playBeep(1000, 300);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  vTaskDelay(pdMS_TO_TICKS(500));
}

// ---------------------------------------------------------------------------
// Flask / LinUCB calls
// ---------------------------------------------------------------------------

int fetchStrategy() {
  HTTPClient http;
  http.begin(String(FLASK_IP) + "/select_strategy");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);

  struct tm timeinfo;
  getLocalTime(&timeinfo);

  char body[256];
  snprintf(body, sizeof(body),
    "{\"hour\":%d,\"day_of_week\":%d,\"sleep_hours\":%.1f,"
    "\"minutes_until_class\":%.1f,\"semester_week\":%d,\"past_snooze_rate\":%.2f}",
    currentHour, timeinfo.tm_wday,
    sleepHours, minsUntilClass, semesterWeek, pastSnoozeRate);

  int strategyIdx = 1;  // default NORMAL if request fails
  int code = http.POST(body);
  if (code == 200) {
    String resp = http.getString();
    int idx = -1;
    sscanf(resp.c_str(), "{\"strategy_index\":%d", &idx);
    if (idx >= 0 && idx <= 2) strategyIdx = idx;
    Serial.printf(">>>Strategy selected: %d\n", strategyIdx);
  } else {
    Serial.printf(">>>fetchStrategy failed code=%d defaulting to 1\n", code);
  }
  http.end();
  return strategyIdx;
}

void sendMorningResult(int strategyIdx, bool woke, float responseTimeSecs) {
  HTTPClient http;
  http.begin(String(FLASK_IP) + "/update_morning_result");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);

  struct tm timeinfo;
  getLocalTime(&timeinfo);

  char body[512];
  snprintf(body, sizeof(body),
    "{\"strategy_index\":%d,\"woke\":%s,\"snooze_count\":%d,"
    "\"response_time_s\":%.1f,\"did_safety_override\":false,"
    "\"context\":{\"hour\":%d,\"day_of_week\":%d,\"sleep_hours\":%.1f,"
    "\"minutes_until_class\":%.1f,\"semester_week\":%d,\"past_snooze_rate\":%.2f}}",
    strategyIdx, woke ? "true" : "false", snoozeCount, responseTimeSecs,
    currentHour, timeinfo.tm_wday,
    sleepHours, minsUntilClass, semesterWeek, pastSnoozeRate);

  int code = http.POST(body);
  Serial.printf(">>>Result sent: strategy=%d woke=%d snoozes=%d time=%.1fs code=%d\n",
    strategyIdx, (int)woke, snoozeCount, responseTimeSecs, code);
  http.end();
}

// ---------------------------------------------------------------------------
// Puzzle
// ---------------------------------------------------------------------------

void generatePuzzle() {
  int a = random(10, 99);
  int b = random(10, 99);
  puzzleAnswer   = a + b;
  puzzleAttempts = 0;
  puzzleActive   = true;
  Serial.printf(">>>Puzzle: %d + %d = ?\n", a, b);
  snprintf(puzzleQuestion, sizeof(puzzleQuestion), "%d + %d", a, b);
}

// ---------------------------------------------------------------------------
// Snooze
// ---------------------------------------------------------------------------

void snooze() {
  snoozeCount++;
  pastSnoozeRate = min(1.0f, pastSnoozeRate + 0.1f);

  xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
  snoozed     = true;
  alarmActive = false;
  ledcWrite(0, 0);
  snoozeMinute = currentMinute + SNOOZE_MINS;
  snoozeHour   = currentHour;
  xSemaphoreGive(stateMutex);

  if (snoozeMinute >= 60) {
    snoozeMinute -= 60;
    snoozeHour = (snoozeHour + 1) % 24;
  }

  playBeep(1000, 100);
  vTaskDelay(pdMS_TO_TICKS(80));
  playBeep(1200, 100);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void updateDisplay() {
  display.clearDisplay();

  display.setTextSize(3);
  display.setCursor(10, 8);
  display.printf("%02d:%02d", currentHour, currentMinute);

  display.setTextSize(2);
  display.setCursor(50, 40);
  display.printf(":%02d", currentSecond);

  display.setTextSize(1);
  display.setCursor(0, 56);
  if (alarmActive && puzzleActive) {
    display.printf("Puzzle: %s=?", puzzleQuestion);
  } else if (alarmActive) {
    display.print(">> PRESS TO SNOOZE <<");
  } else if (snoozed) {
    display.printf("Snooze: %02d:%02d", snoozeHour, snoozeMinute);
  } else if (sleeping) {
    display.print("Sleeping... ZZZ");
  } else if (sleepDebtHours > 0.5f) {
    display.printf("Debt: %.1fh  Al:%02d:%02d", sleepDebtHours, alarmHour, alarmMinute);
  } else {
    display.printf("Alarm: %02d:%02d", alarmHour, alarmMinute);
  }

  display.display();
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

void loadAlarmSettings() {
  prefs.begin("alarm", false);
  alarmHour   = prefs.getInt("hour",     7);
  alarmMinute = prefs.getInt("minute",   30);
  ringtone    = prefs.getInt("ringtone", 0);
  prefs.end();
}

void saveAlarmSettings() {
  prefs.begin("alarm", false);
  prefs.putInt("hour",     alarmHour);
  prefs.putInt("minute",   alarmMinute);
  prefs.putInt("ringtone", ringtone);
  prefs.end();
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

void handleRoot() {
  server.send(200, "text/plain", "Smart Clock API running.");
}

void handleSet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("hour") || !server.hasArg("minute") || !server.hasArg("ringtone")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }
  int h = server.arg("hour").toInt();
  int m = server.arg("minute").toInt();
  int r = server.arg("ringtone").toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59 || r < 0 || r > 1) {
    server.send(400, "text/plain", "Invalid values");
    return;
  }
  xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
  alarmHour   = h;
  alarmMinute = m;
  ringtone    = r;
  snoozed     = false;
  alarmActive = false;
  xSemaphoreGive(stateMutex);
  saveAlarmSettings();
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[64];
  snprintf(json, sizeof(json),
    "{\"hour\":%d,\"minute\":%d,\"ringtone\":%d}",
    alarmHour, alarmMinute, ringtone);
  server.send(200, "application/json", json);
}

void handleDismiss() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  float responseTime = (millis() - alarmStartMs) / 1000.0f;
  sendMorningResult(currentStrategy, true, responseTime);
  snoozeCount    = 0;
  pastSnoozeRate = max(0.0f, pastSnoozeRate - 0.05f);
  xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
  alarmActive = false;
  xSemaphoreGive(stateMutex);
  server.send(200, "text/plain", "OK");
}

void handlePuzzle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (puzzleActive) {
    char json[128];
    snprintf(json, sizeof(json),
      "{\"active\":true,\"question\":\"%s\",\"attempts\":%d}",
      puzzleQuestion, puzzleAttempts);
    server.send(200, "application/json", json);
  } else {
    server.send(200, "application/json", "{\"active\":false}");
  }
}

void handlePuzzleAnswer() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("answer")) {
    server.send(400, "text/plain", "Missing answer");
    return;
  }
  int ans = server.arg("answer").toInt();
  puzzleAttempts++;
  if (ans == puzzleAnswer) {
    puzzleActive = false;
    // Dismiss alarm — same logic as handleDismiss
    float responseTime = (millis() - alarmStartMs) / 1000.0f;
    sendMorningResult(currentStrategy, true, responseTime);
    snoozeCount    = 0;
    pastSnoozeRate = max(0.0f, pastSnoozeRate - 0.05f);
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    alarmActive = false;
    xSemaphoreGive(stateMutex);
    setLED(0, 255, 0);  // green flash = correct
    vTaskDelay(pdMS_TO_TICKS(500));
    setLED(0, 0, 0);
    server.send(200, "application/json", "{\"correct\":true}");
  } else {
    server.send(200, "application/json",
      "{\"correct\":false,\"attempts\":" + String(puzzleAttempts) + "}");
  }
}

void handleSleepStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[128];
  snprintf(json, sizeof(json),
    "{\"sleeping\":%s,\"sleep_debt_hours\":%.1f,\"last_sleep_hours\":%.1f}",
    sleeping ? "true" : "false", sleepDebtHours, sleepHours);
  server.send(200, "application/json", json);
}

void handleSleepToggle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!sleeping) {
    sleeping     = true;
    sleepStartMs = millis();
    setLED(0, 0, 20);
  } else {
    float hoursSlept = (millis() - sleepStartMs) / 3600000.0f;
    sleepHours        = hoursSlept;
    sleepDebtHours   += (TARGET_SLEEP_HRS - hoursSlept);
    sleepDebtHours    = max(-4.0f, min(sleepDebtHours, 16.0f));
    sleeping          = false;
    setLED(0, 0, 0);
  }
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// FreeRTOS tasks
// ---------------------------------------------------------------------------

void displayTask(void* pvParameters) {
  for (;;) {
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    updateDisplay();
    xSemaphoreGive(stateMutex);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void alarmTask(void* pvParameters) {
  for (;;) {
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    bool active = alarmActive;
    int  strat  = currentStrategy;
    xSemaphoreGive(stateMutex);

    if (active) {
      if (strat == 0) {
        // GENTLE: soft melody + dim warm LED
        setLED(80, 48, 8);
        playMelody(nokiaMelody, nokiaDurations, 14);
        setLED(0, 0, 0);
      } else if (strat == 1) {
        // NORMAL: beep pattern + red pulse
        setLED(255, 0, 0);
        playBeepPattern();
        setLED(0, 0, 0);
      } else {
        // NUCLEAR: loud buzzer + passive beeps + bright red
        setLED(255, 0, 0);
        loudBuzzerOn();
        playBeepPattern();
        loudBuzzerOff();
        setLED(0, 0, 0);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void buttonTask(void* pvParameters) {
  for (;;) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (digitalRead(BUTTON_PIN) == LOW) {
        xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
        bool active = alarmActive;
        xSemaphoreGive(stateMutex);
        if (active) snooze();
        while (digitalRead(BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void sleepButtonTask(void* pvParameters) {
  for (;;) {
    if (digitalRead(SLEEP_BTN_PIN) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));  // debounce
      if (digitalRead(SLEEP_BTN_PIN) == LOW) {
        if (!sleeping) {
          // Going to sleep
          sleeping     = true;
          sleepStartMs = millis();
          setLED(0, 0, 20);  // dim blue = sleep mode
          Serial.println(">>>Sleep started");
        } else {
          // Waking up manually (not via alarm)
          float hoursSlept = (millis() - sleepStartMs) / 3600000.0f;
          sleepHours        = hoursSlept;
          sleepDebtHours   += (TARGET_SLEEP_HRS - hoursSlept);
          sleepDebtHours    = max(-4.0f, min(sleepDebtHours, 16.0f));  // clamp
          sleeping          = false;
          setLED(0, 0, 0);
          Serial.printf(">>>Woke: slept=%.1fh debt=%.1fh\n", hoursSlept, sleepDebtHours);
        }
        while (digitalRead(SLEEP_BTN_PIN) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void sunriseTask(void* pvParameters) {
  for (;;) {
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    int minsUntilAlarm = (alarmHour * 60 + alarmMinute) - (currentHour * 60 + currentMinute);
    bool active = alarmActive;
    xSemaphoreGive(stateMutex);

    if (!active && minsUntilAlarm == 30) {
      ledSunrise(30 * 60 * 1000);  // 30 min sunrise
    } else if (active) {
      setLED(255, 60, 10);  // full warm white during alarm
    } else if (!active && !snoozed) {
      setLED(0, 0, 0);  // off when idle
    }
    vTaskDelay(pdMS_TO_TICKS(30000));  // check every 30 seconds
  }
}

void serverTask(void* pvParameters) {
  for (;;) {
    server.handleClient();

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
      currentHour   = timeinfo.tm_hour;
      currentMinute = timeinfo.tm_min;
      currentSecond = timeinfo.tm_sec;

      bool isAlarmTime  = (currentHour == alarmHour && currentMinute == alarmMinute
                           && !alarmActive && !snoozed);
      bool isSnoozeTime = (currentHour == snoozeHour && currentMinute == snoozeMinute
                           && currentSecond == 0 && snoozed);

      if (isAlarmTime || isSnoozeTime) {
        alarmActive  = true;
        snoozed      = false;
        alarmStartMs = millis();
        xSemaphoreGive(stateMutex);
        // These HTTP calls must be outside mutex
        generatePuzzle();
        currentStrategy = fetchStrategy();
      } else {
        xSemaphoreGive(stateMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  esp_log_level_set("*",    ESP_LOG_NONE);
  esp_log_level_set("wifi", ESP_LOG_NONE);
  Serial.println(">>>Boot OK");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(3000);
    if (digitalRead(BUTTON_PIN) == LOW) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 24);
      display.println("Resetting WiFi...");
      display.display();
      WiFiManager wm;
      wm.resetSettings();
    }
  }
  ledcSetup(0, 1000, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 0);

  // Loud buzzer
  pinMode(LOUD_BUZZER_PIN, OUTPUT);
  digitalWrite(LOUD_BUZZER_PIN, LOW);

  // RGB LED — use LEDC channels 1, 2, 3 (channel 0 is passive buzzer)
  ledcSetup(1, 5000, 8);
  ledcAttachPin(LED_RED_PIN,   1);
  ledcSetup(2, 5000, 8);
  ledcAttachPin(LED_GREEN_PIN, 2);
  ledcSetup(3, 5000, 8);
  ledcAttachPin(LED_BLUE_PIN,  3);

  // Sleep button
  pinMode(SLEEP_BTN_PIN, INPUT_PULLUP);

  stateMutex = xSemaphoreCreateMutex();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.print("Connecting...");
  display.display();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("WiFi setup needed.");
    display.println("Connect to AP:");
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.println("SmartClock");
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.println("then open 192.168.4.1");
    display.display();
  });

  // wm.resetSettings();  // uncomment only to force re-setup
  if (!wm.autoConnect("SmartClock-Setup")) {
    Serial.println(">>>WiFi failed — restarting");
    ESP.restart();
  }

  Serial.print(">>>IP: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("Connected!");
  display.setCursor(0, 36);
  display.print(WiFi.localIP().toString());
  display.display();
  delay(2000);

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  int ntpRetries = 0;
  while (!getLocalTime(&timeinfo) && ntpRetries < 20) {
    delay(500);
    ntpRetries++;
  }

  loadAlarmSettings();

  server.on("/",        HTTP_GET, handleRoot);
  server.on("/set",     HTTP_GET, handleSet);
  server.on("/status",  HTTP_GET, handleStatus);
  server.on("/dismiss", HTTP_GET, handleDismiss);
  server.on("/puzzle",        HTTP_GET, handlePuzzle);
  server.on("/puzzle/answer", HTTP_GET, handlePuzzleAnswer);
  server.on("/sleep_status",  HTTP_GET, handleSleepStatus);
  server.on("/sleep_toggle",  HTTP_GET, handleSleepToggle);
  server.on("/set", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/status", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/dismiss", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/sleep_toggle", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/sleep_status", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/puzzle", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.on("/puzzle/answer", HTTP_OPTIONS, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.send(204);
  });
  server.begin();

  xTaskCreatePinnedToCore(serverTask,      "Server",   4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(displayTask,     "Display",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(alarmTask,       "Alarm",    4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask,      "Button",   1024, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(sunriseTask,     "Sunrise",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(sleepButtonTask, "SleepBtn", 1024, NULL, 1, NULL, 1);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
