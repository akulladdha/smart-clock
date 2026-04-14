import numpy as np
import json
import os
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from dataclasses import dataclass, field, asdict
from typing import Optional
from datetime import datetime, timedelta

# ---------------------------------------------------------------------------
# How it works
# ---------------------------------------------------------------------------
#NOTE: Each of these progression start with a light, gradually getting brighter and brighter ab 30mins before alarm, strobe just flashes it on/off
# Arm 0: The "Gentle"   Soft Beep   Phone Buzz  Strobe Light
# Arm 1: The "Standard" Phone Buzz  Strobe Light    Loud Alarm
# Arm 2: The "Emergency"    Loud Alarm  Loud Alarm2 Phone Buzz + Strobe + Loud Alarm3

#Idea is that the bandit selects one of these three options and then ESP goes through the progression
#If user still not awake after x mins, start emergency progression
#The varying loud alarms will be different Rhythmms / sounds / volume levels

# ---------------------------------------------------------------------------
# Implementation
# ---------------------------------------------------------------------------

# Alarm has puzzle and snooze button
# If puzzle is solved user is awake, if snooze is hit, alarm waits 10mins and then continues on in progression that bandit selected

# 30mins before alarm, esp starts slowly turning on light
# Then alarms starts, goes through the modalities at increasing intensities, if user still not awake, do emergency
# Then before esp goes back into low power mode it sends the colleceted data to RPI and gets the next wakeup sequence
# RPI shuts down, ESP goes into low power mode (For RPI to shutdown ESP needs to control MOSFET on RPI power supply)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------


ARMS = ["GENTLE_STRAT", "NORMAL_STRAT", "NUCLEAR_STRAT"]
N_ARMS = len(ARMS)
N_FEATURES = 8


# Intensity Penalties: High intensity = High penalty.
# This prevents the Bandit from "cheating" by always choosing the Loudest alarm.
STRATEGY_PENALTIES = {
    "GENTLE_STRAT": 0.0,    # No penalty: Beeps/Light
    "NORMAL_STRAT": 0.15,   # Mild penalty: Vibration/Buzz
    "NUCLEAR_STRAT": 0.40   # Heavy penalty: Loud Alarm/Siren
}

# ---------------------------------------------------------------------------
# Context + Event dataclasses
# ---------------------------------------------------------------------------

@dataclass
class AlarmContext:
    hour: int                    # 0–23
    day_of_week: int             # 0=Mon … 6=Sun
    sleep_hours: float           # estimated hours slept
    minutes_until_class: float   # 0–180+
    semester_week: int           # 1–16
    past_snooze_rate: float      # 0.0–1.0  (rolling avg over last 7 days)


@dataclass
class AlarmEvent:
    context: AlarmContext
    arm_chosen: int
    escalation_steps: int        # 0 = woke on first modality
    woke: bool
    reward: float
    timestamp: str = field(default_factory=lambda: datetime.now().isoformat())


# ---------------------------------------------------------------------------
# LinUCB core
# ---------------------------------------------------------------------------

class LinUCBArm:
    """Single arm of a disjoint LinUCB bandit."""

    def __init__(self, n_features: int, alpha: float):
        self.alpha = alpha
        self.A = np.identity(n_features)
        self.b = np.zeros(n_features)

    def ucb_score(self, x: np.ndarray) -> float:
        A_inv = np.linalg.inv(self.A)
        theta = A_inv @ self.b
        return float(theta @ x + self.alpha * np.sqrt(x @ A_inv @ x))

    def update(self, x: np.ndarray, reward: float):
        self.A += np.outer(x, x)
        self.b += reward * x

    def expected_reward(self, x: np.ndarray) -> float:
        """Exploitation-only estimate (no UCB bonus) — useful for evaluation."""
        theta = np.linalg.inv(self.A) @ self.b
        return float(theta @ x)


