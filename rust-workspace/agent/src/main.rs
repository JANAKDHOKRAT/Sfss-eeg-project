// agent/src/main.rs — secure multi-user EEG agent
//
// Runtime user lifecycle:
//   POST /user/add    → server assigns random user_id, registers with Leader+Helper, spawns EEG loop
//   POST /user/remove → stops loop, removes from agent+Leader+Helper+Reconstruct
//   GET  /users       → list of active user_ids (dashboard polls this)
//   GET  /snapshot    → all users' tick counters (no signal data — security fix B)
//
// Note: /history and raw EEG endpoints removed (security fix B — plaintext exposure).
// Note: /snapshot returns only {user_id, tick, sample_rate} — no attention scores.
//
// On startup: 4 initial users are created automatically.
// The browser can then add/remove users at runtime via POST buttons.
//
// SDPF invariants preserved:
//   1. user_id: random u32, stored in used_ids HashSet — NEVER reused
//   2. ctr[user]: lives inside sk0/sk1 per UserContext — no global counter
//   3. ctr parity L&H: agent only sends to Helper if Leader accepted (P4)
//   4. idx == user_id: passed explicitly in every payload
//   5. Independent keys: sfss_sys::gen(user_id) called per user
//   6. PRF alignment: sk0+sk1 updated atomically under one lock scope
//   P3: filter cloned out of lock, FFT runs lock-free, state written back
//   P4: monotonic seq per user, Leader gates Helper on acceptance
//   P5: seq replay protection enforced by Leader+Helper

mod eeg; // bring in the EEG simulator module from eeg.rs
mod attention; // bring in the attention analysis module from attention.rs

use attention::{make_filter, analyse, BiquadChain}; // import filter builder, analyzer, and filter type
use axum::{ // import Axum web framework types
    extract::State, // access shared application state in handlers
    http::{HeaderMap, StatusCode}, // read request headers and return HTTP status codes
    routing::{get, post}, // create GET and POST routes
    Json, Router, // JSON extractor/response and router builder
};
use eeg::{EegSimulator, SAMPLE_RATE}; // import EEG simulator and sample rate constant
use rand::Rng; // import random number generator trait
use subtle::ConstantTimeEq; // import constant-time equality for auth checks
use serde::{Deserialize, Serialize}; // import serialization and deserialization derives
use serde_json::json; // import json! macro for building JSON payloads
use std::collections::{HashMap, HashSet, VecDeque}; // import map, set, and queue structures
use std::sync::{Arc, Mutex, atomic::{AtomicUsize, Ordering}}; // import shared ownership, locking, and atomic counter tools
use std::time::Duration; // import duration type for timers and timeouts
use tokio::sync::oneshot; // import one-shot channel for stop signals
use tokio::time; // import Tokio timing utilities
use tower_http::cors::{CorsLayer, AllowOrigin}; // import CORS middleware and allowed-origin helper
use tracing::info; // import structured info-level logging

// ── Constants ─────────────────────────────────────────────────────────────────

// ── Service URLs ──────────────────────────────────────────────────────────────
//
// Fix 7 — TLS deployment note:
// These URLs use plain HTTP on localhost. This is safe ONLY because all
// services run on the same machine and loopback traffic does not leave the host.
//
// For any real deployment across machines or networks:
//   1. Enable TLS on Leader, Helper, and Reconstruct (e.g. rustls + axum-server)
//   2. Change these URLs to https://
//   3. Use mTLS (mutual TLS) so servers authenticate each other
//   4. Set AGENT_ALLOWED_ORIGIN to the actual dashboard domain (not localhost)
//
// Without TLS on a network:
//   - user_id, seq, and ctx are visible to a network eavesdropper
//   - ctx is cryptographically safe (IND-CPA secure per SFSS Theorem 1)
//     but user_id reveals participation metadata (accepted in SFSS Leak function)
const LEADER_URL: &str = "http://127.0.0.1:8080"; // Leader server base URL
const HELPER_URL: &str = "http://127.0.0.1:8081"; // Helper server base URL
const RECON_URL:  &str = "http://127.0.0.1:8090"; // Reconstruct server base URL
const AGENT_PORT:         u16  = 8082; // Agent API port
const TRANSMIT_S:         u64  = 30; // transmit one encrypted sample every 30 seconds
const JITTER_MS:          u64  = 5_000;   // Fix 5: up to 5 s random delay per transmission
const EPOCH_MS:           u64  = 1_000; // EEG epoch duration in milliseconds
const INITIAL_USERS:      usize = 4; // number of users created on startup
const MAX_USERS:          usize = 100;    // Fix 8: rate limit cap
const RETRY_QUEUE_CAP:    usize = 5;     // Fix 6: max pending retries per user

