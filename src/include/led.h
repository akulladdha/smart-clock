#pragma once
#include <stdint.h>

void ledInit();
void setLED(uint8_t r, uint8_t g, uint8_t b);
void ledSunrise(int durationMs);
void ledAlarmPulse();