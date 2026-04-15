#include <Arduino.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <time.h>
#include "esp_log.h"

#include "include/config.h"
#include "include/display.h"
#include "include/audio.h"
#include "include/led.h"
#include "include/network.h"
#include "include/alarm.h"
#include "include/puzzle.h"

WebServer server(80);

// ---------------------------------------------------------------------------
// FreeRTOS tasks defined here
// ---------------------------------------------------------------------------

void displayTask(void* pvParameters) {
  for (;;) {
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    updateDisplay();
    xSemaphoreGive(stateMutex);
    vTaskDelay(pdMS_TO_TICKS(200));
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
        // HTTP calls outside mutex
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

  // WiFi reset check
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(3000);
    if (digitalRead(BUTTON_PIN) == LOW) {
      WiFiManager wm;
      wm.resetSettings();
    }
  }

  stateMutex = xSemaphoreCreateMutex();
  pinMode(SLEEP_BTN_PIN, INPUT_PULLUP);

  audioInit();
  ledInit();
  displayInit();

  networkInit();
  loadAlarmSettings();
  registerRoutes();
  server.begin();

  xTaskCreatePinnedToCore(serverTask,      "Server",   4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(displayTask,     "Display",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(alarmTask,       "Alarm",    4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask,      "Button",   1024, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(sunriseTask,     "Sunrise",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(sleepButtonTask, "SleepBtn", 1024, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}