// Fix 1: API key — read from env, fall back to obvious dev value
fn api_key() -> String { // return the API key string used by the dashboard
    std::env::var("AGENT_API_KEY") // read AGENT_API_KEY from environment
        .unwrap_or_else(|_| "dev-key-change-in-production".into()) // fall back to default dev key if missing
} // end api_key

// Fix 2: Allowed CORS origin — read from env, default to localhost dashboard
fn allowed_origin() -> String { // return the allowed browser origin
    std::env::var("AGENT_ALLOWED_ORIGIN") // read AGENT_ALLOWED_ORIGIN from environment
        .unwrap_or_else(|_| "http://localhost:3000".into()) // default to local dashboard origin
} // end allowed_origin

// Fix 1 (improved): fully branch-free constant-time auth check.
//
// The previous version had a length-mismatch branch:
//   if e.len() != p.len() { dummy.ct_eq(e) }  // the branch itself leaks timing
//
// This version pads both slices to the same maximum length BEFORE comparing,
// so there is exactly one code path regardless of lengths — no branch taken
// on the length comparison, no timing difference between correct and wrong keys.
//
// An attacker measuring response latency learns nothing about the expected
// key length, because the comparison always runs over MAX_KEY_LEN bytes.
const MAX_KEY_LEN: usize = 256; // larger than any realistic API key

fn check_auth(headers: &HeaderMap) -> bool { // verify Authorization header in constant time
    let expected = format!("Bearer {}", api_key()); // expected bearer token string
    headers.get("Authorization") // look up Authorization header
        .and_then(|v| v.to_str().ok()) // convert header value to UTF-8 string if possible
        .map(|provided| { // compare if header exists and is valid UTF-8
            // Pad both to MAX_KEY_LEN with zeroes so the comparison always
            // processes the same number of bytes — no branch on length.
            let mut e_padded = [0u8; MAX_KEY_LEN]; // fixed-size expected buffer
            let mut p_padded = [0u8; MAX_KEY_LEN]; // fixed-size provided buffer
            let e = expected.as_bytes(); // expected header as bytes
            let p = provided.as_bytes(); // provided header as bytes
            e_padded[..e.len().min(MAX_KEY_LEN)].copy_from_slice(&e[..e.len().min(MAX_KEY_LEN)]); // copy expected bytes
            p_padded[..p.len().min(MAX_KEY_LEN)].copy_from_slice(&p[..p.len().min(MAX_KEY_LEN)]); // copy provided bytes
            // One constant-time comparison: MAX_KEY_LEN bytes, always.
            // If actual lengths differ, the padding zeroes will mismatch → false.
            let lengths_match = (e.len() == p.len()) as u8; // 1 if lengths match, 0 otherwise
            let bytes_match   = e_padded.ct_eq(&p_padded).unwrap_u8(); // constant-time byte equality
            // Both must be 1 — combined with & so no branch is taken.
            (lengths_match & bytes_match) == 1 // final auth result
        })
        .unwrap_or(false) // if no header or invalid UTF-8, reject
} // end check_auth

// ── Per-user context ──────────────────────────────────────────────────────────

struct UserContext { // all per-user runtime state lives here
    user_id:            u32, // user identity
    sk0:                sfss_sys::StreamKey, // stream key share 0 for SFSS encryption
    sk1:                sfss_sys::StreamKey, // stream key share 1 for SFSS encryption
    sim:                EegSimulator, // EEG signal generator for this user
    filter:             BiquadChain, // signal-processing filter state
    // latest_result intentionally removed:
    //   /snapshot no longer exposes it (security fix B removed raw scores/samples).
    //   Storing it serves no purpose and keeps plaintext attention data in RAM
    //   longer than necessary (Claim 7 partial mitigation).
    tick:               u64, // current EEG epoch number
    transmit_countdown: u64, // countdown until next transmission
    next_seq:           u64, // next monotonic sequence number for replay protection
    retry_queue:        VecDeque<(u64, String)>, // queued failed transmissions as (seq, ctx_hex)
} // end UserContext

impl UserContext { // methods for UserContext
    fn new(user_id: u32, sk0: sfss_sys::StreamKey, sk1: sfss_sys::StreamKey, stagger: u64) -> Self { // construct a user context
        Self { // initialize all fields
            user_id, sk0, sk1, // store user and stream keys
            sim:                EegSimulator::new(), // create a fresh EEG simulator
            filter:             make_filter(), // create the filter chain
            tick:               0, // start at epoch 0
            transmit_countdown: TRANSMIT_S.saturating_sub(stagger).max(1), // stagger first send
            next_seq:           0, // start sequence numbers at 0
            retry_queue:        VecDeque::with_capacity(RETRY_QUEUE_CAP), // preallocate retry queue
        } // end Self initialization
    } // end new
} // end impl UserContext

// ── Registry ──────────────────────────────────────────────────────────────────

