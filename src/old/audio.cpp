#include "include/audio.h"
#include "include/config.h"
#include <Arduino.h>

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

void audioInit() {
  ledcSetup(0, 1000, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 0);
  pinMode(LOUD_BUZZER_PIN, OUTPUT);
  digitalWrite(LOUD_BUZZER_PIN, LOW);
}

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

void loudBuzzerOn()  { digitalWrite(LOUD_BUZZER_PIN, HIGH); }
void loudBuzzerOff() { digitalWrite(LOUD_BUZZER_PIN, LOW);  }

void playBeepPattern() {
  for (int i = 0; i < 3; i++) {
    if (digitalRead(BUTTON_PIN) == LOW) return;
    playBeep(1000, 300);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  vTaskDelay(pdMS_TO_TICKS(500));
}