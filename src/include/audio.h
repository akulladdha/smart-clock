#pragma once

void audioInit();
void playBeep(int freq, int durationMs);
void playMelody(int* melody, int* durations, int length);
void loudBuzzerOn();
void loudBuzzerOff();
void playBeepPattern();

// Exported melody data (used by alarm.cpp)
extern int nokiaMelody[];
extern int nokiaDurations[];