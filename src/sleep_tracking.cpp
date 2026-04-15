#include "include/sleep_tracking.h"
#include "include/config.h"
#include "include/led.h"
#include <Arduino.h>

void handleSleepStart() {
  sleeping     = true;
  sleepStartMs = millis();
  setLED(0, 0, 20);
  Serial.println(">>>Sleep started");
}

void handleSleepEnd() {
  float hoursSlept = (millis() - sleepStartMs) / 3600000.0f;
  sleepHours        = hoursSlept;
  sleepDebtHours   += (TARGET_SLEEP_HRS - hoursSlept);
  sleepDebtHours    = max(-4.0f, min(sleepDebtHours, 16.0f));
  sleeping          = false;
  setLED(0, 0, 0);
  Serial.printf(">>>Woke: slept=%.1fh debt=%.1fh\n", hoursSlept, sleepDebtHours);
}