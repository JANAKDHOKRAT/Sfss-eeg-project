/**
 * eeg.rs — Synthetic EEG signal generator.
 *
 * ── What real EEG looks like ────────────────────────────────────────────────
 * EEG measures voltage fluctuations from cortical neurons at the scalp.
 * The signal is broadband noise with structure in specific frequency bands:
 *
 *   Band      | Range   | Amplitude | Cognitive correlate
 *   ----------+---------+-----------+-----------------------------
 *   Delta     | 0.5–4Hz | 20–200µV  | Deep sleep, unconsciousness
 *   Theta     | 4–8Hz   | 10–50µV   | Drowsiness, meditation
 *   Alpha     | 8–13Hz  | 5–30µV    | Relaxed, eyes closed
 *   Beta      | 13–30Hz | 2–20µV    | Active thinking, attention
 *   Gamma     | 30–100Hz| 1–5µV     | High-level binding, focus
 *
 * Attention (active cognition) → high Beta, suppressed Alpha
 * Drowsiness/inattention      → high Alpha/Theta, low Beta
 *
 * Reference amplitude relationships from:
 *   Klimesch (1999), "EEG alpha and theta oscillations"
 *   Pope, Bogart & Bartolome (1995), "Biocybernetic adaptation"
 *
 * ── What the simulator does ─────────────────────────────────────────────────
 * 1. Maintains a latent `attention_state` ∈ [0,1] as a random walk with
 *    mean reversion — it drifts slowly like a real subject's arousal level.
 * 2. Each band's amplitude is a function of attention_state (see table above).
 * 3. Each band is a superposition of sinusoids at representative frequencies.
 * 4. A pink (1/f) noise floor is added using Paul Kellet's IIR algorithm —
 *    the standard reference implementation for 1/f noise.
 * 5. Occasional eye-blink artifacts are injected (large slow deflections).
 *
 * ── Kellet pink noise ───────────────────────────────────────────────────────
 * Real EEG background noise follows a 1/f power spectrum.
 * White noise has equal power at all frequencies.
 * Pink noise has power ∝ 1/f — more low frequency content.
 * Kellet's 6-pole IIR filter approximates this from white noise:
 *   b0..b5 are state variables, updated each sample.
 *   Source: https://www.firstpr.com.au/dsp/pink-noise/  (Kellet, 2011)
 */

use rand::prelude::*;
use std::f64::consts::PI;


pub const SAMPLE_RATE: usize = 256;   // Hz — standard EEG acquisition rate
pub const EPOCH_SAMPLES: usize = 256; // 1 second per epoch

/// The output of one epoch: raw samples + the latent attention state that
/// produced them (used for ground truth comparison against computed score).
pub struct Epoch {
    pub samples: Vec<f32>,
    pub true_attention: f64,  // latent state (0..1) — for validation only
}

/// Kellet 6-pole IIR pink noise state (one per channel).
struct PinkNoise {
    b: [f64; 6],
}

impl PinkNoise {
    fn new() -> Self { Self { b: [0.0; 6] } }

    /// Kellet's algorithm — produces one pink noise sample from white.
    fn next(&mut self, white: f64) -> f64 {
        self.b[0] =  0.99886 * self.b[0] + white * 0.0555179;
        self.b[1] =  0.99332 * self.b[1] + white * 0.0750759;
        self.b[2] =  0.96900 * self.b[2] + white * 0.1538520;
        self.b[3] =  0.86650 * self.b[3] + white * 0.3104856;
        self.b[4] =  0.55000 * self.b[4] + white * 0.5329522;
        self.b[5] = -0.76160 * self.b[5] + white * 0.0168980;
        self.b[0] + self.b[1] + self.b[2] + self.b[3] + self.b[4] + self.b[5]
            + white * 0.5362
    }
}

pub struct EegSimulator {
    t: f64,                   // current time in seconds
    attention_state: f64,     // latent attention ∈ [0, 1]
    state_velocity: f64,      // for smooth random walk
    pink: PinkNoise,
    blink_countdown: usize,   // samples until next blink artifact
    beta_spike_remaining: usize, // samples remaining in beta spike
    beta_spike_amp: f64,      // amplitude of current spike
    rng: StdRng,
}

impl EegSimulator {
    pub fn new() -> Self {
        let mut rng = StdRng::from_entropy();
        let attention_state = rng.gen::<f64>();
        let blink_countdown = rng.gen_range(512..2048);
        Self {
            t: 0.0,
            attention_state,
            state_velocity: 0.0,
            pink: PinkNoise::new(),
            blink_countdown,
            beta_spike_remaining: 0,
            beta_spike_amp: 0.0,
            rng,
        }
    }

