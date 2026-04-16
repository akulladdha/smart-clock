#include "include/network.h"
#include "include/config.h"
#include "include/puzzle.h"
#include "include/sleep_tracking.h"
#include "include/led.h"
#include "include/display.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <time.h>
#include <Adafruit_SSD1306.h>

// Declared extern so display.cpp and main.cpp share the same instance
extern Adafruit_SSD1306 display;
extern WebServer server;

Preferences prefs;

// ---------------------------------------------------------------------------
// WiFi + NTP
// ---------------------------------------------------------------------------

void networkInit() {
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
}

// ---------------------------------------------------------------------------
// NVS persistence
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
// Flask / LinUCB
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

  int strategyIdx = 1;
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
// HTTP handlers (defined here, registered via registerRoutes())
// ---------------------------------------------------------------------------

static void handleRoot() {
  server.send(200, "text/plain", "Smart Clock API running.");
}

static void handleSet() {
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

static void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[64];
  snprintf(json, sizeof(json),
    "{\"hour\":%d,\"minute\":%d,\"ringtone\":%d}",
    alarmHour, alarmMinute, ringtone);
  server.send(200, "application/json", json);
}

static void handleDismiss() {
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

static void handlePuzzle() {
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

static void handlePuzzleAnswer() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("answer")) {
    server.send(400, "text/plain", "Missing answer");
    return;
  }
  int ans = server.arg("answer").toInt();
  puzzleAttempts++;
  if (ans == puzzleAnswer) {
    puzzleActive = false;
    float responseTime = (millis() - alarmStartMs) / 1000.0f;
    sendMorningResult(currentStrategy, true, responseTime);
    snoozeCount    = 0;
    pastSnoozeRate = max(0.0f, pastSnoozeRate - 0.05f);
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    alarmActive = false;
    xSemaphoreGive(stateMutex);
    setLED(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    setLED(0, 0, 0);
    server.send(200, "application/json", "{\"correct\":true}");
  } else {
    server.send(200, "application/json",
      "{\"correct\":false,\"attempts\":" + String(puzzleAttempts) + "}");
  }
}

static void handleSleepStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char json[128];
  snprintf(json, sizeof(json),
    "{\"sleeping\":%s,\"sleep_debt_hours\":%.1f,\"last_sleep_hours\":%.1f}",
    sleeping ? "true" : "false", sleepDebtHours, sleepHours);
  server.send(200, "application/json", json);
}

static void handleSleepToggle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!sleeping) {
    handleSleepStart();
  } else {
    handleSleepEnd();
  }
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

static void corsOptions(const char* methods) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", methods);
  server.send(204);
}

void registerRoutes() {
  server.on("/",              HTTP_GET,     handleRoot);
  server.on("/set",           HTTP_GET,     handleSet);
  server.on("/status",        HTTP_GET,     handleStatus);
  server.on("/dismiss",       HTTP_GET,     handleDismiss);
  server.on("/puzzle",        HTTP_GET,     handlePuzzle);
  server.on("/puzzle/answer", HTTP_GET,     handlePuzzleAnswer);
  server.on("/sleep_status",  HTTP_GET,     handleSleepStatus);
  server.on("/sleep_toggle",  HTTP_GET,     handleSleepToggle);

  server.on("/set",           HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/status",        HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/dismiss",       HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/sleep_toggle",  HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/sleep_status",  HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/puzzle",        HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
  server.on("/puzzle/answer", HTTP_OPTIONS, []() { corsOptions("GET, OPTIONS"); });
}