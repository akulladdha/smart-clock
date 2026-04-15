#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
extern const char* NTP_SERVER;
extern const long  GMT_OFFSET_SEC;
extern const int   DST_OFFSET_SEC;

// ---------------------------------------------------------------------------
// Hardware pins
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define BUZZER_PIN      25
#define BUTTON_PIN      26
#define LOUD_BUZZER_PIN 33
#define LED_RED_PIN     27
#define LED_GREEN_PIN   14
#define LED_BLUE_PIN    12
#define SLEEP_BTN_PIN   32

// ---------------------------------------------------------------------------
// Flask / LinUCB
// ---------------------------------------------------------------------------
extern const char* FLASK_IP;

// ---------------------------------------------------------------------------
// Shared mutable state
// ---------------------------------------------------------------------------
extern float         sleepHours;
extern float         minsUntilClass;
extern int           semesterWeek;
extern float         pastSnoozeRate;
extern int           snoozeCount;
extern int           currentStrategy;
extern unsigned long alarmStartMs;

// Sleep tracking
extern unsigned long sleepStartMs;
extern bool          sleeping;
extern float         sleepDebtHours;
extern const float   TARGET_SLEEP_HRS;

// Alarm settings
extern int alarmHour;
extern int alarmMinute;
extern int ringtone;
extern int SNOOZE_MINS;

// Clock state
extern volatile int  currentHour;
extern volatile int  currentMinute;
extern volatile int  currentSecond;

// Alarm state
extern volatile bool alarmActive;
extern volatile bool snoozed;
extern int snoozeHour;
extern int snoozeMinute;

// FreeRTOS mutex
extern SemaphoreHandle_t stateMutex;