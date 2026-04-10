import time
import random
import math

# --- Configuration matching your C++ Servers ---
LEADER_URL = "http://localhost:12345/submit"
HELPER_URL = "http://localhost:12346/submit"
CLOCK_INTERVAL = 30 # Transmit every 30 seconds

def generate_secret_shares(secret_value, modulus=256):
    """Additive Secret Sharing modulo 256"""
    share_1 = random.randint(0, modulus - 1)
    share_0 = (secret_value + modulus - share_1) % modulus
    return share_0, share_1

class SimulatedBrain:
    """
    Simulates realistic human focus drifting over time.
    Instead of pure randomness, it models the inverse relationship 
    between Beta (focus) and Alpha (relaxation) brainwaves.
    """
    def __init__(self):
        self.focus_level = 0.5  # Starts at 50% focus
        self.time_step = 0

    def get_eeg_reading(self):
        self.time_step += 1
        
        # 1. Simulate human attention span (drifting over time)
        # Uses a slow wave (fatigue/flow state) plus random noise (distractions)
        slow_drift = math.sin(self.time_step * 0.05) * 0.05
        micro_distraction = random.uniform(-0.08, 0.08)
        
        self.focus_level += slow_drift + micro_distraction
        # Clamp focus between 10% and 90% (humans are rarely at absolute 0 or 100)
        self.focus_level = max(0.1, min(0.9, self.focus_level)) 

        # 2. Translate focus into simulated EEG Power Bands (Microvolts squared)
        # When focus is high, Beta spikes. When focus is low, Alpha spikes.
        beta_power = (self.focus_level * 50.0) + random.uniform(2.0, 5.0)
        alpha_power = ((1.0 - self.focus_level) * 50.0) + random.uniform(2.0, 5.0)
        
        return beta_power, alpha_power

def run_eeg_agent():
    print("==================================================")
    print("  [+] Starting High-Fidelity Mock EEG Agent")
    print("  [+] Simulating Alpha/Beta Brainwave Frequencies")
    print("==================================================\n")
    
    brain = SimulatedBrain()
    
    while True:
        beta_accumulator = 0
        alpha_accumulator = 0
        
        print(f"--- Beginning {CLOCK_INTERVAL}-second sampling window ---")
        
        # 1. Sample the "brain" once per second
        for i in range(CLOCK_INTERVAL):
            beta, alpha = brain.get_eeg_reading()
            beta_accumulator += beta
            alpha_accumulator += alpha
            
            # Print a visual tracker of the raw "brainwaves"
            ratio = beta / alpha
            bar = "█" * int(ratio * 10)
            print(f"  Sec {i+1:02d} | Beta: {beta:04.1f} | Alpha: {alpha:04.1f} | Ratio: {ratio:0.2f} | {bar}")
            
            time.sleep(1) # Wait 1 real second
            
        # 2. Calculate the average attention over the 30 seconds
        avg_beta = beta_accumulator / CLOCK_INTERVAL
        avg_alpha = alpha_accumulator / CLOCK_INTERVAL
        final_ratio = avg_beta / avg_alpha
        
        # 3. Map the ratio to your 0-255 scalar requirement
        # A normal Beta/Alpha ratio sits between 0.5 (sleepy) and 2.5 (highly focused)
        normalized_ratio = max(0.0, min(2.5, final_ratio))
        attention_scalar = int((normalized_ratio / 2.5) * 255)
        
        print(f"\n[*] 30s Window Complete.")
        print(f"[*] Aggregated Attention Scalar: {attention_scalar} / 255")
        
        # 4. Cryptographic Shredding
        leader_share, helper_share = generate_secret_shares(attention_scalar)
        print(f"[*] Secret Shares Generated -> Leader: {leader_share}, Helper: {helper_share}")
        
        # 5. Transmit (Uncomment these when the C++ servers are ready for HTTP)
        # requests.post(LEADER_URL, json={"share": leader_share})
        # requests.post(HELPER_URL, json={"share": helper_share})
        print(f"[+] Shares ready for transmission to Ports 12345 & 12346.\n")

if __name__ == "__main__":
    run_eeg_agent()