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
#define RPI_UART_BAUD    115200
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
// Outcome struct — filled during alarm execution, sent back to RPi
// ---------------------------------------------------------------------------
typedef struct {
    bool    woke;
    uint8_t snooze_count;
    uint16_t response_time_s;
    bool    safety_override;
} WakeOutcome;

// ---------------------------------------------------------------------------
// UART helpers
// ---------------------------------------------------------------------------