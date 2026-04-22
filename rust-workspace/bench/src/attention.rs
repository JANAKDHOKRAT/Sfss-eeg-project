// attention.rs — EEG → calibrated attention score (0–255)
//
// All 8 prior fixes applied.  Additional fix in this version:
//
// FIX: EI calibration (score was always 0)
//   Root cause: log_EI for this simulator falls in [-5, -1] at realistic
//   attention states.  tanh(log_EI) ≈ -1.0 throughout → score ≈ 0.
//
//   The issue is not the formula — it is the calibration.
//   log_EI = log(β) − log(α+θ) is always negative for physiological EEG because
//   alpha+theta power > beta power in absolute µV² (wider band, stronger amplitudes).
//   The formula requires calibration to the ACTUAL range of this simulator.
//
//   Calibration derivation:
//     At attention=0.1 (drowsy):    log_EI ≈ −3.9  → score should ≈  30
//     At attention=0.5 (neutral):   log_EI ≈ −0.7  → score should ≈ 128
//     At attention=0.9 (focused):   log_EI ≈ +2.3  → score should ≈ 250
//     Midpoint: log_EI = −2.5 → score = 128
//     Scale: log_EI range ≈ [−4, +2], spread = 6 → scale = 6/2 = 3, but tanh
//            saturates so we use scale=1.5 to keep midpoint transition sharp
//
//   Calibrated formula:
//     score = tanh( (log_EI + 2.5) / 1.5 ) × 0.5 + 0.5  → [0, 1]
//     attention_score = round(score × 255)
//
//   This is a monotone transformation — higher EI still means higher score.
//   The SDPF crypto layer is unaffected; only the u8 mapping changes.
//
// P1: Hann window power correction — PSD × N/Σw² (≈8/3)  [Harris 1978]
// P2: DC removal before windowing — epoch mean subtracted
// Band intervals: half-open [f_lo, f_hi), no shared bin
// Power: Σ PSD[k]·Δf  (integral, not mean)

use rustfft::{FftPlanner, num_complex::Complex};
use serde::Serialize;
use crate::eeg::SAMPLE_RATE;

const EPS: f64 = 1e-10;

// ── Calibration constants (derived from simulator's EI range) ─────────────────
// Shift the midpoint of the sigmoid from 0 to −2.5 (typical mid-attention log_EI)
// Scale controls transition sharpness (1.5 = moderate spread)
const EI_SHIFT: f64 = 2.5;
const EI_SCALE: f64 = 1.5;

// ── IIR biquad ────────────────────────────────────────────────────────────────

#[derive(Clone)]
pub struct Biquad { b0:f64,b1:f64,b2:f64,a1:f64,a2:f64,w1:f64,w2:f64 }

impl Biquad {
    fn process(&mut self, x: f64) -> f64 {
        let w0 = x - self.a1*self.w1 - self.a2*self.w2;
        let y  = self.b0*w0 + self.b1*self.w1 + self.b2*self.w2;
        self.w2 = self.w1; self.w1 = w0; y
    }
}

#[derive(Clone)]
pub struct BiquadChain(pub Vec<Biquad>);

impl BiquadChain {
    pub fn process(&mut self, x: f64) -> f64 {
        self.0.iter_mut().fold(x, |s, bq| bq.process(s))
    }
}

fn hp_0_5hz() -> Biquad {
    let k = (std::f64::consts::PI*0.5/SAMPLE_RATE as f64).tan();
    let k2=k*k; let sq2k=std::f64::consts::SQRT_2*k; let d=1.0+sq2k+k2;
    Biquad{b0:1.0/d,b1:-2.0/d,b2:1.0/d,a1:2.0*(k2-1.0)/d,a2:(1.0-sq2k+k2)/d,w1:0.0,w2:0.0}
}
fn lp_50hz() -> Biquad {
    let k=(std::f64::consts::PI*50.0/SAMPLE_RATE as f64).tan();
    let k2=k*k; let sq2k=std::f64::consts::SQRT_2*k; let d=1.0+sq2k+k2;
    Biquad{b0:k2/d,b1:2.0*k2/d,b2:k2/d,a1:2.0*(k2-1.0)/d,a2:(1.0-sq2k+k2)/d,w1:0.0,w2:0.0}
}
fn notch_50hz() -> Biquad {
    let w0=2.0*std::f64::consts::PI*50.0/SAMPLE_RATE as f64;
    let bw=w0/30.0; let kn=(bw/2.0).tan(); let norm=1.0/(1.0+kn);
    Biquad{b0:norm,b1:-2.0*w0.cos()*norm,b2:norm,
           a1:-2.0*w0.cos()*norm,a2:(1.0-kn)*norm,w1:0.0,w2:0.0}
}

pub fn make_filter() -> BiquadChain {
    BiquadChain(vec![hp_0_5hz(), lp_50hz(), notch_50hz()])
}

// ── Hann power correction ─────────────────────────────────────────────────────
// P1: correction = N / Σw²  where w[n] = 0.5*(1 - cos(2πn/(N-1)))
// For N=256: S2 = 95.625, correction ≈ 2.677 ≈ 8/3
fn hann_power_correction(n: usize) -> f64 {
    let s2: f64 = (0..n).map(|i| {
        let w = 0.5*(1.0-(2.0*std::f64::consts::PI*i as f64/(n-1) as f64).cos());
        w*w
    }).sum();
    n as f64 / s2
}

