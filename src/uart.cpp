#include "include/uart.h"

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void RPiUART::begin() {
    RPI_UART.begin(RPI_UART_BAUD, SERIAL_8N1, RPI_RX, RPI_TX);
}

void RPiUART::send_ready() {
    send_frame(CODE_READY);
    Serial.println("[UART] Sent READY");
}

Strategy RPiUART::recv_strategy() {
    Serial.println("[UART] Waiting for strategy...");
    while (true) {
        wait_for_start();
        uint8_t code = recv_byte();
        switch (code) {
            case CODE_GENTLE:  return STRAT_GENTLE;
            case CODE_NORMAL:  return STRAT_NORMAL;
            case CODE_NUCLEAR: return STRAT_NUCLEAR;
            default:
                Serial.printf("[UART] Unexpected code 0x%02X, discarding.\n", code);
        }
    }
}

void RPiUART::send_wake_report(WakeOutcome &out) {
    send_byte(START_BYTE);
    send_byte(CODE_WAKE_REPORT);

    uint8_t payload[5] = {
        (uint8_t)out.woke,
        out.snooze_count,
        (uint8_t)((out.response_time_s >> 8) & 0xFF),
        (uint8_t)(out.response_time_s & 0xFF),
        (uint8_t)out.safety_override,
    };
    RPI_UART.write(payload, sizeof(payload));
    Serial.println("[UART] Sent WAKE_REPORT");
}

void RPiUART::wait_for_ack() {
    Serial.println("[UART] Waiting for ACK...");
    while (true) {
        wait_for_start();
        uint8_t code = recv_byte();
        if (code == CODE_ACK) {
            Serial.println("[UART] ACK received.");
            return;
        }
        Serial.printf("[UART] Expected ACK, got 0x%02X, discarding.\n", code);
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void RPiUART::send_byte(uint8_t b) {
    RPI_UART.write(b);
}

void RPiUART::send_frame(uint8_t code) {
    send_byte(START_BYTE);
    send_byte(code);
    Serial.printf("[UART] TX: START=0xAA CODE=0x%02X\n", code);
}

uint8_t RPiUART::recv_byte() {
    while (RPI_UART.available() == 0);
    return (uint8_t)RPI_UART.read();
}

void RPiUART::wait_for_start() {
    while (recv_byte() != START_BYTE);
}