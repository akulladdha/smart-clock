#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include "esp_log.h"

// --- NTP (adjust GMT_OFFSET_SEC for your timezone, e.g. 19800 = UTC+5:30) ---
const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET_SEC  = -21600;
const int   DST_OFFSET_SEC  = 3600;

// --- Hardware ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define BUZZER_PIN  25
#define BUTTON_PIN  26

// --- Alarm settings (persisted in NVS) ---
int alarmHour   = 7;
int alarmMinute = 30;
int ringtone    = 0;   // 0 = Nokia, 1 = Beeps
int SNOOZE_MINS = 5;

// --- Clock state ---
volatile int currentHour = 0, currentMinute = 0, currentSecond = 0;

// --- Alarm state ---
volatile bool alarmActive = false;
volatile bool snoozed     = false;
int  snoozeHour, snoozeMinute;

// --- FreeRTOS mutex ---
SemaphoreHandle_t stateMutex;

// --- Notes ---
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

void playBeepPattern() {
  for (int i = 0; i < 3; i++) {
    if (digitalRead(BUTTON_PIN) == LOW) return;
    playBeep(1000, 300);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  vTaskDelay(pdMS_TO_TICKS(500));
}

void snooze() {
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
  if (alarmActive) {
    display.print(">> PRESS TO SNOOZE <<");
  } else if (snoozed) {
    display.printf("Snooze: %02d:%02d", snoozeHour, snoozeMinute);
  } else {
    display.printf("Alarm: %02d:%02d", alarmHour, alarmMinute);
  }

  display.display();
}

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

// --- HTTP handlers ---

void handleRoot() {
  server.sendHeader("Cache-Control", "max-age=3600");  // ADD THIS
  server.send(200, "text/html", R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Clock</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #111;
      color: #f0f0f0;
      display: flex;
      justify-content: center;
      align-items: flex-start;
      min-height: 100vh;
      padding: 32px 16px;
    }

    .card {
      background: #1e1e1e;
      border-radius: 16px;
      padding: 28px 24px;
      width: 100%;
      max-width: 360px;
    }

    h1 {
      font-size: 1.3rem;
      font-weight: 600;
      margin-bottom: 24px;
      color: #fff;
    }

    label {
      display: block;
      font-size: 0.8rem;
      color: #888;
      margin-bottom: 6px;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }

    input[type="text"], input[type="number"] {
      width: 100%;
      padding: 12px 14px;
      background: #2a2a2a;
      border: 1px solid #333;
      border-radius: 10px;
      color: #f0f0f0;
      font-size: 1rem;
      outline: none;
      transition: border-color 0.2s;
    }

    input:focus { border-color: #4a9eff; }

    .row {
      display: flex;
      gap: 12px;
    }

    .row .field { flex: 1; }

    .field { margin-bottom: 20px; }

    .ringtone-group {
      display: flex;
      gap: 10px;
      margin-bottom: 24px;
    }

    .rt-btn {
      flex: 1;
      padding: 12px;
      border: 2px solid #333;
      border-radius: 10px;
      background: #2a2a2a;
      color: #aaa;
      font-size: 0.95rem;
      cursor: pointer;
      transition: all 0.15s;
    }

    .rt-btn.active {
      border-color: #4a9eff;
      background: #1a3a5c;
      color: #fff;
    }

    .set-btn {
      width: 100%;
      padding: 14px;
      background: #4a9eff;
      color: #fff;
      border: none;
      border-radius: 10px;
      font-size: 1rem;
      font-weight: 600;
      cursor: pointer;
      transition: background 0.2s;
    }

    .set-btn:hover { background: #3a8eef; }
    .set-btn:active { background: #2a7edf; }

    .status {
      margin-top: 14px;
      text-align: center;
      font-size: 0.9rem;
      min-height: 1.2em;
      color: #888;
    }

    .status.ok  { color: #4caf50; }
    .status.err { color: #f44336; }
  </style>
</head>
<body>
<div class="card">
  <h1>Smart Clock</h1>

  <div class="row">
    <div class="field">
      <label>Hour (0–23)</label>
      <input type="number" id="hour" min="0" max="23" placeholder="7">
    </div>
    <div class="field">
      <label>Minute (0–59)</label>
      <input type="number" id="minute" min="0" max="59" placeholder="30">
    </div>
  </div>

  <label style="margin-bottom:10px;">Ringtone</label>
  <div class="ringtone-group">
    <button class="rt-btn active" data-rt="0" onclick="selectRingtone(0)">Nokia</button>
    <button class="rt-btn"       data-rt="1" onclick="selectRingtone(1)">Beeps</button>
  </div>

  <button class="set-btn" onclick="setAlarm()">Set Alarm</button>
  <div class="status" id="status"></div>
</div>

<script>
  let selectedRingtone = 0;

  function selectRingtone(rt) {
    selectedRingtone = rt;
    document.querySelectorAll('.rt-btn').forEach(btn => {
      btn.classList.toggle('active', parseInt(btn.dataset.rt) === rt);
    });
  }

  function setStatus(msg, type) {
    const el = document.getElementById('status');
    el.textContent = msg;
    el.className = 'status ' + (type || '');
  }

  async function setAlarm() {
    const h = parseInt(document.getElementById('hour').value);
    const m = parseInt(document.getElementById('minute').value);

    if (isNaN(h) || h < 0 || h > 23) { setStatus('Hour must be 0–23.', 'err'); return; }
    if (isNaN(m) || m < 0 || m > 59) { setStatus('Minute must be 0–59.', 'err'); return; }

    try {
      const res = await fetch('/set?hour=' + h + '&minute=' + m + '&ringtone=' + selectedRingtone);
      if (res.ok) {
        setStatus('Alarm set!', 'ok');
      } else {
        setStatus(`Error ${res.status}`, 'err');
      }
    } catch (e) {
      setStatus('Could not reach ESP32.', 'err');
    }
  }

  async function fetchStatus() {
    try {
      const res  = await fetch('/status');
      const data = await res.json();
      document.getElementById('hour').value   = data.hour;
      document.getElementById('minute').value = data.minute;
      selectRingtone(data.ringtone);
      setStatus('');
    } catch (_) {
      // silently ignore — ESP32 might not be online yet
    }
  }

  window.addEventListener('load', fetchStatus);
</script>
</body>
</html>
)rawhtml");
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

// ---------------------------------------------------------------------------
// FreeRTOS tasks

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
    int  rt     = ringtone;
    xSemaphoreGive(stateMutex);

    if (active) {
      if (rt == 0) {
        playMelody(nokiaMelody, nokiaDurations, 14);
      } else {
        playBeepPattern();
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void buttonTask(void* pvParameters) {
  for (;;) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));  // debounce
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
        alarmActive = true;
        snoozed     = false;
      }
      xSemaphoreGive(stateMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_NONE);
  esp_log_level_set("wifi", ESP_LOG_NONE);  // add this
  Serial.println(">>>Boot OK");
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledcSetup(0, 1000, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 0);

  stateMutex = xSemaphoreCreateMutex();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Show connecting screen
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.print("Connecting...");
  display.display();

  // Connect to WiFi via WiFiManager
  // On first boot (or after reset): creates AP "SmartClock-Setup" for config.
  // On subsequent boots: reconnects automatically using saved credentials.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);  // give up portal after 3 min if no one connects

  // Show AP instructions on OLED while in config portal
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

  //wm.resetSettings();
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

  // Sync time via NTP
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  int ntpRetries = 0;
  while (!getLocalTime(&timeinfo) && ntpRetries < 20) {
    delay(500);
    ntpRetries++;
  }

  // Load alarm settings from NVS
  loadAlarmSettings();

  // Start HTTP server
  server.on("/",       HTTP_GET, handleRoot);
  server.on("/set",    HTTP_GET, handleSet);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  xTaskCreatePinnedToCore(serverTask,  "Server",  4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(displayTask, "Display", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(alarmTask,   "Alarm",   4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask,  "Button",  1024, NULL, 3, NULL, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
