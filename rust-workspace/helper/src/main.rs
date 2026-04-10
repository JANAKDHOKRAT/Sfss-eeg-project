// helper/src/main.rs
//
// Helper server — port 8081, NO CORS.
//
// Role:
//   Receives encrypted EEG ciphertexts (Ctx) from the Agent.
//   Evaluates SDPF_eval with key1, accumulates share1 per user.
//   The Reconstruct service fetches the accumulated share1 (server-to-server only).
//
// SDPF correctness guarantee (from sfss.h):
//   share0(x) + share1(x) = attention_value   if x == user_id
//   share0(x) + share1(x) = 0                 otherwise
//
// Relationship to Leader:
//   Leader and Helper receive the SAME Ctx for each transmission.
//   Leader evaluates with key0 → share0
//   Helper evaluates with key1 → share1
//   share0 alone reveals nothing. share1 alone reveals nothing.
//   Only share0 + share1 (done inside Reconstruct) yields the plaintext.
//
// Security:
//   • NO CorsLayer — browsers blocked by their own CORS policy
//   • share1 is NEVER logged (spec: "Do NOT log share0, share1, ctx, prf outputs")
//   • Unknown user_id  → 410 Gone   (late packet after removal — no panic)
//   • Duplicate seq    → 409 Conflict (P5 replay protection)
//   • Out-of-order seq → 409 Conflict (P4 ordering protection)
//
// Endpoints:
//   POST /setup            — Agent registers a new user with key1
//   POST /submit           — Agent submits encrypted ctx + seq number
//   POST /user/remove      — Agent signals user removal
//   GET  /aggregate        — Reconstruct polls all shares   (server-to-server)
//   GET  /aggregate/:uid   — Reconstruct polls one user's share

