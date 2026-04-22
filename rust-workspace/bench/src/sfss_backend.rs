//! SFSS histogram backend — uses your existing sfss-sys FFI.
//! Calls the real C++ SDPF_enc/SDPF_eval for each of B=5 bins per tick.
//!
//! THREAT MODEL: TRUSTED CLIENT ONLY.
//! Servers cannot verify that the client's input is a valid one-hot vector.

use std::time::Instant;
use crate::histogram::{score_to_indicators, B};

pub struct SfssResult {
    pub histogram: Vec<u32>,
    pub setup_bytes: usize,
    pub total_ctx_bytes: usize,
    pub s2s_bytes: usize,
    pub tick_times_ns: Vec<u64>,
}

pub fn run(scores: &[u8], user_id: u32) -> SfssResult {
    // One DPF key pair per user session
    let (key0, key1, mut sk0, mut sk1) = sfss_sys::gen(user_id);
    let setup_bytes = 2 * sfss_sys::SFSS_KEY_SIZE as usize;

    let mut agg0 = [0u32; B];
    let mut agg1 = [0u32; B];
    let mut total_ctx_bytes = 0usize;
    let mut tick_times_ns = Vec::with_capacity(scores.len());

    for &score in scores {
        let t0 = Instant::now();
        let indicators = score_to_indicators(score);

        // Encrypt B indicator values via B scalar SDPF_enc calls.
        // Each call invokes the REAL C++ SDPF_enc<uint32_t> from sfss.h.
        for b in 0..B {
            let ctx = sfss_sys::encrypt(&mut sk0, &mut sk1, indicators[b] as u8);
            total_ctx_bytes += sfss_sys::SFSS_CTX_SIZE as usize;

            // Leader evaluates DPF with key0 → share0
            let share0 = sfss_sys::eval(&key0, &ctx, user_id);
            // Helper evaluates DPF with key1 → share1
            let share1 = sfss_sys::eval(&key1, &ctx, user_id);

            // Accumulate shares mod 2^32
            agg0[b] = agg0[b].wrapping_add(share0);
            agg1[b] = agg1[b].wrapping_add(share1);
        }

        tick_times_ns.push(t0.elapsed().as_nanos() as u64);
    }

    // Reconstruct: share0 + share1 (mod 2^32) for each bin
    let histogram: Vec<u32> = (0..B)
        .map(|b| sfss_sys::reconstruct(agg0[b], agg1[b]))
        .collect();

    SfssResult {
        histogram, setup_bytes, total_ctx_bytes,
        s2s_bytes: 0,  // SFSS needs ZERO server-to-server communication
        tick_times_ns,
    }
}