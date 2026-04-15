#include "include/puzzle.h"
#include <Arduino.h>

bool  puzzleActive   = false;
int   puzzleAnswer   = 0;
int   puzzleAttempts = 0;
char  puzzleQuestion[32] = "";

void generatePuzzle() {
  int a = random(10, 99);
  int b = random(10, 99);
  puzzleAnswer   = a + b;
  puzzleAttempts = 0;
  puzzleActive   = true;
  Serial.printf(">>>Puzzle: %d + %d = ?\n", a, b);
  snprintf(puzzleQuestion, sizeof(puzzleQuestion), "%d + %d", a, b);
}