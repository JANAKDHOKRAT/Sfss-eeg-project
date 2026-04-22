//! Multi-user parallel benchmark: Prio3Histogram vs SFSS
//!
//! Each of N users gets:
//!   - Their own EEG trace (independent RNG)
//!   - Their own SDPF key pair (SFSS: independent SDPF_gen per user)
//!   - Their own independent shard/verify calls (Prio3)
//!
//! Histograms are summed across all users.
//! The crypto in prio3_backend.rs and sfss_backend.rs is UNCHANGED.

mod eeg;
mod attention;
mod histogram;
mod prio3_backend;
mod sfss_backend;

use histogram::{B, BUCKETS, bucket_label, score_to_bucket};
use rayon::prelude::*;
use std::time::Instant;

struct Config {
    users: usize,
    ticks_per_user: usize,
    user_id_base: u32,
    chunk_length: usize,
    num_aggregators: u8,
}

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut cfg = Config {
        users: 1000,
        ticks_per_user: 100,
        user_id_base: 1000,
        chunk_length: 2,
        num_aggregators: 2,
    };
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--users"   => { i += 1; cfg.users = args[i].parse().unwrap(); }
            "--ticks"   => { i += 1; cfg.ticks_per_user = args[i].parse().unwrap(); }
            "--base-id" => { i += 1; cfg.user_id_base = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }
    cfg
}

/// Generate one user's EEG trace using the real pipeline.
/// Each user gets an independent EegSimulator (own RNG state).
fn generate_user_trace(ticks: usize) -> Vec<u8> {
    let mut sim = eeg::EegSimulator::new();
    let mut filt = attention::make_filter();
    (0..ticks).map(|_| {
        let epoch = sim.next_epoch();
        attention::analyse(&epoch.samples, &mut filt).attention_score
    }).collect()
}