    /// Generate one epoch of EPOCH_SAMPLES raw EEG samples.
    pub fn next_epoch(&mut self) -> Epoch {
        let dt = 1.0 / SAMPLE_RATE as f64;

        // ── Update latent attention state (slow random walk, mean-reverting) ──
        let noise = (self.rng.gen::<f64>() - 0.5) * 0.12;
        let reversion = (0.5 - self.attention_state) * 0.02;
        self.state_velocity = self.state_velocity * 0.85 + noise + reversion;

        // Realistic attention "shocks"
        if self.rng.gen::<f64>() < 0.08 {
            let shock = if self.rng.gen::<f64>() < 0.5 { 0.25 } else { -0.25 };
            self.state_velocity += shock;
        }

        self.attention_state = (self.attention_state + self.state_velocity).clamp(0.0, 1.0);
        let a = self.attention_state;

        // ── Beta spike initiation ────────────────────────────────────────────
        // Beta spikes occur during focused attention (a > 0.6) and represent
        // transient bursts of cognitive processing. 12% chance per second when alert.
        if self.beta_spike_remaining == 0 && a > 0.6 && self.rng.gen::<f64>() < 0.12 {
            self.beta_spike_remaining = self.rng.gen_range(25..75); // 100-300ms
            self.beta_spike_amp = self.rng.gen_range(6.0..12.0); // µV boost (reduced)
        }

        // ── Band amplitudes with FLOORS and realistic modulation ─────────────
        // CRITICAL: Real EEG never has zero activity in any band.
        // EI = β / (α + θ) must stay in stable range (~0.1 to ~3.0)
        
        // Delta: gentle modulation, always present (15-35 µV)
        let delta_amp = 15.0 + 20.0 * (1.0 - a);        

        // Theta: suppressed during focus but NEVER ZERO (floor = 4 µV)
        // Range: 4 µV (focused) to 20 µV (drowsy)
        let theta_amp = 4.0 + 16.0 * (1.0 - a).powf(1.3);
        
        // Alpha: suppressed during focus but NEVER ZERO (floor = 3 µV)
        // Range: 3 µV (focused) to 22 µV (relaxed)
        // Using gentler power (1.4 instead of 2.5) to avoid near-zero
        let alpha_amp = 3.0 + 19.0 * (1.0 - a).powf(1.4);
        
        // Beta: boosted during focus (floor = 2.5 µV, peak = 18 µV)
        // Linear-ish rise with slight curve for natural feel
        let beta_base = 2.5 + 15.5 * a.powf(1.4);
        
        // Gamma: moderate rise with attention (floor = 0.8 µV)
        let gamma_amp = 0.8 + 3.2 * a.powf(1.3);

        // Random phase offsets
        let mut ph = |_: ()| -> f64 { self.rng.gen::<f64>() * 2.0 * PI };
        let pd = ph(()); let pt = ph(()); let pa = ph(()); let pb = ph(()); let pg = ph(());

        let mut samples = Vec::with_capacity(EPOCH_SAMPLES);
        let start_t = self.t;

        for i in 0..EPOCH_SAMPLES {
            let t = start_t + i as f64 * dt;

            // ── Delta (0.5–4 Hz) ───────────────────────────────────────────
            let delta = delta_amp * (2.0*PI*1.5*t + pd).sin()
                      + delta_amp * 0.6 * (2.0*PI*2.8*t + pd).sin();

            // ── Theta (4–8 Hz) ─────────────────────────────────────────────
            let theta = theta_amp * (2.0*PI*5.0*t + pt).sin()
                      + theta_amp * 0.5 * (2.0*PI*6.5*t + pt).sin();

            // ── Alpha (8–13 Hz) — "alpha spindle" shape ────────────────────
            let alpha_mod = 1.0 + 0.3 * (2.0*PI*0.2*t).sin();
            let alpha = alpha_amp * alpha_mod * (2.0*PI*10.0*t + pa).sin()
                      + alpha_amp * 0.4 * (2.0*PI*11.5*t + pa).sin();

            // ── Beta (13–30 Hz) with SPIKE handling ────────────────────────
            let spike_contrib = if self.beta_spike_remaining > 0 {
                self.beta_spike_remaining -= 1;
                let decay = self.beta_spike_remaining as f64 / 75.0;
                self.beta_spike_amp * decay * (2.0*PI*22.0*t).sin()
            } else { 0.0 };

            let beta = beta_base * (2.0*PI*17.0*t + pb).sin()
                     + beta_base * 0.6 * (2.0*PI*21.0*t + pb).sin()
                     + beta_base * 0.3 * (2.0*PI*26.0*t + pb).sin()
                     + spike_contrib;

            // ── Gamma (30–50 Hz) ───────────────────────────────────────────
            let gamma = gamma_amp * (2.0*PI*38.0*t + pg).sin()
                      + gamma_amp * 0.4 * (2.0*PI*45.0*t + pg).sin();

            // ── Pink noise background (1/f) ────────────────────────────────
            let white = self.rng.gen::<f64>() * 2.0 - 1.0;
            let pink_sample = self.pink.next(white) * 2.5;

            // ── Eye blink artifact ─────────────────────────────────────────
            let blink_idx = self.blink_countdown;
            let blink = if i < blink_idx && blink_idx - i < 50 {
                let phase = (blink_idx - i) as f64 / 50.0;
                80.0 * (PI * phase).sin()
            } else { 0.0 };

            let sample = (delta + theta + alpha + beta + gamma + pink_sample + blink) as f32;
            samples.push(sample);

            if self.blink_countdown > 0 {
                self.blink_countdown -= 1;
                if self.blink_countdown == 0 {
                    self.blink_countdown = self.rng.gen_range(768..3840);
                }
            }
        }

        self.t += EPOCH_SAMPLES as f64 * dt;

        Epoch { samples, true_attention: a }
    }

    pub fn current_attention(&self) -> f64 { self.attention_state }
}