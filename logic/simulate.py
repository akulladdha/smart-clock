import numpy as np
import random
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from typing import Optional
from bandit import AlarmEvent, AlarmContext, LinUCBBandit, compute_reward, ARMS, N_ARMS

# Severity order used by escalation engine (least → most disruptive)
SEVERITY_ORDER = [0, 2, 1, 3, 4]  # soft_beep→strobe→loud→buzz→call
#Only used in simulation ^^^ 
# ---------------------------------------------------------------------------
# Simulator  (replaces real hardware during development)
# ---------------------------------------------------------------------------

class StudentSimulator:
    """
    Simulates a student's response to different alarm modalities given context.
    Each student profile has a hidden "true preference" vector — the bandit
    has to discover it through experience.

    Wake probability per arm is deterministic from the profile but noisy
    (Bernoulli draw), mimicking real-world unpredictability.
    """

    def __init__(self, profile: str = "heavy_sleeper"):
        profiles = {
            # (base_wake_prob per arm, snooze_tendency, sleep_irregularity)
            "light_sleeper":  {"arm_probs": [0.85, 0.95, 0.80, 0.70, 0.99], "noise": 0.05},
            "heavy_sleeper":  {"arm_probs": [0.10, 0.55, 0.40, 0.70, 0.95], "noise": 0.10},
            "phone_dependent":{"arm_probs": [0.20, 0.35, 0.30, 0.90, 0.98], "noise": 0.08},
            "light_sensitive":{"arm_probs": [0.40, 0.60, 0.92, 0.55, 0.97], "noise": 0.07},
        }
        p = profiles.get(profile, profiles["heavy_sleeper"])
        self.base_probs = np.array(p["arm_probs"])
        self.noise = p["noise"]
        self.profile = profile

    def wake_probability(self, arm: int, ctx: AlarmContext) -> float:
        """
        Adjust base probability by context factors:
          - Early mornings are harder
          - Low sleep makes all modalities less effective
          - Being close to class time adds urgency (slight boost)
        """
        p = self.base_probs[arm]
        if ctx.hour < 7:
            p *= 0.75                              # harder to wake pre-7am
        if ctx.sleep_hours < 5:
            p *= 0.80                              # sleep deprived
        if ctx.minutes_until_class < 30:
            p = min(1.0, p * 1.15)                # urgency boost
        p += random.gauss(0, self.noise)           # real-world noise
        return float(np.clip(p, 0.0, 1.0))

    def simulate_alarm(self, arm: int, ctx: AlarmContext) -> tuple[bool, int, float]:
        """
        Returns (woke, escalation_steps, response_time_seconds).
        Escalation tries up to 3 additional modalities in severity order.
        """
        severity_idx = SEVERITY_ORDER.index(arm) if arm in SEVERITY_ORDER else 0
        escalation_steps = 0
        response_time = 0.0
        current_arm = arm

        for step in range(4):  # original + up to 3 escalations
            p = self.wake_probability(current_arm, ctx)
            if random.random() < p:
                response_time = (step * 180 + random.uniform(10, 170))
                return True, escalation_steps, response_time
            escalation_steps += 1
            # Move to next severity level
            next_idx = min(severity_idx + step + 1, len(SEVERITY_ORDER) - 1)
            current_arm = SEVERITY_ORDER[next_idx]

        return False, escalation_steps, 720.0  # gave up after ~12 min


# ---------------------------------------------------------------------------
# Context generator (for simulation)
# ---------------------------------------------------------------------------

def generate_context(week: int = 8, realistic: bool = True) -> AlarmContext:
    """
    Generate a realistic AlarmContext for a college student.
    If realistic=True, follows typical student sleep patterns.
    """
    day = random.randint(0, 6)

    if realistic:
        if day >= 5:  # weekend: sleep in
            hour = random.choices(range(7, 13), weights=[1,2,3,4,5,4])[0]
            sleep_hours = random.uniform(7.0, 10.0)
            min_class = random.uniform(120, 180)
        else:         # weekday: early classes common
            hour = random.choices(range(6, 11), weights=[2,5,8,5,2])[0]
            sleep_hours = random.triangular(4.0, 8.5, 6.5)
            min_class = random.uniform(10, 90)
    else:
        hour = random.randint(6, 11)
        sleep_hours = random.uniform(4.0, 9.0)
        min_class = random.uniform(10, 120)

    snooze_rate = random.betavariate(2, 5)  # skewed toward low (most alarms work)

    return AlarmContext(
        hour=hour,
        day_of_week=day,
        sleep_hours=round(sleep_hours, 1),
        minutes_until_class=round(min_class, 1),
        semester_week=week,
        past_snooze_rate=round(snooze_rate, 2),
    )


# ---------------------------------------------------------------------------
# Training / testing functions
# ---------------------------------------------------------------------------