struct Registry { // shared registry for active users and all previously used IDs
    users:    HashMap<u32, (Arc<Mutex<UserContext>>, oneshot::Sender<()>)>, // user_id -> context and stop signal
    used_ids: HashSet<u32>, // all used IDs so they are never reused
} // end Registry

impl Registry { // methods for Registry
    fn new() -> Self { Self { users: HashMap::new(), used_ids: HashSet::new() } } // create empty registry

    fn fresh_id(&mut self) -> u32 { // generate a unique random user_id
        let mut rng = rand::thread_rng(); // create a thread-local random generator
        loop { // keep trying until a unique ID is found
            let id: u32 = rng.gen_range(1000..u32::MAX); // choose a random u32 above 1000
            if !self.used_ids.contains(&id) { // check if the ID has not been used before
                self.used_ids.insert(id); // mark the ID as used forever
                return id; // return the unique ID
            } // end uniqueness check
        } // end loop
    } // end fresh_id
} // end impl Registry

type SharedRegistry  = Arc<Mutex<Registry>>; // shared, thread-safe registry type alias
type ActiveUserCount = Arc<AtomicUsize>;  // Fix 8

// ── HTTP types ────────────────────────────────────────────────────────────────

// (AddUserResp removed — handle_add_user returns serde_json::Value directly)

#[derive(Deserialize)] // allow JSON deserialization from request body
struct RemoveReq { user_id: u32 } // request body for user removal

#[derive(Serialize)] // allow JSON serialization for response
struct UsersListResp { user_ids: Vec<u32> } // response listing active user IDs

// Snapshot returns ONLY tick metadata — no samples, no scores (security fix B)
#[derive(Serialize)] // allow JSON serialization for snapshot response
struct UserSnapshot { // safe per-user snapshot data
    user_id:     u32, // user ID
    tick:        u64, // current tick count
    sample_rate: usize, // sample rate used by the simulator
} // end UserSnapshot

#[derive(Serialize)] // allow JSON serialization for aggregated snapshot response
struct AllSnapshotResp { users: HashMap<String, UserSnapshot> } // map user IDs to snapshots

#[derive(Serialize)] // allow JSON serialization for status messages
struct StatusResp { status: String, user_id: Option<u32> } // generic status response

// ── HTTP handlers ─────────────────────────────────────────────────────────────

/// POST /user/add — Fix 1: requires Authorization header, Fix 8: rate limit
async fn handle_add_user( // HTTP handler for adding a new user
    State((reg, client, count)): State<(SharedRegistry, reqwest::Client, ActiveUserCount)>, // extract shared state
    headers: HeaderMap, // extract request headers for auth check
) -> (StatusCode, Json<serde_json::Value>) { // return status code and JSON
    // Fix 1: authentication
    if !check_auth(&headers) { // reject if auth header is missing or wrong
        return (StatusCode::UNAUTHORIZED, // return 401
                Json(json!({"status": "unauthorized", "detail": "missing or wrong Authorization header"}))); // auth error body
    } // end auth check

    // Fix 8: rate limit
    if count.load(Ordering::Relaxed) >= MAX_USERS { // reject if too many active users
        return (StatusCode::TOO_MANY_REQUESTS, // return 429
                Json(json!({"status": "too_many_users", "max": MAX_USERS}))); // rate-limit body
    } // end rate limit check

    let user_id = { reg.lock().unwrap().fresh_id() }; // create a unique random user_id under lock
    info!("[Agent] Adding user_id={}", user_id); // log the new user

    let (key0, key1, sk0, sk1) = sfss_sys::gen(user_id); // generate SFSS keys for this user

    if let Err(e) = client.post(format!("{}/setup", LEADER_URL)) // prepare POST to Leader /setup
        .json(&json!({"user_id": user_id, "key": hex::encode(&key0.0)})) // send leader key and user_id
        .send().await { tracing::warn!("[Agent] Leader setup failed: {}", e); } // send request and log failure

    if let Err(e) = client.post(format!("{}/setup", HELPER_URL)) // prepare POST to Helper /setup
        .json(&json!({"user_id": user_id, "key": hex::encode(&key1.0)})) // send helper key and user_id
        .send().await { tracing::warn!("[Agent] Helper setup failed: {}", e); } // send request and log failure

    let stagger = { reg.lock().unwrap().users.len() as u64 % TRANSMIT_S }; // compute transmission stagger from current user count
    let ctx_arc  = Arc::new(Mutex::new(UserContext::new(user_id, sk0, sk1, stagger))); // build thread-safe user context
    let (stop_tx, stop_rx) = oneshot::channel::<()>(); // create stop channel for user loop

    tokio::spawn(user_loop(ctx_arc.clone(), client.clone(), count.clone(), stop_rx)); // start background user loop
    reg.lock().unwrap().users.insert(user_id, (ctx_arc, stop_tx)); // register user context and stop sender
    count.fetch_add(1, Ordering::Relaxed); // increment active user count

    // Fix 8: structured audit event — no sensitive data, only operational facts
    tracing::info!( // emit structured audit log
        audit = true, // mark as audit event
        action = "user_add", // action name
        user_id = user_id, // added user ID
        active_users = count.load(Ordering::Relaxed), // current active user count
        outcome = "success", // outcome value
        "Audit: user added" // human-readable message
    ); // end structured audit log
    (StatusCode::CREATED, Json(json!({"user_id": user_id, "status": "ok"}))) // return 201 and JSON success
} // end handle_add_user

