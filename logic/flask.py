from flask import Flask, request, jsonify
import numpy as np
from bandit import LinUCBBandit, AlarmContext, compute_reward, ARMS, AlarmEvent
from datetime import datetime
import threading

# Import your existing classes (LinUCBBandit, AlarmContext, etc. from your script)
# For this example, I assume they are in the same file or imported.

app = Flask(__name__)

# Global bandit instance
bandit = LinUCBBandit(alpha=1.5)
bandit.load("bandit_state.json")

# Lock to prevent race conditions during updates
bandit_lock = threading.Lock()

# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------
@app.route('/select_strategy', methods=['POST'])
def select_strategy():
    """
    ESP32 hits this at the alarm time.
    """
    data = request.json
    try:
        ctx = AlarmContext(
            hour=data['hour'],
            day_of_week=data['day_of_week'],
            sleep_hours=data['sleep_hours'],
            minutes_until_class=data['minutes_until_class'],
            semester_week=data['semester_week'],
            past_snooze_rate=data['past_snooze_rate']
        )
        
        with bandit_lock:
            arm_idx = bandit.select_arm(ctx)
        
        return jsonify({
            "strategy_index": arm_idx,
            "strategy_name": ARMS[arm_idx],
            "status": "success"
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route('/update_morning_result', methods=['POST'])
def update_morning_result():
    """
    ESP32 hits this once the puzzle is solved.
    Expected JSON: 
    {
        "strategy_index": 1,
        "woke": true,
        "snooze_count": 2,
        "response_time_s": 720,
        "did_safety_override": false,
        "context": { ... original context ... }
    }
    """
    data = request.json
    try:
        # Reconstruct context
        ctx_data = data['context']
        ctx = AlarmContext(**ctx_data)
        
        strategy_name = ARMS[data['strategy_index']]
        
        # Calculate the refined reward
        reward = compute_reward(
            woke=data['woke'], 
            snooze_count=data['snooze_count'], 
            response_time_s=data['response_time_s'],
            strategy_used=strategy_name,
            did_safety_override=data.get('did_safety_override', False)
        )
        
        with bandit_lock:
            # Update the bandit model
            bandit.update(ctx, data['strategy_index'], reward)
            
            # Record in history (optional but good for plots later)
            event = AlarmEvent(
                context=ctx,
                arm_chosen=data['strategy_index'],
                escalation_steps=data['snooze_count'], # Mapping snoozes to steps
                woke=data['woke'],
                reward=reward
            )
            bandit.history.append(event)
            
            bandit.save("bandit_state.json")
            
        return jsonify({
            "status": "success", 
            "reward": round(reward, 3),
            "strategy_used": strategy_name
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route('/stats', methods=['GET'])
def get_stats():
    """Returns a quick summary of what the bandit has learned."""
    return jsonify({
        "total_episodes": len(bandit.history),
        "alpha": bandit.alpha,
        "arms": ARMS
    })

if __name__ == '__main__':
    # Run on 0.0.0.0 so the ESP32 can find it on your local network
    app.run(host='0.0.0.0', port=5000, debug=False)