fn main() {
    let cfg = parse_args();
    let total_measurements = cfg.users * cfg.ticks_per_user;

    println!("{}", "=".repeat(74));
    println!("  Multi-User EEG Histogram Benchmark");
    println!("  Prio3Histogram (libprio-rs) vs SFSS (C++ SDPF FFI)");
    println!("{}", "=".repeat(74));
    println!("Configuration:");
    println!("  Users            : {}", cfg.users);
    println!("  Ticks per user   : {}", cfg.ticks_per_user);
    println!("  Total measurements: {}", total_measurements);
    println!("  Histogram bins   : {} {:?}", B, BUCKETS);
    println!("  User ID range    : {} .. {}",
        cfg.user_id_base, cfg.user_id_base + cfg.users as u32 - 1);
    println!();

    // ═══════════════════════════════════════════════════════════════
    // Phase 1: Generate EEG traces for all users (parallel, pure Rust)
    // ═══════════════════════════════════════════════════════════════
    println!("Phase 1: Generating {} EEG traces ({} ticks each)...",
        cfg.users, cfg.ticks_per_user);
    let t_gen = Instant::now();

    // Each user gets an independent EegSimulator with its own RNG.
    // rayon parallelizes across CPU cores — no shared state.
    let user_traces: Vec<Vec<u8>> = (0..cfg.users)
        .into_par_iter()
        .map(|_| generate_user_trace(cfg.ticks_per_user))
        .collect();

    let gen_ms = t_gen.elapsed().as_millis();
    println!("  Done: {} ms ({} traces × {} ticks)", gen_ms, cfg.users, cfg.ticks_per_user);

    // Show aggregate score distribution across all users
    let all_scores: Vec<u8> = user_traces.iter().flat_map(|t| t.iter().copied()).collect();
    let mean_score = all_scores.iter().map(|&s| s as f64).sum::<f64>() / all_scores.len() as f64;
    println!("  Score range: [{}, {}], mean: {:.1}",
        all_scores.iter().min().unwrap(), all_scores.iter().max().unwrap(), mean_score);
    println!("  Ground-truth distribution (all {} measurements):", total_measurements);
    for i in 0..B {
        let c = all_scores.iter().filter(|&&s| score_to_bucket(s) == i).count();
        let pct = c as f64 / total_measurements as f64 * 100.0;
        let bar_len = (c as f64 / total_measurements as f64 * 60.0) as usize;
        println!("    {}: {:6}  ({:5.1}%)  {}", bucket_label(i), c, pct, "▓".repeat(bar_len));
    }
    println!();

    // ═══════════════════════════════════════════════════════════════
    // Phase 2: Run Prio3Histogram for all users
    //
    // Each user's scores are independently sharded, verified, and
    // aggregated. The histogram accumulates across ALL users.
    //
    // CRYPTO UNCHANGED: calls prio3_backend::run() exactly as-is.
    // ═══════════════════════════════════════════════════════════════
    println!("Phase 2: Running Prio3Histogram ({} users × {} ticks = {} measurements)...",
        cfg.users, cfg.ticks_per_user, total_measurements);

    let p3_config = prio3_backend::Prio3Config {
        num_aggregators: cfg.num_aggregators,
        chunk_length: cfg.chunk_length,
    };

    let t_p3 = Instant::now();

    // Run each user's Prio3 session independently.
    // Each call to prio3_backend::run() does the full VDAF flow:
    //   shard → prepare_init → prepare_shares_to_prepare_message
    //   → prepare_next → aggregate → unshard
    // with real Field128 arithmetic, real FLP proofs, real XofTurboShake128.
    let p3_results: Vec<prio3_backend::Prio3Result> = user_traces.iter()
        .map(|scores| prio3_backend::run(scores, &p3_config).expect("Prio3 failed"))
        .collect();

    let p3_wall_ms = t_p3.elapsed().as_millis();

    // Combine histograms: element-wise sum across all users
    let mut p3_histogram = vec![0u128; B];
    let mut p3_total_client = 0usize;
    let mut p3_total_s2s = 0usize;
    let mut p3_total_ns = 0u64;
    for r in &p3_results {
        for i in 0..B { p3_histogram[i] += r.histogram[i]; }
        p3_total_client += r.total_client_bytes;
        p3_total_s2s += r.total_s2s_bytes;
        p3_total_ns += r.tick_times_ns.iter().sum::<u64>();
    }
    let p3_crypto_ms = p3_total_ns as f64 / 1_000_000.0;
    println!("  Done: {} ms wall, {:.0} ms crypto", p3_wall_ms, p3_crypto_ms);
    println!();

    // ═══════════════════════════════════════════════════════════════
    // Phase 3: Run SFSS for all users
    //
    // Each user gets their own SDPF_gen() → own DPF key pair.
    // Each user's scores are encrypted with their own stream keys.
    // Each server evaluates with that user's server key.
    //
    // CRYPTO UNCHANGED: calls sfss_backend::run() exactly as-is.
    // ═══════════════════════════════════════════════════════════════
    println!("Phase 3: Running SFSS ({} users × {} ticks = {} measurements)...",
        cfg.users, cfg.ticks_per_user, total_measurements);

    let t_sf = Instant::now();

    // Each user gets a unique user_id.
    // sfss_backend::run() calls sfss_sys::gen(user_id) which invokes
    // the real SDPF_gen<32> — each user gets independent DPF keys,
    // independent stream keys, independent PRF counters.
    let sf_results: Vec<sfss_backend::SfssResult> = user_traces.iter()
        .enumerate()
        .map(|(i, scores)| {
            let uid = cfg.user_id_base + i as u32;
            sfss_backend::run(scores, uid)
        })
        .collect();

    let sf_wall_ms = t_sf.elapsed().as_millis();

    // Combine histograms across all users
    let mut sf_histogram = vec![0u32; B];
    let mut sf_total_setup = 0usize;
    let mut sf_total_ctx = 0usize;
    let mut sf_total_ns = 0u64;
    for r in &sf_results {
        for i in 0..B { sf_histogram[i] += r.histogram[i]; }
        sf_total_setup += r.setup_bytes;
        sf_total_ctx += r.total_ctx_bytes;
        sf_total_ns += r.tick_times_ns.iter().sum::<u64>();
    }
    let sf_crypto_ms = sf_total_ns as f64 / 1_000_000.0;
    println!("  Done: {} ms wall, {:.0} ms crypto", sf_wall_ms, sf_crypto_ms);
    println!();

    // ═══════════════════════════════════════════════════════════════
    // Phase 4: Compare results
    // ═══════════════════════════════════════════════════════════════
    let p3_hist_u32: Vec<u32> = p3_histogram.iter().map(|&v| v as u32).collect();
    let match_ok = p3_hist_u32 == sf_histogram;

    println!("{}", "=".repeat(74));
    println!("RESULTS ({} users × {} ticks = {} total measurements)",
        cfg.users, cfg.ticks_per_user, total_measurements);
    println!("{}", "=".repeat(74));
    println!();

    println!("  Histograms match: {}", if match_ok { "YES ✓" } else { "NO ✗" });
    println!();
    println!("  {:12} {:>10} {:>10}", "Bucket", "Prio3", "SFSS");
    println!("  {}", "-".repeat(36));
    for i in 0..B {
        let ok = if p3_hist_u32[i] == sf_histogram[i] { "✓" } else { "✗" };
        println!("  {:12} {:>10} {:>10}  {}", bucket_label(i), p3_hist_u32[i], sf_histogram[i], ok);
    }
    println!("  {:12} {:>10} {:>10}", "TOTAL",
        p3_hist_u32.iter().sum::<u32>(), sf_histogram.iter().sum::<u32>());
    println!();

    // ── Per-user metrics ──────────────────────────────────────────
    let p3_per_user_bytes = (p3_total_client + p3_total_s2s) as f64 / cfg.users as f64;
    let sf_per_user_bytes = (sf_total_setup + sf_total_ctx) as f64 / cfg.users as f64;
    let p3_per_tick_ms = p3_crypto_ms / total_measurements as f64;
    let sf_per_tick_ms = sf_crypto_ms / total_measurements as f64;

    println!("Per-user communication ({} ticks/user):", cfg.ticks_per_user);
    println!("  Prio3: {:.0} B/user ({} client + {} s2s / {} users)",
        p3_per_user_bytes, p3_total_client, p3_total_s2s, cfg.users);
    println!("  SFSS:  {:.0} B/user ({} setup + {} ctx / {} users)",
        sf_per_user_bytes, sf_total_setup, sf_total_ctx, cfg.users);
    println!();

    println!("Total communication ({} users combined):", cfg.users);
    let p3_total = p3_total_client + p3_total_s2s;
    let sf_total = sf_total_setup + sf_total_ctx;
    println!("  Prio3 total : {:>12} B  ({:.2} MB)", p3_total, p3_total as f64 / 1_048_576.0);
    println!("  SFSS  total : {:>12} B  ({:.2} MB)", sf_total, sf_total as f64 / 1_048_576.0);
    if sf_total > 0 && p3_total > sf_total {
        println!("  SFSS saving : {:.1}%",
            (p3_total as f64 - sf_total as f64) / p3_total as f64 * 100.0);
    }
    println!();

    println!("Timing ({} total measurements):", total_measurements);
    println!("  Prio3: {:.0} ms total, {:.4} ms/tick", p3_crypto_ms, p3_per_tick_ms);
    println!("  SFSS:  {:.0} ms total, {:.4} ms/tick", sf_crypto_ms, sf_per_tick_ms);
    println!("  SFSS speedup: {:.0}×",
        if sf_per_tick_ms > 0.0 { p3_per_tick_ms / sf_per_tick_ms } else { 0.0 });
    println!();

    println!("Scalability:");
    println!("  SFSS setup cost : {} B × {} users = {} B ({:.2} MB)",
        2 * sfss_sys::SFSS_KEY_SIZE, cfg.users,
        sf_total_setup, sf_total_setup as f64 / 1_048_576.0);
    println!("  SFSS amortized  : {:.1} B/tick/user (setup amortized over {} ticks)",
        sf_per_user_bytes / cfg.ticks_per_user as f64, cfg.ticks_per_user);
    println!("  Prio3 per tick  : {} B/tick/user (no amortization)",
        (p3_total_client + p3_total_s2s) / total_measurements);
    println!();

    println!("Threat model:");
    println!("  Prio3: VERIFIABLE — FLP proof rejects invalid inputs.");
    println!("         Each of {} users' measurements independently verified.", cfg.users);
    println!("  SFSS:  TRUSTED CLIENT — each of {} users has own DPF key pair.", cfg.users);
    println!("         {} independent SDPF_gen<32> calls (own AES keys).", cfg.users);
    println!();

    // Sanity check: histogram totals must equal total measurements
    let p3_sum: u32 = p3_hist_u32.iter().sum();
    let sf_sum: u32 = sf_histogram.iter().sum();
    assert_eq!(p3_sum as usize, total_measurements,
        "Prio3 histogram total {} ≠ expected {}", p3_sum, total_measurements);
    assert_eq!(sf_sum as usize, total_measurements,
        "SFSS histogram total {} ≠ expected {}", sf_sum, total_measurements);

    if !match_ok {
        eprintln!("ERROR: Histograms do not match!");
        std::process::exit(1);
    }

    println!("✓ All {} measurements verified. Histograms match across {} users.",
        total_measurements, cfg.users);
}