def run_simulation(
    n_episodes: int = 500,
    profile: str = "heavy_sleeper",
    alpha: float = 1.2,
    seed: int = 42,
    verbose: bool = False,
) -> tuple[LinUCBBandit, list[float]]:
    """
    Train the bandit through simulated alarm episodes.

    Returns the trained bandit and a list of per-episode rewards.

    Args:
        n_episodes:  Number of simulated alarm events.
        profile:     Student profile — 'heavy_sleeper', 'light_sleeper',
                     'phone_dependent', or 'light_sensitive'.
        alpha:       UCB exploration parameter. Higher = more exploration.
        seed:        Random seed for reproducibility.
        verbose:     Print each episode's outcome.
    """
    random.seed(seed)
    np.random.seed(seed)

    bandit = LinUCBBandit(alpha=alpha)
    sim = StudentSimulator(profile=profile)
    rewards = []

    print(f"\n{'='*55}")
    print(f"  Simulation: {n_episodes} episodes | profile={profile} | alpha={alpha}")
    print(f"{'='*55}")

    for ep in range(n_episodes):
        week = min(16, 1 + ep // 32)  # advance semester week
        ctx = generate_context(week=week)
        arm = bandit.select_arm(ctx)
        woke, esc_steps, resp_time = sim.simulate_alarm(arm, ctx)
        reward = compute_reward(woke, esc_steps, resp_time)

        event = AlarmEvent(
            context=ctx,
            arm_chosen=arm,
            escalation_steps=esc_steps,
            woke=woke,
            reward=reward,
        )
        bandit.record(event)
        rewards.append(reward)

        if verbose or ep % 100 == 0:
            print(f"  Ep {ep+1:>4} | arm={ARMS[arm]:<15} | "
                  f"woke={'Y' if woke else 'N'} | esc={esc_steps} | reward={reward:.2f}")

    avg = np.mean(rewards[-100:]) if len(rewards) >= 100 else np.mean(rewards)
    print(f"\n  Final 100-ep avg reward: {avg:.3f}")
    print(f"  Arm selection counts: { {ARMS[i]: sum(1 for e in bandit.history if e.arm_chosen==i) for i in range(N_ARMS)} }")
    return bandit, rewards


def evaluate_bandit(bandit: LinUCBBandit, n_eval: int = 200,
                    profile: str = "heavy_sleeper", seed: int = 99) -> dict:
    """
    Evaluate a trained bandit (exploitation only — no UCB bonus) against
    a random baseline.

    Returns a dict of evaluation metrics.
    """
    random.seed(seed)
    np.random.seed(seed)
    sim = StudentSimulator(profile=profile)

    bandit_rewards, random_rewards = [], []
    bandit_woke, random_woke = 0, 0

    for _ in range(n_eval):
        ctx = generate_context()
        x = LinUCBBandit.context_to_features(ctx)

        # Bandit picks best arm (pure exploitation)
        best_arm = int(np.argmax([arm.expected_reward(x) for arm in bandit.arms]))
        rand_arm = random.randint(0, N_ARMS - 1)

        for arm, reward_list, woke_count in [
            (best_arm, bandit_rewards, None),
            (rand_arm, random_rewards, None),
        ]:
            woke, esc, rt = sim.simulate_alarm(arm, ctx)
            r = compute_reward(woke, esc, rt)
            reward_list.append(r)

        b_woke, *_ = sim.simulate_alarm(best_arm, ctx)
        r_woke, *_ = sim.simulate_alarm(rand_arm, ctx)
        bandit_woke += int(b_woke)
        random_woke += int(r_woke)

    metrics = {
        "bandit_avg_reward":  round(float(np.mean(bandit_rewards)), 4),
        "random_avg_reward":  round(float(np.mean(random_rewards)), 4),
        "improvement_pct":    round((np.mean(bandit_rewards) / max(np.mean(random_rewards), 1e-9) - 1) * 100, 1),
        "bandit_wake_rate":   round(bandit_woke / n_eval, 3),
        "random_wake_rate":   round(random_woke / n_eval, 3),
    }

    print(f"\n{'='*55}")
    print(f"  Evaluation ({n_eval} episodes, profile={profile})")
    print(f"{'='*55}")
    for k, v in metrics.items():
        print(f"  {k:<25} {v}")
    return metrics


def compare_alphas(
    alphas: list[float] = [0.5, 1.0, 1.5, 2.5],
    n_episodes: int = 400,
    profile: str = "heavy_sleeper",
) -> dict[float, list[float]]:
    """
    Compare multiple alpha values to find the best exploration/exploitation
    balance for your student profile. Returns reward curves per alpha.
    """
    print(f"\n  Alpha comparison | profile={profile} | episodes={n_episodes}")
    results = {}
    for a in alphas:
        _, rewards = run_simulation(n_episodes=n_episodes, profile=profile,
                                    alpha=a, seed=0, verbose=False)
        results[a] = rewards
        window = min(50, n_episodes)
        print(f"  alpha={a:.1f}  last-{window}-ep avg: {np.mean(rewards[-window:]):.3f}")
    return results


def plot_results(
    rewards: list[float],
    alpha_curves: Optional[dict] = None,
    profile: str = "",
    save_path: Optional[str] = None,
):
    """
    Plot learning curve and (optionally) alpha comparison side-by-side.
    """
    fig = plt.figure(figsize=(13, 4.5))
    n_plots = 2 if alpha_curves else 1
    gs = gridspec.GridSpec(1, n_plots, figure=fig)

    # --- Learning curve ---
    ax1 = fig.add_subplot(gs[0])
    window = 30
    smoothed = np.convolve(rewards, np.ones(window) / window, mode="valid")
    x_smooth = np.arange(window - 1, len(rewards))

    ax1.plot(rewards, alpha=0.2, color="#7F77DD", linewidth=0.8, label="raw reward")
    ax1.plot(x_smooth, smoothed, color="#534AB7", linewidth=2,
             label=f"{window}-ep moving avg")
    ax1.axhline(np.mean(rewards[:50]),  color="#D85A30", linewidth=1,
                linestyle="--", label="early avg (first 50)")
    ax1.axhline(np.mean(rewards[-100:]), color="#1D9E75", linewidth=1,
                linestyle="--", label="late avg (last 100)")
    ax1.set_xlabel("Episode")
    ax1.set_ylabel("Reward")
    ax1.set_title(f"Learning curve — {profile}")
    ax1.legend(fontsize=8)
    ax1.set_ylim(-0.05, 1.1)
    ax1.grid(True, alpha=0.3)

    # --- Alpha comparison ---
    if alpha_curves:
        ax2 = fig.add_subplot(gs[1])
        colors = ["#534AB7", "#1D9E75", "#D85A30", "#BA7517"]
        for (a, curve), color in zip(alpha_curves.items(), colors):
            sm = np.convolve(curve, np.ones(window) / window, mode="valid")
            x_sm = np.arange(window - 1, len(curve))
            ax2.plot(x_sm, sm, color=color, linewidth=2, label=f"α={a}")
        ax2.set_xlabel("Episode")
        ax2.set_ylabel("Reward (smoothed)")
        ax2.set_title("Alpha comparison")
        ax2.legend(fontsize=8)
        ax2.set_ylim(-0.05, 1.1)
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    if save_path:
        plt.savefig(save_path, dpi=150, bbox_inches="tight")
        print(f"  Plot saved to {save_path}")
    plt.show()


def inspect_policy(bandit: LinUCBBandit):
    """
    Print a human-readable summary of what the bandit has learned:
    which arm it would choose across representative contexts.
    """
    test_cases = [
        ("Early weekday, low sleep, urgent",
         AlarmContext(hour=7, day_of_week=0, sleep_hours=4.5,
                      minutes_until_class=20, semester_week=10, past_snooze_rate=0.7)),
        ("Morning weekday, normal sleep",
         AlarmContext(hour=8, day_of_week=2, sleep_hours=7.0,
                      minutes_until_class=60, semester_week=5, past_snooze_rate=0.2)),
        ("Weekend late morning, well rested",
         AlarmContext(hour=10, day_of_week=6, sleep_hours=9.0,
                      minutes_until_class=150, semester_week=3, past_snooze_rate=0.1)),
        ("Finals week, 6am, sleep-deprived",
         AlarmContext(hour=6, day_of_week=1, sleep_hours=3.5,
                      minutes_until_class=30, semester_week=16, past_snooze_rate=0.8)),
        ("High snooze history, class in 15 min",
         AlarmContext(hour=9, day_of_week=3, sleep_hours=5.0,
                      minutes_until_class=15, semester_week=8, past_snooze_rate=0.9)),
    ]

    print(f"\n{'='*65}")
    print(f"  Learned policy inspection")
    print(f"{'='*65}")
    print(f"  {'Context':<42} {'Chosen arm':<17} {'UCB scores'}")
    print(f"  {'-'*62}")
    for label, ctx in test_cases:
        x = LinUCBBandit.context_to_features(ctx)
        scores = [arm.ucb_score(x) for arm in bandit.arms]
        best = int(np.argmax(scores))
        score_str = "  ".join(f"{s:.2f}" for s in scores)
        print(f"  {label:<42} {ARMS[best]:<17} [{score_str}]")

# ---------------------------------------------------------------------------
# Entry point — quick demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":

    # 1. Train on a heavy sleeper profile
    bandit, rewards = run_simulation(
        n_episodes=500,
        profile="heavy_sleeper",
        alpha=1.2,
        verbose=False,
    )

    # 2. Evaluate trained bandit vs random baseline
    evaluate_bandit(bandit, n_eval=200, profile="heavy_sleeper")

    # 3. See what the bandit learned
    inspect_policy(bandit)

    # 4. Compare alpha values to tune exploration
    alpha_curves = compare_alphas(
        alphas=[0.5, 1.0, 1.5, 2.5],
        n_episodes=400,
        profile="heavy_sleeper",
    )

    # 5. Save trained model
    bandit.save("bandit_state.json")

    # 6. Plot everything
    plot_results(rewards, alpha_curves=alpha_curves,
                 profile="heavy_sleeper", save_path="learning_curve.png")