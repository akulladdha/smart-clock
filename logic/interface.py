import serial
import struct
from datetime import datetime
from bandit import LinUCBBandit, AlarmContext, ARMS, AlarmEvent, compute_reward

START_BYTE = 0xAA

# Outbound codes (RPi -> ESP)
CODE_GENTLE  = 0x00
CODE_NORMAL  = 0x01
CODE_NUCLEAR = 0x02
CODE_ACK     = 0x12

# Inbound codes (ESP -> RPi)
CODE_READY       = 0x10
CODE_WAKE_REPORT = 0x11

ARM_TO_CODE = {
    0: CODE_GENTLE,
    1: CODE_NORMAL,
    2: CODE_NUCLEAR,
}

class AlarmUARTInterface:

    def __init__(self, bandit: LinUCBBandit, port: str = "/dev/serial0", baud: int = 115200):
        self.bandit = bandit
        #timeout=None makes all reads block indefinitely
        self.ser = serial.Serial(port, baud, timeout=None)
        print(f"[UART] Connected on {port} @ {baud}")

    def _send_frame(self, code: int):
        self.ser.write(bytes([START_BYTE, code]))
        print(f"[UART] TX: START=0xAA CODE=0x{code:02X}")

    def _recv_byte(self) -> int:
        """Blocks indefinitely until one byte arrives."""
        return self.ser.read(1)[0]

    def _recv_bytes(self, n: int) -> bytes:
        """Blocks indefinitely until exactly n bytes arrive."""
        return self.ser.read(n)

    def _wait_for_start(self):
        """Spin until we see the start byte, discarding anything else."""
        while True:
            b = self._recv_byte()
            if b == START_BYTE:
                return

    def wait_for_ready(self):
        print("[UART] Waiting for READY...")
        while True:
            self._wait_for_start()
            code = self._recv_byte()
            if code == CODE_READY:
                print("[UART] ESP32 READY received.")
                return

    def dispatch_sequence(self, ctx: AlarmContext) -> int:
        arm = self.bandit.select_arm(ctx)
        code = ARM_TO_CODE[arm]
        self._send_frame(code)
        print(f"[UART] Dispatched strategy: {ARMS[arm]}")
        return arm

    def receive_report(self) -> dict:
        """Blocks indefinitely until a valid WAKE_REPORT frame arrives."""
        print("[UART] Waiting for WAKE_REPORT...")
        while True:
            self._wait_for_start()
            code = self._recv_byte()
            if code != CODE_WAKE_REPORT:
                print(f"[UART] Unexpected code 0x{code:02X}, discarding.")
                continue

            #5 payload bytes: woke, snooze_count, response_time_s (2 bytes BE), safety_override
            payload = self._recv_bytes(5)
            woke, snooze_count, response_time_s, safety_override = struct.unpack(
                ">BB H B", payload
            )
            report = {
                "woke":            bool(woke),
                "snooze_count":    snooze_count,
                "response_time_s": response_time_s,
                "safety_override": bool(safety_override),
            }
            print(f"[UART] RX WAKE_REPORT: {report}")
            return report

    def send_ack(self):
        self._send_frame(CODE_ACK)
        print("[UART] ACK sent.")

    def run_alarm_cycle(self, ctx: AlarmContext):
        self.wait_for_ready() #blocks until ESP ready
        arm = self.dispatch_sequence(ctx)

        report = self.receive_report()

        reward = compute_reward(
            woke=report["woke"],
            snooze_count=report["snooze_count"],
            response_time_s=report["response_time_s"],
            strategy_used=ARMS[arm],
            did_safety_override=report["safety_override"],# This is true if the ESP had to force the emergency alarm
        )

        event = AlarmEvent(
            context=ctx,
            arm_chosen=arm,
            escalation_steps=0,   # no longer tracked over UART, default to 0
            woke=report["woke"],
            reward=reward,
        )


        self.bandit.record(event)
        self.bandit.save("bandit_state.json")
        self.send_ack()
        print(f"[UART] Cycle complete. Arm={ARMS[arm]}, Reward={reward:.3f}")
