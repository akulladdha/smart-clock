// #include <Arduino.h>
// #include "include/uart.h"


// void send_byte(uint8_t b) {
//     RPI_UART.write(b);
// }

// void send_frame(uint8_t code) {
//     send_byte(START_BYTE);
//     send_byte(code);
//     Serial.printf("[UART] TX: START=0xAA CODE=0x%02X\n", code);
// }

// // Blocks indefinitely until one byte is available
// uint8_t recv_byte() {
//     while (RPI_UART.available() == 0);
//     return (uint8_t)RPI_UART.read();
// }

// // Discard bytes until we see the start byte
// void wait_for_start() {
//     while (recv_byte() != START_BYTE);
// }

// // ---------------------------------------------------------------------------
// // Protocol actions
// // ---------------------------------------------------------------------------

// void send_ready() {
//     send_frame(CODE_READY);
//     Serial.println("[UART] Sent READY");
// }

// Strategy recv_strategy() {
//     Serial.println("[UART] Waiting for strategy...");
//     while (true) {
//         wait_for_start();
//         uint8_t code = recv_byte();
//         switch (code) {
//             case CODE_GENTLE:  return STRAT_GENTLE;
//             case CODE_NORMAL:  return STRAT_NORMAL;
//             case CODE_NUCLEAR: return STRAT_NUCLEAR;
//             default:
//                 Serial.printf("[UART] Unexpected code 0x%02X, discarding.\n", code);
//         }
//     }
// }

// void send_wake_report(WakeOutcome &out) {
//     send_byte(START_BYTE);
//     send_byte(CODE_WAKE_REPORT);

//     // 5 payload bytes matching Python struct.unpack(">BB H B", payload)
//     uint8_t payload[5] = {
//         (uint8_t)out.woke,
//         out.snooze_count,
//         (uint8_t)((out.response_time_s >> 8) & 0xFF),  // high byte
//         (uint8_t)(out.response_time_s & 0xFF),          // low byte
//         (uint8_t)out.safety_override,
//     };
//     RPI_UART.write(payload, sizeof(payload));
//     Serial.println("[UART] Sent WAKE_REPORT");
// }

// void wait_for_ack() {
//     Serial.println("[UART] Waiting for ACK...");
//     while (true) {
//         wait_for_start();
//         uint8_t code = recv_byte();
//         if (code == CODE_ACK) {
//             Serial.println("[UART] ACK received.");
//             return;
//         }
//         Serial.printf("[UART] Expected ACK, got 0x%02X, discarding.\n", code);
//     }
// }

// // ---------------------------------------------------------------------------
// // Hardware stubs — replace with your actual driver calls
// // ---------------------------------------------------------------------------

// void set_led_brightness(float brightness) {
//     // Drive LED via LEDC PWM
//     Serial.printf("[HW] LED brightness -> %.2f\n", brightness);
// }

// void play_gentle_beep(int level) {
//     Serial.printf("[HW] Gentle beep level=%d\n", level);
// }

// void trigger_phone_buzz(int level) {
//     Serial.printf("[HW] Phone buzz level=%d\n", level);
// }

// void play_strobe(int duration_ms) {
//     Serial.printf("[HW] Strobe %d ms\n", duration_ms);
// }

// void play_loud_alarm(int level) {
//     Serial.printf("[HW] LOUD ALARM level=%d\n", level);
// }

// bool check_puzzle_solved() {
//     return false; // stub — poll your button/display
// }

// bool check_snooze_pressed() {
//     return false; // stub
// }

// // ---------------------------------------------------------------------------
// // Light ramp — runs before alarm triggers
// // ---------------------------------------------------------------------------

// void run_light_ramp(int ramp_minutes) {
//     int steps = 20;
//     int step_delay_ms = (ramp_minutes * 60 * 1000) / steps;
//     for (int i = 0; i <= steps; i++) {
//         set_led_brightness((float)i / steps);
//         delay(step_delay_ms);
//     }
// }

// // ---------------------------------------------------------------------------
// // Strategy runners
// // ---------------------------------------------------------------------------

// bool run_gentle(WakeOutcome &out) {
//     for (int level = 1; level <= 5; level++) {
//         play_gentle_beep(level);
//         unsigned long start = millis();
//         while (millis() - start < 30000) {
//             if (check_puzzle_solved()) return true;
//             if (check_snooze_pressed()) {
//                 out.snooze_count++;
//                 delay(600000);
//             }
//         }
//     }
//     trigger_phone_buzz(3);
//     delay(30000);
//     if (check_puzzle_solved()) return true;
//     play_strobe(10000);
//     return check_puzzle_solved();
// }

// bool run_normal(WakeOutcome &out) {
//     for (int level = 1; level <= 3; level++) {
//         trigger_phone_buzz(level);
//         unsigned long start = millis();
//         while (millis() - start < 30000) {
//             if (check_puzzle_solved()) return true;
//             if (check_snooze_pressed()) {
//                 out.snooze_count++;
//                 delay(600000);
//             }
//         }
//     }
//     play_strobe(10000);
//     delay(15000);
//     if (check_puzzle_solved()) return true;
//     play_loud_alarm(2);
//     return check_puzzle_solved();
// }

// bool run_nuclear(WakeOutcome &out) {
//     play_loud_alarm(3);
//     delay(10000);
//     if (check_puzzle_solved()) return true;
//     play_loud_alarm(5);
//     delay(10000);
//     if (check_puzzle_solved()) return true;
//     // Final: all modalities simultaneously
//     trigger_phone_buzz(5);
//     play_strobe(5000);
//     play_loud_alarm(7);
//     out.safety_override = true;
//     return check_puzzle_solved();
// }

// // ---------------------------------------------------------------------------
// // Arduino entry points
// // ---------------------------------------------------------------------------

// void setup() {
//     Serial.begin(115200);                          // USB debug
//     RPI_UART.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_RX, RPI_TX);  // RPi UART
//     Serial.println("[BOOT] ESP32 alarm controller starting...");

//     send_ready();

//     Strategy strategy = recv_strategy();
//     Serial.printf("[MAIN] Strategy received: %d\n", strategy);

//     run_light_ramp(30);

//     WakeOutcome outcome = {false, 0, 0, false};
//     unsigned long t_start = millis();

//     switch (strategy) {
//         case STRAT_GENTLE:  outcome.woke = run_gentle(outcome);  break;
//         case STRAT_NORMAL:  outcome.woke = run_normal(outcome);  break;
//         case STRAT_NUCLEAR: outcome.woke = run_nuclear(outcome); break;
//         default:
//             Serial.println("[MAIN] Unknown strategy, defaulting to NORMAL");
//             outcome.woke = run_normal(outcome);
//     }

//     outcome.response_time_s = (uint16_t)((millis() - t_start) / 1000);

//     send_wake_report(outcome);
//     wait_for_ack();

//     Serial.println("[MAIN] Cycle complete. Going to deep sleep.");
//     // esp_sleep_enable_timer_wakeup(time_us);
//     // esp_deep_sleep_start();
// }

// void loop() {
//     // Everything runs once in setup(), loop() stays empty
// }