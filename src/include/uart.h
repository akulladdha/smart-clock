#pragma once
#include <Arduino.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// Protocol constants — must match Python exactly
// ---------------------------------------------------------------------------
#define START_BYTE       0xAA

// Inbound codes (RPi -> ESP)
#define CODE_GENTLE      0x00
#define CODE_NORMAL      0x01
#define CODE_NUCLEAR     0x02
#define CODE_ACK         0x12

// Outbound codes (ESP -> RPi)
#define CODE_READY       0x10
#define CODE_WAKE_REPORT 0x11

// ---------------------------------------------------------------------------
// UART config
// ---------------------------------------------------------------------------
#define RPI_UART         Serial2
#define RPI_UART_BAUD    9600
#define RPI_TX           17
#define RPI_RX           16

// ---------------------------------------------------------------------------
// Strategy type
// ---------------------------------------------------------------------------
typedef enum {
    STRAT_GENTLE  = 0,
    STRAT_NORMAL  = 1,
    STRAT_NUCLEAR = 2,
    STRAT_UNKNOWN = -1
} Strategy;

// ---------------------------------------------------------------------------
// Outcome struct
// ---------------------------------------------------------------------------
typedef struct {
    bool     woke;
    uint8_t  snooze_count;
    uint16_t response_time_s;
    bool     safety_override;
} WakeOutcome;

// ---------------------------------------------------------------------------
// RPiUART class — declarations only
// ---------------------------------------------------------------------------
class RPiUART {
public:
    void     begin();
    void     send_ready();
    Strategy recv_strategy();
    void     send_wake_report(WakeOutcome &out);
    void     wait_for_ack();

private:
    void    send_byte(uint8_t b);
    void    send_frame(uint8_t code);
    uint8_t recv_byte();
    void    wait_for_start();
};