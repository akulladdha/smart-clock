#include "include/led.h"
#include "include/config.h"
#include <Arduino.h>

void ledInit() {
  ledcSetup(1, 5000, 8);
  ledcAttachPin(LED_RED_PIN,   1);
  ledcSetup(2, 5000, 8);
  ledcAttachPin(LED_GREEN_PIN, 2);
  ledcSetup(3, 5000, 8);
  ledcAttachPin(LED_BLUE_PIN,  3);
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(1, r);
  ledcWrite(2, g);
  ledcWrite(3, b);
}

void ledSunrise(int durationMs) {
  int steps = 100;
  int stepDelay = durationMs / steps;
  for (int i = 0; i <= steps; i++) {
    if (alarmActive) return;
    uint8_t brightness = (uint8_t)((i / 100.0f) * 255);
    setLED(brightness, (uint8_t)(brightness * 0.6f), (uint8_t)(brightness * 0.1f));
    vTaskDelay(pdMS_TO_TICKS(stepDelay));
  }
}

void ledAlarmPulse() {
  for (int i = 0; i < 10; i++) {
    setLED(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    setLED(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}