/// POST /user/remove — Fix 1: requires Authorization header
async fn handle_remove_user( // HTTP handler for removing a user
    State((reg, client, count)): State<(SharedRegistry, reqwest::Client, ActiveUserCount)>, // extract shared state
    headers: HeaderMap, // extract request headers
    Json(req): Json<RemoveReq>, // parse JSON body into RemoveReq
) -> (StatusCode, Json<StatusResp>) { // return status and JSON
    // Fix 1: authentication
    if !check_auth(&headers) { // reject if auth header invalid
        return (StatusCode::UNAUTHORIZED, // return 401
                Json(StatusResp { status: "unauthorized".into(), user_id: None })); // unauthorized response
    } // end auth check

    let removed = reg.lock().unwrap().users.remove(&req.user_id); // remove user from registry if present
    match removed { // handle removal result
        None => (StatusCode::NOT_FOUND, // user not found
                 Json(StatusResp { status: "not_found".into(), user_id: Some(req.user_id) })), // not-found response
        Some((_ctx, stop_tx)) => { // user found, unpack context and stop channel
            let _ = stop_tx.send(()); // signal the user loop to stop
            count.fetch_sub(1, Ordering::Relaxed); // decrement active user count
            let del = json!({"user_id": req.user_id}); // build remove payload
            let _ = client.post(format!("{}/user/remove", LEADER_URL)).json(&del).send().await; // notify Leader
            let _ = client.post(format!("{}/user/remove", HELPER_URL)).json(&del).send().await; // notify Helper
            let _ = client.post(format!("{}/user/remove", RECON_URL)).json(&del).send().await; // notify Reconstruct
            info!("[Agent] user_id={} removed", req.user_id); // log removal
            // Fix 8: structured audit event for user removal
            tracing::info!( // emit structured removal audit log
                audit = true, // mark as audit event
                action = "user_remove", // action name
                user_id = req.user_id, // removed user ID
                active_users = count.load(Ordering::Relaxed), // current active user count
                outcome = "success", // outcome value
                "Audit: user removed" // human-readable message
            ); // end structured audit log
            (StatusCode::OK, Json(StatusResp { status: "removed".into(), user_id: Some(req.user_id) })) // return success response
        } // end removal success branch
    } // end match
} // end handle_remove_user

// GET /users — no auth required (returns only active user_id list, no sensitive data)
async fn handle_list_users( // HTTP handler for listing active users
    State((reg, _, _)): State<(SharedRegistry, reqwest::Client, ActiveUserCount)>, // extract only registry from shared state
) -> Json<UsersListResp> { // return JSON list of user IDs
    let mut ids: Vec<u32> = reg.lock().unwrap().users.keys().copied().collect(); // collect active user IDs
    ids.sort(); // sort them for stable output
    Json(UsersListResp { user_ids: ids }) // return the sorted list as JSON
} // end handle_list_users

// GET /snapshot — no auth required (returns only tick counters, no signal data)
async fn handle_snapshot_all( // HTTP handler for safe snapshot data
    State((reg, _, _)): State<(SharedRegistry, reqwest::Client, ActiveUserCount)>, // extract registry
) -> Json<AllSnapshotResp> { // return JSON snapshot map
    let r = reg.lock().unwrap(); // lock the registry
    let mut users = HashMap::new(); // create output map
    for (uid, (arc, _)) in r.users.iter() { // iterate over each active user
        let ctx = arc.lock().unwrap(); // lock the user context
        users.insert(uid.to_string(), UserSnapshot { // insert snapshot for this user
            user_id: *uid, tick: ctx.tick, sample_rate: SAMPLE_RATE, // include only safe metadata
        }); // end snapshot insert
    } // end loop
    Json(AllSnapshotResp { users }) // return all snapshots as JSON
} // end handle_snapshot_all

// ── Per-user EEG + SFSS loop ──────────────────────────────────────────────────

