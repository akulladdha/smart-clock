#include "include/display.h"
#include "config.h"
#include "include/puzzle.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void displayInit() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
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
  if (alarmActive && puzzleActive) {
    display.printf("Puzzle: %s=?", puzzleQuestion);
  } else if (alarmActive) {
    display.print(">> PRESS TO SNOOZE <<");
  } else if (snoozed) {
    display.printf("Snooze: %02d:%02d", snoozeHour, snoozeMinute);
  } else if (sleeping) {
    display.print("Sleeping... ZZZ");
  } else if (sleepDebtHours > 0.5f) {
    display.printf("Debt: %.1fh  Al:%02d:%02d", sleepDebtHours, alarmHour, alarmMinute);
  } else {
    display.printf("Alarm: %02d:%02d", alarmHour, alarmMinute);
  }

  display.display();
}