class LinUCBBandit:
    """
    Contextual bandit that selects the best wake modality given an AlarmContext.

    Feature vector (8-dim):
        [sin(hour), cos(hour), day_of_week/6, sleep_hours/10,
         min_until_class/180, semester_week/16, past_snooze_rate, 1.0]
    """

    def __init__(self, alpha: float = 1.0):
        self.alpha = alpha
        self.arms = [LinUCBArm(N_FEATURES, alpha) for _ in range(N_ARMS)]
        self.history: list[AlarmEvent] = []

    # ------------------------------------------------------------------
    # Feature engineering
    # ------------------------------------------------------------------

    @staticmethod
    def context_to_features(ctx: AlarmContext) -> np.ndarray:
        return np.array([
            np.sin(2 * np.pi * ctx.hour / 24),
            np.cos(2 * np.pi * ctx.hour / 24),
            ctx.day_of_week / 6.0,
            min(ctx.sleep_hours, 10.0) / 10.0,
            min(ctx.minutes_until_class, 180.0) / 180.0,
            ctx.semester_week / 16.0,
            ctx.past_snooze_rate,
            1.0,  # bias
        ])

    # ------------------------------------------------------------------
    # Core bandit interface
    # ------------------------------------------------------------------

    def select_arm(self, ctx: AlarmContext) -> int:
        x = self.context_to_features(ctx)
        scores = [arm.ucb_score(x) for arm in self.arms]
        return int(np.argmax(scores))

    def update(self, ctx: AlarmContext, arm: int, reward: float):
        x = self.context_to_features(ctx)
        self.arms[arm].update(x, reward)

    def record(self, event: AlarmEvent):
        self.history.append(event)
        self.update(event.context, event.arm_chosen, event.reward)

    # ------------------------------------------------------------------
    # Persistence
    # ------------------------------------------------------------------

    def save(self, path: str = "bandit_state.json"):
        state = {
            "alpha": self.alpha,
            "arms": [{"A": a.A.tolist(), "b": a.b.tolist()} for a in self.arms],
        }
        with open(path, "w") as f:
            json.dump(state, f)
        print(f"[Bandit] Saved to {path}")

    def load(self, path: str = "bandit_state.json"):
        if not os.path.exists(path):
            print(f"[Bandit] No saved state at {path}, starting fresh.")
            return
        with open(path) as f:
            state = json.load(f)
        self.alpha = state["alpha"]
        for arm, s in zip(self.arms, state["arms"]):
            arm.A = np.array(s["A"])
            arm.b = np.array(s["b"])
            arm.alpha = self.alpha
        print(f"[Bandit] Loaded from {path}")


# ---------------------------------------------------------------------------
# Reward function
# ---------------------------------------------------------------------------


def compute_reward(
    woke: bool, 
    snooze_count: int, 
    response_time_s: float, 
    strategy_used: str,
    did_safety_override: bool = False
) -> float:
    """
    Calculates the final reward [0, 1] for the Bandit's morning performance.
    
    Args:
        woke: Did the user eventually solve the puzzle?
        snooze_count: How many times did the user hit snooze?
        response_time_s: Total seconds from first alarm to puzzle solved.
        strategy_used: The name of the strategy the bandit selected.
        did_safety_override: True if the ESP32 had to force a 'Nuclear' alarm
                             because the Bandit's choice failed for too long.
    """
    
    # 1. The "Failure" Case
    # If the user never woke up, or the system had to force a safety override,
    # we return a near-zero or zero reward to strongly discourage this outcome.
    if not woke or did_safety_override:
        return 0.05 if did_safety_override else 0.0

    # 2. Base Efficiency Score (Starts at 1.0)
    # We penalize based on snoozes. Each snooze is a sign the strategy was too weak.
    # A 0.2 penalty means 5 snoozes = 0.0 base reward.
    base_reward = max(0.0, 1.0 - (snooze_count * 0.2))

    # 3. Speed Bonus
    # We reward the user (and the agent) for a total morning time under 10 mins (600s).
    time_bonus = max(0.0, 0.2 - (response_time_s / 600.0))

    # 4. Disruption Penalty (The Alignment Guardrail)
    # This is the "cost" of being loud. 
    disruption_penalty = STRATEGY_PENALTIES.get(strategy_used, 0.0)

    # 5. Final Calculation
    # Reward = (Efficiency + Speed) - Annoyance
    # We clip between 0 and 1 to keep the math stable for LinUCB.
    final_reward = (base_reward + time_bonus) - disruption_penalty
    
    return float(np.clip(final_reward, 0.0, 1.0))