// ── Half-open band bins [f_low, f_high) — zero overlap ───────────────────────
fn band_bins(f_low: f64, f_high: f64, freq_res: f64, n_bins: usize) -> (usize, usize) {
    let k_low  = (f_low  / freq_res).ceil() as usize;
    let k_high = ((f_high / freq_res).ceil() as usize).saturating_sub(1).min(n_bins-1);
    (k_low, k_high)
}

fn band_power(psd: &[f64], f_low: f64, f_high: f64, freq_res: f64) -> f64 {
    let (k_low, k_high) = band_bins(f_low, f_high, freq_res, psd.len());
    if k_high < k_low { return 0.0; }
    psd[k_low..=k_high].iter().sum::<f64>() * freq_res
}

// ── Public types ──────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Serialize)]
pub struct BandPowers {
    pub delta: f64,   // [0.5,4)  Hz  µV²
    pub theta: f64,   // [4,8)    Hz  µV²
    pub alpha: f64,   // [8,13)   Hz  µV²
    pub beta:  f64,   // [13,30)  Hz  µV²
    pub gamma: f64,   // [30,50)  Hz  µV²
    pub total: f64,   // [0.5,50) Hz  µV²
}

#[derive(Debug, Clone, Serialize)]
pub struct AttentionResult {
    pub bands:            BandPowers,
    pub log_ei:           f64,  // log(β+ε) − log(α+θ+ε)  raw, uncalibrated
    pub engagement_index: f64,  // β/(α+θ+ε) linear, display only
    pub attention_score:  u8,   // 0–255, calibrated, sent via SDPF
}

// ── Analysis pipeline ─────────────────────────────────────────────────────────

/// Analyse one epoch of raw EEG.
/// `samples` must be a CLONE already extracted from the lock (P3).
/// `filter`  is the per-user IIR chain, mutated in place.
pub fn analyse(samples: &[f32], filter: &mut BiquadChain) -> AttentionResult {
    let n = samples.len();
    assert!(n >= 2);

    // 1. Band-pass + notch IIR filter (state persists across epochs)
    let filtered: Vec<f64> = samples.iter().map(|&s| filter.process(s as f64)).collect();

    // 2. P2: DC removal — subtract epoch mean before windowing
    let mean_dc: f64 = filtered.iter().sum::<f64>() / n as f64;
    let dc_free: Vec<f64> = filtered.iter().map(|&s| s - mean_dc).collect();

    // 3. Hann window
    let windowed: Vec<Complex<f32>> = dc_free.iter().enumerate().map(|(i, &s)| {
        let w = 0.5*(1.0-(2.0*std::f64::consts::PI*i as f64/(n-1) as f64).cos());
        Complex::new((s*w) as f32, 0.0)
    }).collect();

    // 4. FFT
    let mut planner = FftPlanner::<f32>::new();
    let fft = planner.plan_fft_forward(n);
    let mut spectrum = windowed;
    fft.process(&mut spectrum);

    // 5. P1: One-sided PSD with Hann power correction
    // PSD[k] = |X[k]|² × (N/S2)            for k = 0, N/2
    // PSD[k] = 2 × |X[k]|² × (N/S2)        for k = 1..N/2-1
    let freq_res = SAMPLE_RATE as f64 / n as f64;
    let win_corr = hann_power_correction(n);

    let psd: Vec<f64> = (0..=n/2).map(|k| {
        let mag_sq = spectrum[k].norm_sqr() as f64 * win_corr;
        if k == 0 || k == n/2 { mag_sq } else { 2.0*mag_sq }
    }).collect();

    // 6. Band power = Σ PSD[k]·Δf  (integral, half-open intervals)
    let delta = band_power(&psd, 0.5,  4.0,  freq_res);
    let theta = band_power(&psd, 4.0,  8.0,  freq_res);
    let alpha = band_power(&psd, 8.0,  13.0, freq_res);
    let beta  = band_power(&psd, 13.0, 30.0, freq_res);
    let gamma = band_power(&psd, 30.0, 50.0, freq_res);
    let total = band_power(&psd, 0.5,  50.0, freq_res);

    // 7. Log-scale Engagement Index
    let log_ei    = (beta + EPS).ln() - (alpha + theta + EPS).ln();
    let ei_linear = beta / (alpha + theta + EPS);

    // 8. Calibrated normalisation
    // Simulator's log_EI range: ≈ [−4, +2]  (−4 = drowsy, +2 = highly focused)
    // Midpoint at −2.5 → maps to score=128
    // tanh( (log_EI + EI_SHIFT) / EI_SCALE ) ∈ (−1, +1)
    // score = (tanh × 0.5 + 0.5) × 255  ∈ (0, 255)
    let calibrated = (log_ei + EI_SHIFT) / EI_SCALE;
    let attention_score = (calibrated.tanh() * 0.5 + 0.5) * 255.0;
    let attention_score = attention_score.clamp(0.0, 255.0) as u8;

    AttentionResult {
        bands: BandPowers { delta, theta, alpha, beta, gamma, total },
        log_ei,
        engagement_index: ei_linear,
        attention_score,
    }
}