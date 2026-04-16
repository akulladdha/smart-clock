#include "include/alarm.h"
#include "include/config.h"
#include "include/audio.h"
#include "include/led.h"
#include "include/sleep_tracking.h"

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

void alarmTask(void* pvParameters) {
  for (;;) {
    xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100));
    bool active = alarmActive;
    int  strat  = currentStrategy;
    xSemaphoreGive(stateMutex);

    if (active) {
      if (strat == 0) {
        setLED(80, 48, 8);
        playMelody(nokiaMelody, nokiaDurations, 14);
        setLED(0, 0, 0);
      } else if (strat == 1) {
        setLED(255, 0, 0);
        playBeepPattern();
        setLED(0, 0, 0);
      } else {
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
        while (digitalRead(BUTTON_PIN) == LOW) vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void sleepButtonTask(void* pvParameters) {
  for (;;) {
    if (digitalRead(SLEEP_BTN_PIN) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (digitalRead(SLEEP_BTN_PIN) == LOW) {
        if (!sleeping) {
          handleSleepStart();
        } else {
          handleSleepEnd();
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
      ledSunrise(30 * 60 * 1000);
    } else if (active) {
      setLED(255, 60, 10);
    } else if (!active && !snoozed) {
      setLED(0, 0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}