use axum::{
    extract::{Path, State},
    http::StatusCode,
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use sfss_sys::{ServerKey, Ctx};

// ── Per-user state ────────────────────────────────────────────────────────────

struct PerUserState {
    key:       ServerKey,
    aggregate: u32,           // Σ share1  (mod 2^32, wrapping — Z/2^32 group arithmetic)
    count:     u64,           // number of valid SDPF evaluations accumulated
    last_seq:  u64,           // P4: highest accepted seq (u64::MAX = none received yet)
    seen_seqs: HashSet<u64>,  // P5: all accepted seqs — prevents exact replay attacks
}

struct ServerState {
    users: HashMap<u32, PerUserState>,
}

impl ServerState {
    fn new() -> Self { Self { users: HashMap::new() } }
}

type SharedState = Arc<Mutex<ServerState>>;

// ── Request / response types ──────────────────────────────────────────────────

#[derive(Deserialize)]
struct SetupReq { user_id: u32, key: String }

#[derive(Deserialize)]
struct SubmitReq {
    user_id: u32,
    ctx:     String,  // hex-encoded Ctx (SFSS_CTX_SIZE bytes)
    seq:     u64,     // P4: monotonically increasing per user, assigned by Agent
}

#[derive(Deserialize)]
struct RemoveReq { user_id: u32 }

// Internal response — consumed only by Reconstruct (server-to-server).
// share = accumulated share1; meaningless without Leader's complementary share0.
#[derive(Serialize)]
struct UserAggResp { user_id: u32, share: u32, count: u64 }

#[derive(Serialize)]
struct AllAggResp { users: HashMap<String, UserAggResp> }

#[derive(Serialize)]
struct StatusResp { status: String, count: u64 }

// ── Handlers ──────────────────────────────────────────────────────────────────

async fn handle_setup(
    State(state): State<SharedState>,
    Json(req): Json<SetupReq>,
) -> (StatusCode, Json<StatusResp>) {
    let key_bytes = match hex::decode(&req.key) {
        Ok(b) if b.len() == sfss_sys::SFSS_KEY_SIZE as usize => b,
        _ => {
            tracing::warn!("[Helper] Setup uid={} — bad key (wrong length or invalid hex)",
                req.user_id);
            return (StatusCode::BAD_REQUEST,
                    Json(StatusResp { status: "bad_key".into(), count: 0 }));
        }
    };
    let mut key_arr = [0u8; sfss_sys::SFSS_KEY_SIZE as usize];
    key_arr.copy_from_slice(&key_bytes);

    let mut s = state.lock().unwrap();
    s.users.insert(req.user_id, PerUserState {
        key:       ServerKey(key_arr),
        aggregate: 0,
        count:     0,
        last_seq:  u64::MAX,   // sentinel — first seq=0 satisfies (u64::MAX == u64::MAX) branch
        seen_seqs: HashSet::new(),
    });
    tracing::info!("[Helper] Setup uid={} — registered (active={})", req.user_id, s.users.len());
    (StatusCode::OK, Json(StatusResp { status: "ok".into(), count: 0 }))
}

async fn handle_submit(
    State(state): State<SharedState>,
    Json(req): Json<SubmitReq>,
) -> (StatusCode, Json<StatusResp>) {
    // Decode and validate ctx before acquiring the lock
    let ctx_bytes = match hex::decode(&req.ctx) {
        Ok(b) if b.len() == sfss_sys::SFSS_CTX_SIZE as usize => b,
        _ => {
            tracing::warn!("[Helper] Submit uid={} seq={} — bad ctx", req.user_id, req.seq);
            return (StatusCode::BAD_REQUEST,
                    Json(StatusResp { status: "bad_ctx".into(), count: 0 }));
        }
    };
    let mut ctx_arr = [0u8; sfss_sys::SFSS_CTX_SIZE as usize];
    ctx_arr.copy_from_slice(&ctx_bytes);
    let ctx = Ctx(ctx_arr);

    let mut s = state.lock().unwrap();

    // Late-packet guard — user was removed; drop silently.
    // Returns 410 Gone so the Agent logs this as expected, not an error.
    // Never panics on missing user_id.
    let user = match s.users.get_mut(&req.user_id) {
        Some(u) => u,
        None    => {
            tracing::warn!("[Helper] Submit uid={} seq={} — late packet (user removed), dropping",
                req.user_id, req.seq);
            return (StatusCode::GONE,
                    Json(StatusResp { status: "user_not_found".into(), count: 0 }));
        }
    };

    // ── P5: Replay protection ─────────────────────────────────────────────────
    // Reject any seq already in seen_seqs before modifying any state.
    if user.seen_seqs.contains(&req.seq) {
        tracing::warn!("[Helper] REPLAY uid={} seq={} — rejected", req.user_id, req.seq);
        return (StatusCode::CONFLICT,
                Json(StatusResp { status: "replay".into(), count: user.count }));
    }

    // ── P4: Ordering protection ───────────────────────────────────────────────
    // Helper and Leader both enforce ordering independently.
    // The Agent only sends to Helper after Leader accepts — so in normal operation
    // Helper never sees a seq that Leader already rejected. But we enforce it here
    // defensively, and to keep count exactly equal to Leader's count.
    let ordering_ok = user.last_seq == u64::MAX || req.seq > user.last_seq;
    if !ordering_ok {
        tracing::warn!("[Helper] OUT-OF-ORDER uid={} seq={} last_accepted={}",
            req.user_id, req.seq, user.last_seq);
        return (StatusCode::CONFLICT,
                Json(StatusResp { status: "out_of_order".into(), count: user.count }));
    }

    // Packet is valid — commit
    user.seen_seqs.insert(req.seq);
    user.last_seq  = req.seq;

    // ── SDPF_eval<uint32_t, 32> ──────────────────────────────────────────────
    // Evaluates the DPF tree at index = user_id using key1.
    // share0 + share1 = attention_value  if eval index == user_id
    // share0 + share1 = 0                for any other index (DPF zero property)
    // Invariant 4: eval index MUST equal user_id — we pass req.user_id explicitly.
    let share1 = sfss_sys::eval(&user.key, &ctx, req.user_id);

    // Accumulate in Z/2^32 — wrapping_add is the correct group operation.
    user.aggregate = user.aggregate.wrapping_add(share1);
    user.count    += 1;
    let count      = user.count;

    // ⚠ share1 is NOT logged — spec: "Do NOT log share0, share1, ctx, prf outputs"
    // Logging the share would be a partial information leak: an attacker with access
    // to Helper's logs could recover attention values if they also compromised Leader.
    tracing::info!("[Helper] Submit uid={} seq={} count={} — accepted", req.user_id, req.seq, count);
    (StatusCode::OK, Json(StatusResp { status: "ok".into(), count }))
}

async fn handle_remove(
    State(state): State<SharedState>,
    Json(req): Json<RemoveReq>,
) -> (StatusCode, Json<StatusResp>) {
    let mut s = state.lock().unwrap();
    if s.users.remove(&req.user_id).is_some() {
        tracing::info!("[Helper] Removed uid={} (active={})", req.user_id, s.users.len());
        (StatusCode::OK, Json(StatusResp { status: "removed".into(), count: 0 }))
    } else {
        tracing::warn!("[Helper] Remove uid={} — not found", req.user_id);
        (StatusCode::NOT_FOUND, Json(StatusResp { status: "not_found".into(), count: 0 }))
    }
}

// GET /aggregate — polled by Reconstruct service only.
// No CorsLayer means browsers cannot reach this even if they try.
async fn handle_aggregate_all(State(state): State<SharedState>) -> Json<AllAggResp> {
    let s = state.lock().unwrap();
    let users = s.users.iter().map(|(uid, u)| {
        (uid.to_string(), UserAggResp { user_id: *uid, share: u.aggregate, count: u.count })
    }).collect();
    Json(AllAggResp { users })
}

async fn handle_aggregate_user(
    State(state): State<SharedState>,
    Path(uid): Path<u32>,
) -> Json<UserAggResp> {
    let s = state.lock().unwrap();
    match s.users.get(&uid) {
        Some(u) => Json(UserAggResp { user_id: uid, share: u.aggregate, count: u.count }),
        None    => Json(UserAggResp { user_id: uid, share: 0, count: 0 }),
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt().with_env_filter("helper=info").init();
    sfss_sys::init();

    let state: SharedState = Arc::new(Mutex::new(ServerState::new()));

    let app = Router::new()
        .route("/setup",              post(handle_setup))
        .route("/submit",             post(handle_submit))
        .route("/user/remove",        post(handle_remove))
        .route("/aggregate",          get(handle_aggregate_all))
        .route("/aggregate/:user_id", get(handle_aggregate_user))
        // ← NO CorsLayer — browsers cannot reach any endpoint on this server
        .with_state(state);

    tracing::info!("[Helper] Listening on 0.0.0.0:8081 — no CORS, server-to-server only");
    axum::serve(
        tokio::net::TcpListener::bind("0.0.0.0:8081").await.unwrap(),
        app,
    ).await.unwrap();
}