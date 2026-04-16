import os
from bandit import LinUCBBandit, AlarmContext
from interface import AlarmUARTInterface

if __name__ == "__main__":
    bandit = LinUCBBandit(alpha=1.8)
    bandit.load("bandit_state.json")

    # Build context from your schedule/sensor data
    ctx = AlarmContext(
        hour=7, day_of_week=0, sleep_hours=6.5,
        minutes_until_class=30, semester_week=8,
        past_snooze_rate=0.4
    )

    interface = AlarmUARTInterface(bandit, port="/dev/serial0", baud=115200)
    interface.run_alarm_cycle(ctx)

    # After ACK, shut down RPi — ESP will cut power via MOSFET
    #os.system("sudo shutdown -h now")