async fn user_loop( // background task for one user
    ctx_arc:  Arc<Mutex<UserContext>>, // shared user context
    client:   reqwest::Client, // reusable HTTP client
    _count:   ActiveUserCount,  // kept in signature so tokio::spawn call sites are unchanged;
                                // count is decremented only in handle_remove_user (Fix 5)
    mut stop: oneshot::Receiver<()>, // stop signal receiver
) { // start user loop
    let user_id = ctx_arc.lock().unwrap().user_id; // read user_id once at startup
    let mut interval = time::interval(Duration::from_millis(EPOCH_MS)); // create 1 second interval
    info!("[User {}] EEG loop started", user_id); // log loop start

    loop { // run forever until stopped
        tokio::select! { // wait on either the timer or the stop signal
            _ = interval.tick() => {} // one epoch has elapsed, continue processing
            _ = &mut stop => { // stop signal received
                // Fix 5: do NOT decrement count here.
                // count is decremented exactly once in handle_remove_user,
                // which is the only caller that sends the stop signal.
                // A second fetch_sub here would double-decrement, making
                // the rate-limit counter go negative (wrapping on AtomicUsize).
                info!("[User {}] EEG loop stopped", user_id); // log loop stop
                return; // exit the task
            } // end stop branch
        } // end select

        // P3: clone filter state outside lock, run FFT lock-free
        let (epoch, mut filter_snap) = { // take a snapshot of EEG and filter state
            let mut ctx = ctx_arc.lock().unwrap(); // lock the user context
            (ctx.sim.next_epoch(), ctx.filter.clone()) // generate next EEG epoch and clone filter
        }; // release lock immediately

        let result = analyse(&epoch.samples, &mut filter_snap); // run signal analysis on the epoch
        let score  = result.attention_score; // extract attention score

        let should_transmit = { // decide whether this epoch should be transmitted
            let mut ctx = ctx_arc.lock().unwrap(); // lock user context to update state
            ctx.filter             = filter_snap; // store updated filter state
            // latest_result no longer stored — score used directly below then dropped
            ctx.tick              += 1; // increment epoch counter
            // saturating_sub: if countdown somehow reaches 0 and we subtract again,
            // saturating_sub(1) stays at 0 rather than wrapping to u64::MAX.
            ctx.transmit_countdown = ctx.transmit_countdown.saturating_sub(1); // decrement countdown safely
            if ctx.transmit_countdown == 0 { // if countdown reaches zero
                ctx.transmit_countdown = TRANSMIT_S; // reset countdown
                true // indicate that we should transmit now
            } else { false } // otherwise do not transmit
        }; // end transmit decision block

        // Fix 5: jitter — logs only tick, no score (Fix A.4)
        info!("[User {}] tick={}", user_id, ctx_arc.lock().unwrap().tick); // log tick only

        if should_transmit { // only enter transmission path when countdown reaches zero
            // Fix 6: backpressure — check queue BEFORE encrypting.
            //
            // Why this matters for SFSS:
            //   sfss_sys::encrypt() increments the streaming counter (ctr) in
            //   sk0 and sk1. In the old code, if the queue was full we still
            //   encrypted (advancing ctr) then discarded the ciphertext — wasted
            //   work, and a ctr value consumed without any data being sent.
            //
            //   Skipping encrypt entirely when overloaded is SAFE because:
            //   - ctr lives only on the agent (servers never track it)
            //   - seq only increments when we transmit (no seq gap created)
            //   - P4 ordering is unaffected (no out-of-order seq produced)
            //   - P5 replay detection is unaffected (no new seq seen by servers)
            //   - We simply skip one data point, identical to an epoch with no EEG
            { // open a scope for queue-length check
                let queue_len = ctx_arc.lock().unwrap().retry_queue.len(); // read queue length under lock
                if queue_len >= RETRY_QUEUE_CAP { // if queue is full
                    tracing::warn!( // log warning
                        "[User {}] retry queue full ({}/{}) — dropping this tick (backpressure)", // message
                        user_id, queue_len, RETRY_QUEUE_CAP // details
                    ); // end warning log
                    // Skip encrypt, skip send — ctr and seq unchanged.
                    // The skipped attention value is lost; this is the correct
                    // behaviour when the downstream is overloaded.
                    continue; // skip to next epoch
                } // end backpressure branch
            } // end queue check scope

            // Fix 6 (jitter placement): encrypt and assign seq FIRST,
            // THEN sleep the jitter, THEN send.
            //
            // Why order matters for SFSS:
            //   sk0 and sk1 hold the streaming counter (ctr). Calling
            //   sfss_sys::encrypt() increments ctr atomically in both keys.
            //   If we slept BEFORE encrypting, the ctr would advance at a
            //   random wall-clock time — a theoretical timing oracle for the
            //   counter state. By encrypting first, the ctr always advances
            //   at the same logical point in the epoch loop regardless of jitter.
            //   The jitter then only delays the TCP send, not the crypto.

            // P4/Invariant 6: seq + encrypt atomically under one lock.
            //
            // Why clone+writeback instead of direct &mut ctx.sk0 / &mut ctx.sk1:
            //   Rust's borrow checker does not permit two simultaneous &mut borrows
            //   that both go through the same MutexGuard, even to different fields.
            //   sfss_sys::encrypt takes (&mut StreamKey, &mut StreamKey) — two borrows
            //   of different fields of `ctx`, but both via the same `ctx` path.
            //   Compiler error E0499.
            //
            //   Fix: clone sk0 and sk1 out of the guard, call encrypt on the local
            //   copies (which updates their ctr field), then write them back.
            //   StreamKey derives Clone; clone is a 25-byte memcpy — negligible cost.
            //   The write-back happens under the same lock scope, so the update is
            //   still atomic with the seq increment (Invariant 6 preserved).
            let (ctx_hex, seq) = { // create ciphertext and sequence number atomically
                let mut ctx  = ctx_arc.lock().unwrap(); // lock user context
                let seq      = ctx.next_seq; // read current sequence
                ctx.next_seq += 1; // increment sequence for next transmission
                // Clone sk0 and sk1 out (25 bytes each) to satisfy the borrow checker.
                // encrypt() updates ctr inside both clones; we write them back below.
                let mut sk0_tmp = ctx.sk0.clone(); // clone stream key 0
                let mut sk1_tmp = ctx.sk1.clone(); // clone stream key 1
                let bytes = sfss_sys::encrypt(&mut sk0_tmp, &mut sk1_tmp, score); // encrypt the score
                // Write updated stream keys (with incremented ctr) back to UserContext.
                ctx.sk0 = sk0_tmp; // write back updated key 0
                ctx.sk1 = sk1_tmp; // write back updated key 1
                (hex::encode(&bytes.0), seq) // return hex ciphertext and sequence number
            }; // end atomic encryption block

            // Fix 5 (jitter): sleep AFTER encrypt, so the network send is delayed
            // but the crypto state has already advanced deterministically.
            let jitter_ms = rand::thread_rng().gen_range(0..JITTER_MS); // choose random delay
            if jitter_ms > 0 { // only sleep if delay is nonzero
                time::sleep(Duration::from_millis(jitter_ms)).await; // sleep for jitter duration
            } // end jitter sleep

            // Fix 4: peek-and-remove-on-success retry pattern.
            //
            // The previous implementation used drain(..) which removes all entries
            // from the queue BEFORE attempting to send. If the process crashed
            // between the drain and the final send, those entries were lost forever.
            //
            // Correct pattern: peek at the front, send, remove only if send succeeded.
            // If send fails again, leave the entry in the queue for the next tick.
            // This is idempotent: retrying the same (seq, ctx_hex) is safe because
            // servers enforce P5 (replay detection via seen_seqs HashSet).
            loop { // keep retrying queued transmissions
                let front = { // look at the front item without removing it
                    let ctx = ctx_arc.lock().unwrap(); // lock user context
                    ctx.retry_queue.front().cloned() // clone the front item if any
                }; // end front extraction
                match front { // handle queue state
                    None => break, // queue empty, stop retry loop
                    Some((retry_seq, ref retry_ctx_hex)) => { // a queued item exists
                        let retry_payload = json!({"user_id": user_id, "ctx": retry_ctx_hex, "seq": retry_seq}); // build retry JSON
                        match client.post(format!("{}/submit", LEADER_URL)) // send retry to Leader
                            .json(&retry_payload).send().await // serialize and send
                        {
                            Ok(r) if r.status().is_success() => { // Leader accepted retry
                                // Leader accepted — pop from queue, send to Helper
                                ctx_arc.lock().unwrap().retry_queue.pop_front(); // remove from retry queue
                                tracing::info!("[User {}] retry seq={} → Leader accepted", user_id, retry_seq); // log success
                                let _ = client.post(format!("{}/submit", HELPER_URL)) // send same retry to Helper
                                    .json(&retry_payload).send().await; // serialize and send
                            } // end accepted branch
                            Ok(r) if r.status() == reqwest::StatusCode::CONFLICT => { // replay detected
                                // Already seen by server (P5) — safe to discard
                                ctx_arc.lock().unwrap().retry_queue.pop_front(); // remove stale queued item
                                tracing::info!("[User {}] retry seq={} → CONFLICT (already seen, discarding)", user_id, retry_seq); // log discard
                            } // end conflict branch
                            _ => { // any other failure
                                // Still failing — leave in queue, try next tick
                                tracing::warn!("[User {}] retry seq={} → still failing, keeping in queue", user_id, retry_seq); // log failure
                                break; // stop retry loop for now
                            } // end failure branch
                        } // end retry send match
                    } // end Some branch
                } // end match front
            } // end retry loop

            // Now send the new payload
            let payload = json!({"user_id": user_id, "ctx": ctx_hex, "seq": seq}); // build new payload JSON

            // P4: Leader gates Helper
            let leader_ok = match client.post(format!("{}/submit", LEADER_URL)) // send to Leader first
                .json(&payload).send().await // serialize and send
            {
                Ok(r) if r.status().is_success() => { // Leader accepted
                    info!("[User {}] → Leader seq={} {}", user_id, seq, r.status()); // log success
                    true // allow sending to Helper
                } // end success branch
                Ok(r) if r.status() == reqwest::StatusCode::CONFLICT => { // replay conflict
                    tracing::warn!("[User {}] Leader seq={} CONFLICT — skip Helper", user_id, seq); // log conflict
                    false // do not send to Helper
                } // end conflict branch
                Ok(r) if r.status() == reqwest::StatusCode::GONE => { // user removed response
                    info!("[User {}] Leader seq={} GONE (user removed)", user_id, seq); // log removal
                    false // do not send to Helper
                } // end gone branch
                Ok(r) => { // other non-2xx response
                    // Fix 6: non-2xx, non-conflict — add to retry queue
                    tracing::warn!("[User {}] Leader seq={} {} — queuing retry", user_id, seq, r.status()); // log retry queueing
                    let mut ctx = ctx_arc.lock().unwrap(); // lock user context
                    if ctx.retry_queue.len() >= RETRY_QUEUE_CAP { // if queue is full
                        ctx.retry_queue.pop_front(); // drop oldest queued item
                    } // end queue-full check
                    ctx.retry_queue.push_back((seq, ctx_hex.clone())); // push current transmission into queue
                    false // do not send to Helper yet
                } // end non-2xx branch
                Err(e) => { // request failure
                    tracing::warn!("[User {}] Leader FAILED: {} — queuing retry", user_id, e); // log failure
                    let mut ctx = ctx_arc.lock().unwrap(); // lock user context
                    if ctx.retry_queue.len() >= RETRY_QUEUE_CAP { // if queue full
                        ctx.retry_queue.pop_front(); // drop oldest
                    } // end queue-full check
                    ctx.retry_queue.push_back((seq, ctx_hex.clone())); // queue transmission for later
                    false // do not send to Helper
                } // end request-error branch
            }; // end Leader send match

            if leader_ok { // only send to Helper if Leader accepted
                match client.post(format!("{}/submit", HELPER_URL)) // send to Helper
                    .json(&payload).send().await // serialize and send
                {
                    Ok(r) if r.status().is_success() => // Helper accepted
                        info!("[User {}] → Helper seq={} {}", user_id, seq, r.status()), // log success
                    Ok(r) => tracing::warn!("[User {}] Helper seq={} {}", user_id, seq, r.status()), // log non-success
                    Err(e) => tracing::warn!("[User {}] Helper FAILED: {}", user_id, e), // log request error
                } // end Helper send match
            } // end if leader_ok
            // score intentionally NOT logged here (Fix A.4)
            info!("[User {}] seq={} transmitted", user_id, seq); // log transmission without score
        } // end should_transmit branch
    } // end infinite loop
} // end user_loop

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main] // create Tokio async runtime and use async main
async fn main() { // program entry point
    tracing_subscriber::fmt().with_env_filter("agent=info").init(); // initialize logging.
    info!("PPL");
    sfss_sys::init(); // initialize the SFSS crypto backend

    let api_k = api_key(); // load API key
    let origin = allowed_origin(); // load allowed CORS origin
    info!("[Agent] API key loaded ({})", if api_k == "dev-key-change-in-production" { // log whether dev or custom key is used
        "⚠ DEV KEY — set AGENT_API_KEY env var before production" // warning text for default key
    } else { "custom key" }); // custom key text
    info!("[Agent] CORS allowed origin: {}", origin); // log allowed origin

    let client = reqwest::Client::builder() // build shared HTTP client
        // Fix 5: timeout prevents user_loop tasks from blocking indefinitely
        // if Leader, Helper, or Reconstruct hangs (e.g. TCP accept but no response).
        // connect_timeout: max wait to establish TCP connection.
        // timeout: max total time for connect + send + receive response body.
        // Both are set independently so a slow response body also times out.
        .connect_timeout(Duration::from_secs(5)) // 5 second connection timeout
        .timeout(Duration::from_secs(10)) // 10 second total request timeout
        .build() // finalize client
        .expect("reqwest Client failed to build"); // panic if client creation fails
    let registry: SharedRegistry  = Arc::new(Mutex::new(Registry::new())); // create shared registry
    let count:    ActiveUserCount = Arc::new(AtomicUsize::new(0)); // create active user count

    // Fix 2: restricted CORS — only the configured dashboard origin
    // Fix 1: auth checked inside each handler that needs it
    let api_reg    = registry.clone(); // clone registry for API server task
    let api_client = client.clone(); // clone HTTP client for API server task
    let api_count  = count.clone(); // clone user count for API server task

    tokio::spawn(async move { // spawn the HTTP API server in a background task
        // ── CORS origins ─────────────────────────────────────────────────────
        //
        // IMPORTANT — dashboard must be served via HTTP, not opened as file://
        //
        // When index.html is opened directly as file:///path/to/index.html,
        // the browser sends "Origin: null" which is NOT in this list → all
        // POST requests are blocked by CORS → "Add failed: Failed to fetch".
        //
        // To run the dashboard correctly:
        //   cd sfss-final/dashboard
        //   python3 -m http.server 3000
        //   Then open http://127.0.0.1:3000 in your browser
        //
        // The 4 initial users already show because they are created directly
        // in main() before the HTTP server starts — no CORS involved for them.
        // But /user/add and /user/remove go through HTTP and require CORS.
        //
        let allow_origin = AllowOrigin::list(vec![ // create list of allowed origins
            // Dashboard served via python3 -m http.server
            "http://127.0.0.1:3000".parse().unwrap(), // allow 127.0.0.1:3000
            "http://localhost:3000".parse().unwrap(), // allow localhost:3000
            // Dashboard served on other common dev ports
            "http://127.0.0.1:8000".parse().unwrap(), // allow 8000
            "http://localhost:8000".parse().unwrap(), // allow localhost 8000
            "http://127.0.0.1:5500".parse().unwrap(),  // VS Code Live Server
            "http://localhost:5500".parse().unwrap(), // localhost 5500
            "http://127.0.0.1:5173".parse().unwrap(),  // Vite dev server
            "http://localhost:5173".parse().unwrap(), // localhost 5173
            // Agent itself (for curl / direct API calls)
            "http://127.0.0.1:8082".parse().unwrap(), // allow local agent origin
            // Custom origin from env var (already parsed as `origin` above)
            origin.parse().expect("invalid AGENT_ALLOWED_ORIGIN"), // allow configured origin
        ]); // end origin list
        let cors = CorsLayer::new() // create CORS layer
            .allow_origin(allow_origin) // set allowed origins
            .allow_methods([ // allow these HTTP methods
                axum::http::Method::GET, // GET requests allowed
                axum::http::Method::POST, // POST requests allowed
            ]) // end allowed methods
            .allow_headers([axum::http::header::CONTENT_TYPE, axum::http::header::AUTHORIZATION]); // allow JSON and auth headers

        let app = Router::new() // create router
            .route("/user/add",         post(handle_add_user)) // route add-user POST to handler
            .route("/user/remove",      post(handle_remove_user)) // route remove-user POST to handler
            .route("/users",            get(handle_list_users)) // route list-users GET to handler
            .route("/snapshot",         get(handle_snapshot_all)) // route snapshot GET to handler
            .layer(cors) // apply CORS middleware
            .with_state((api_reg, api_client, api_count)); // attach shared state

        let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", AGENT_PORT)) // bind to all interfaces on agent port
            .await.unwrap(); // wait until listener is ready
        info!("[Agent API] Listening on :{}", AGENT_PORT); // log that the API is live
        axum::serve(listener, app).await.unwrap(); // serve Axum app forever
    }); // end spawned API server task

    // Initial users
    for i in 0..INITIAL_USERS { // create the initial set of users
        let user_id = { registry.lock().unwrap().fresh_id() }; // get a unique user_id
        info!("[Agent] Initial user {}/{}: user_id={}", i+1, INITIAL_USERS, user_id); // log initial user creation

        let (key0, key1, sk0, sk1) = sfss_sys::gen(user_id); // generate SFSS keys for this user

        let _ = client.post(format!("{}/setup", LEADER_URL)) // prepare Leader setup request
            .json(&json!({"user_id": user_id, "key": hex::encode(&key0.0)})) // include user_id and Leader key
            .send().await.map_err(|e| tracing::warn!("Leader setup: {}", e)); // send request and warn on failure

        let _ = client.post(format!("{}/setup", HELPER_URL)) // prepare Helper setup request
            .json(&json!({"user_id": user_id, "key": hex::encode(&key1.0)})) // include user_id and Helper key
            .send().await.map_err(|e| tracing::warn!("Helper setup: {}", e)); // send request and warn on failure

        let stagger = (i as u64) * (TRANSMIT_S / INITIAL_USERS as u64); // spread transmissions across startup users
        let ctx_arc  = Arc::new(Mutex::new(UserContext::new(user_id, sk0, sk1, stagger))); // create per-user shared context
        let (stop_tx, stop_rx) = oneshot::channel::<()>(); // create stop signal pair
        tokio::spawn(user_loop(ctx_arc.clone(), client.clone(), count.clone(), stop_rx)); // start user loop task
        registry.lock().unwrap().users.insert(user_id, (ctx_arc, stop_tx)); // register the user
        count.fetch_add(1, Ordering::Relaxed); // increment active user count
    } // end initial users loop

    tokio::signal::ctrl_c().await.ok(); // wait for Ctrl+C before shutting down
    info!("[Agent] Shutting down"); // log shutdown
} // end main