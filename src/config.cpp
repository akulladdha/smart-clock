#include "include/config.h"

const char* NTP_SERVER     = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -21600;
const int   DST_OFFSET_SEC = 3600;

const char* FLASK_IP = "http://10.146.157.13:5001";

float         sleepHours     = 6.5f;
float         minsUntilClass = 60.0f;
int           semesterWeek   = 8;
float         pastSnoozeRate = 0.0f;
int           snoozeCount    = 0;
int           currentStrategy  = 1;
unsigned long alarmStartMs     = 0;

unsigned long sleepStartMs   = 0;
bool          sleeping       = false;
float         sleepDebtHours = 0.0f;
const float   TARGET_SLEEP_HRS = 8.0f;

int alarmHour   = 7;
int alarmMinute = 30;
int ringtone    = 0;
int SNOOZE_MINS = 5;

volatile int  currentHour   = 0;
volatile int  currentMinute = 0;
volatile int  currentSecond = 0;

volatile bool alarmActive = false;
volatile bool snoozed     = false;
int snoozeHour   = 0;
int snoozeMinute = 0;

SemaphoreHandle_t stateMutex = nullptr;