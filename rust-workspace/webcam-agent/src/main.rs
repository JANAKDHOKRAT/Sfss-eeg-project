// webcam-agent/src/main.rs
//
// SFSS Webcam Agent — receives attention scores from the Python eye tracker,
// encrypts them via SDPF (same crypto as the EEG agent), and forwards to
// Leader/Helper for privacy-preserving histogram aggregation.
//
// ═══════════════════════════════════════════════════════════════════════
//  SFSS CRYPTO FLOW (identical to EEG agent — DO NOT CHANGE)
// ═══════════════════════════════════════════════════════════════════════
//
//   Setup (ONCE per face):
//     (key0, key1, sk0, sk1) = SDPF_gen<32>(user_id)     -- sfss_sys::gen
//     POST /setup {user_id, key: hex(key0)} → Leader     -- Leader holds key0
//     POST /setup {user_id, key: hex(key1)} → Helper     -- Helper holds key1
//
//   Per tick (PER SCORE):
//     ctx = SDPF_enc<u32>(sk0, sk1, score)               -- sfss_sys::encrypt
//     POST /submit {user_id, ctx: hex(ctx), seq}         -- Leader + Helper
//     seq advances monotonically: 0, 1, 2, ...           -- required by P4
//
//   Server (Leader/Helper, EXISTING — unchanged):
//     share = SDPF_eval<u32,32>(key, ctx, user_id)       -- sfss_sys::eval
//     aggregate = aggregate.wrapping_add(share)          -- Z/2^32
//     count += 1
//
//   Reconstruct (EXISTING — unchanged):
//     value = share0 + share1 mod 2^32 = Σ plaintexts    -- sfss_sys::reconstruct
//     avg   = value / count = average attention [0,255]
//
//   Dashboard (EXISTING — unchanged):
//     /histogram bins users by their avg → aggregate counts
//
// ═══════════════════════════════════════════════════════════════════════
//  CRITICAL INVARIANTS
// ═══════════════════════════════════════════════════════════════════════
//
//   1. gen() is called EXACTLY ONCE per face session — never per tick
//   2. encrypt() is called ONCE per tick with the RAW attention score
//   3. seq is monotonically increasing per user (required by Leader P4)
//   4. Each face has a unique user_id → unique DPF key pair
//   5. sk0, sk1 are &mut — their internal counters advance per encrypt()
//   6. No input validation — SFSS is TRUSTED-CLIENT (§8 [Song+26])
//
// ═══════════════════════════════════════════════════════════════════════

use axum::{
    extract::State,
    http::StatusCode,
    routing::{get, post},
    Json, Router,
};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tower_http::cors::CorsLayer;
use tracing::{info, warn};

// ── Configuration ─────────────────────────────────────────────────────────

const LISTEN_PORT: u16 = 8083;
const LEADER_URL: &str = "http://127.0.0.1:8080";
const HELPER_URL: &str = "http://127.0.0.1:8081";
const RECONSTRUCT_URL: &str = "http://127.0.0.1:8090";

// ── SFSS Session (one per face) ───────────────────────────────────────────

struct SfssSession {
    user_id: u32,
    face_id: i32,
    key0: sfss_sys::ServerKey,   // sent to Leader at setup; retained for logging
    key1: sfss_sys::ServerKey,   // sent to Helper at setup; retained for logging
    sk0: sfss_sys::StreamKey,    // counter advances per encrypt()
    sk1: sfss_sys::StreamKey,    // counter advances per encrypt()
    seq: u64,                     // monotonic tick counter (0, 1, 2, ...)
    total_ticks: u64,             // accepted tick count
}

impl SfssSession {
    /// Create a new SFSS session for a face.
    ///
    /// INVARIANT: gen() is called EXACTLY ONCE here, never per tick.
    fn new(user_id: u32, face_id: i32) -> Self {
        // SDPF_gen<32>(user_id) — ONCE per session.
        // Generates fresh DPF keys with AES-seeded TwoKeyPRP from emp-tool.
        // user_id is encoded as the target index α in the 32-level DPF tree.
        let (key0, key1, sk0, sk1) = sfss_sys::gen(user_id);

        info!(
            "[SFSS] gen(uid={}) → key0[..4]={:02x}{:02x}{:02x}{:02x} | face_id={}",
            user_id, key0.0[0], key0.0[1], key0.0[2], key0.0[3], face_id
        );

        Self {
            user_id,
            face_id,
            key0,
            key1,
            sk0,
            sk1,
            seq: 0,
            total_ticks: 0,
        }
    }

    /// Encrypt a single attention score.
    ///
    /// CRYPTO: same as sfss_backend.rs / EEG agent:
    ///   cⱼ = (-1)^t_last × (score - F(⟦r⟧₀, ctr) + F(-⟦r⟧₁, ctr))
    ///
    /// PRF counter inside sk0/sk1 advances by 1 per call.
    /// One tick = one call to encrypt() = one ciphertext = one /submit.
    fn encrypt_score(&mut self, score: u8) -> (u64, sfss_sys::Ctx) {
        let seq = self.seq;

        // SDPF_enc<uint32_t> — encrypt RAW score (same as EEG agent).
        // DO NOT one-hot-encode — Leader accumulates shares into ONE u32 per
        // user; encoding 5 indicators would make all users land in bin [0,50).
        let ctx = sfss_sys::encrypt(&mut self.sk0, &mut self.sk1, score);

        self.seq += 1;
        self.total_ticks += 1;

        (seq, ctx)
    }
}

// ── Shared State ──────────────────────────────────────────────────────────

struct AppState {
    sessions: HashMap<u32, SfssSession>,     // user_id → session
    face_to_uid: HashMap<i32, u32>,          // face_id (from Python) → user_id
    http: Client,
}

type SharedState = Arc<Mutex<AppState>>;

// ── API Types ─────────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct AddFaceReq {
    face_id: i32,
}

#[derive(Serialize)]
struct AddFaceResp {
    user_id: u32,
    face_id: i32,
    status: String,
}

#[derive(Deserialize)]
struct ScoreReq {
    user_id: u32,
    score: u8,
}

#[derive(Deserialize)]
struct RemoveFaceReq {
    user_id: u32,
}

/// Payload for Leader/Helper /setup — key is HEX-encoded (593 B = 1186 hex chars).
#[derive(Serialize)]
struct SetupPayload {
    user_id: u32,
    key: String,
}

/// Payload for Leader/Helper /submit — ctx is HEX-encoded (12 B = 24 hex chars).
#[derive(Serialize, Clone)]
struct SubmitPayload {
    user_id: u32,
    ctx: String,
    seq: u64,
}

#[derive(Serialize, Clone)]
struct RemovePayload {
    user_id: u32,
}

// ── Handlers ──────────────────────────────────────────────────────────────

/// POST /face/add — Python tracker registers a new face.
///
/// Triggers:
///   1. SDPF_gen<32>(user_id) — ONCE
///   2. POST /setup → Leader with hex(key0)
///   3. POST /setup → Helper with hex(key1)
///
/// Returns success only after BOTH Leader and Helper confirm setup.
/// This way if either is down, we don't commit a broken session.
async fn handle_add_face(
    State(state): State<SharedState>,
    Json(req): Json<AddFaceReq>,
) -> (StatusCode, Json<AddFaceResp>) {
    // Already registered?
    {
        let s = state.lock().unwrap();
        if let Some(&uid) = s.face_to_uid.get(&req.face_id) {
            return (
                StatusCode::OK,
                Json(AddFaceResp {
                    user_id: uid,
                    face_id: req.face_id,
                    status: "already_registered".into(),
                }),
            );
        }
    }

    // Generate unique user_id and create SFSS session (calls gen() ONCE)
    let user_id: u32 = rand::random();
    let session = SfssSession::new(user_id, req.face_id);

    // Hex-encode keys (Leader/Helper use hex::decode in handle_setup)
    let key0_hex = hex::encode(&session.key0.0);
    let key1_hex = hex::encode(&session.key1.0);

    let http = {
        let s = state.lock().unwrap();
        s.http.clone()
    };

    // Send key0 → Leader
    let leader_res = http
        .post(format!("{}/setup", LEADER_URL))
        .json(&SetupPayload { user_id, key: key0_hex })
        .send()
        .await;

    match leader_res {
        Ok(resp) if resp.status().is_success() => {
            info!("[WEBCAM] /setup → Leader uid={} 200 OK", user_id);
        }
        Ok(resp) => {
            warn!(
                "[WEBCAM] /setup → Leader uid={} FAILED status={}",
                user_id, resp.status()
            );
            return (
                StatusCode::BAD_GATEWAY,
                Json(AddFaceResp {
                    user_id,
                    face_id: req.face_id,
                    status: format!("leader_setup_failed_{}", resp.status()),
                }),
            );
        }
        Err(e) => {
            warn!("[WEBCAM] /setup → Leader uid={} ERROR: {}", user_id, e);
            return (
                StatusCode::BAD_GATEWAY,
                Json(AddFaceResp {
                    user_id,
                    face_id: req.face_id,
                    status: format!("leader_unreachable: {}", e),
                }),
            );
        }
    }

    // Send key1 → Helper
    let helper_res = http
        .post(format!("{}/setup", HELPER_URL))
        .json(&SetupPayload { user_id, key: key1_hex })
        .send()
        .await;

    match helper_res {
        Ok(resp) if resp.status().is_success() => {
            info!("[WEBCAM] /setup → Helper uid={} 200 OK", user_id);
        }
        Ok(resp) => {
            warn!(
                "[WEBCAM] /setup → Helper uid={} FAILED status={}",
                user_id, resp.status()
            );
            return (
                StatusCode::BAD_GATEWAY,
                Json(AddFaceResp {
                    user_id,
                    face_id: req.face_id,
                    status: format!("helper_setup_failed_{}", resp.status()),
                }),
            );
        }
        Err(e) => {
            warn!("[WEBCAM] /setup → Helper uid={} ERROR: {}", user_id, e);
            return (
                StatusCode::BAD_GATEWAY,
                Json(AddFaceResp {
                    user_id,
                    face_id: req.face_id,
                    status: format!("helper_unreachable: {}", e),
                }),
            );
        }
    }

    // Both Leader and Helper accepted — commit session
    {
        let mut s = state.lock().unwrap();
        s.face_to_uid.insert(req.face_id, user_id);
        s.sessions.insert(user_id, session);
    }

    info!(
        "[WEBCAM] Face {} registered → uid:{} (SDPF setup complete)",
        req.face_id, user_id
    );

    (
        StatusCode::OK,
        Json(AddFaceResp {
            user_id,
            face_id: req.face_id,
            status: "registered".into(),
        }),
    )
}

/// POST /score — Python tracker sends an attention score (0-255).
///
/// Triggers:
///   1. SDPF_enc(sk0, sk1, score) — counter advances
///   2. POST /submit → Leader with hex(ctx), seq
///   3. POST /submit → Helper with hex(ctx), seq
async fn handle_score(
    State(state): State<SharedState>,
    Json(req): Json<ScoreReq>,
) -> StatusCode {
    let (seq, ctx, user_id, http) = {
        let mut s = state.lock().unwrap();
        let session = match s.sessions.get_mut(&req.user_id) {
            Some(sess) => sess,
            None => {
                warn!("[WEBCAM] /score uid={} — session not found", req.user_id);
                return StatusCode::NOT_FOUND;
            }
        };

        // SDPF_enc — RAW score, ONE ciphertext per tick
        let (seq, ctx) = session.encrypt_score(req.score);
        (seq, ctx, session.user_id, s.http.clone())
    };

    let payload = SubmitPayload {
        user_id,
        ctx: hex::encode(&ctx.0),
        seq,
    };

    // Send to Leader — fire-and-forget WITH error logging
    let http_l = http.clone();
    let payload_l = payload.clone();
    tokio::spawn(async move {
        match http_l
            .post(format!("{}/submit", LEADER_URL))
            .json(&payload_l)
            .send()
            .await
        {
            Ok(resp) if resp.status().is_success() => {
                // success — too chatty to log per-tick
            }
            Ok(resp) => {
                warn!(
                    "[WEBCAM] /submit → Leader uid={} seq={} FAILED status={}",
                    payload_l.user_id, payload_l.seq, resp.status()
                );
            }
            Err(e) => {
                warn!(
                    "[WEBCAM] /submit → Leader uid={} seq={} ERROR: {}",
                    payload_l.user_id, payload_l.seq, e
                );
            }
        }
    });

    // Send to Helper
    let payload_h = payload.clone();
    tokio::spawn(async move {
        match http
            .post(format!("{}/submit", HELPER_URL))
            .json(&payload_h)
            .send()
            .await
        {
            Ok(resp) if resp.status().is_success() => {}
            Ok(resp) => {
                warn!(
                    "[WEBCAM] /submit → Helper uid={} seq={} FAILED status={}",
                    payload_h.user_id, payload_h.seq, resp.status()
                );
            }
            Err(e) => {
                warn!(
                    "[WEBCAM] /submit → Helper uid={} seq={} ERROR: {}",
                    payload_h.user_id, payload_h.seq, e
                );
            }
        }
    });

    StatusCode::OK
}

/// POST /face/remove — Python tracker signals face lost.
///
/// Cleans up the session locally, then notifies Leader, Helper, and Reconstruct.
async fn handle_remove_face(
    State(state): State<SharedState>,
    Json(req): Json<RemoveFaceReq>,
) -> StatusCode {
    let (face_id, total_ticks, http) = {
        let mut s = state.lock().unwrap();
        match s.sessions.remove(&req.user_id) {
            Some(session) => {
                s.face_to_uid.remove(&session.face_id);
                (session.face_id, session.total_ticks, s.http.clone())
            }
            None => return StatusCode::NOT_FOUND,
        }
    };

    info!(
        "[WEBCAM] Face {} (uid:{}) removed after {} ticks",
        face_id, req.user_id, total_ticks
    );

    let payload = RemovePayload { user_id: req.user_id };

    // Notify Leader
    let http_l = http.clone();
    let p_l = payload.clone();
    tokio::spawn(async move {
        let _ = http_l
            .post(format!("{}/user/remove", LEADER_URL))
            .json(&p_l)
            .send()
            .await;
    });

    // Notify Helper
    let http_h = http.clone();
    let p_h = payload.clone();
    tokio::spawn(async move {
        let _ = http_h
            .post(format!("{}/user/remove", HELPER_URL))
            .json(&p_h)
            .send()
            .await;
    });

    // Notify Reconstruct
    tokio::spawn(async move {
        let _ = http
            .post(format!("{}/user/remove", RECONSTRUCT_URL))
            .json(&payload)
            .send()
            .await;
    });

    StatusCode::OK
}

/// GET /status — show active sessions (session metadata only, no crypto state).
async fn handle_status(State(state): State<SharedState>) -> Json<serde_json::Value> {
    let s = state.lock().unwrap();
    let faces: Vec<_> = s
        .sessions
        .values()
        .map(|sess| {
            serde_json::json!({
                "face_id": sess.face_id,
                "user_id": sess.user_id,
                "seq": sess.seq,
                "ticks": sess.total_ticks
            })
        })
        .collect();

    Json(serde_json::json!({
        "active_faces": faces.len(),
        "faces": faces
    }))
}

/// GET /users — list active user_ids (for dashboard discovery).
async fn handle_users(State(state): State<SharedState>) -> Json<serde_json::Value> {
    let s = state.lock().unwrap();
    let user_ids: Vec<u32> = s.sessions.keys().copied().collect();
    Json(serde_json::json!({ "user_ids": user_ids }))
}

// ── Main ──────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter("webcam_agent=info")
        .init();

    // Initialize SFSS C++ library (AES-NI setup)
    sfss_sys::init();

    let state = Arc::new(Mutex::new(AppState {
        sessions: HashMap::new(),
        face_to_uid: HashMap::new(),
        http: Client::new(),
    }));

    let app = Router::new()
        .route("/face/add", post(handle_add_face))
        .route("/score", post(handle_score))
        .route("/face/remove", post(handle_remove_face))
        .route("/status", get(handle_status))
        .route("/users", get(handle_users))
        .layer(CorsLayer::permissive())
        .with_state(state);

    info!("════════════════════════════════════════════════════════════");
    info!("  SFSS Webcam Agent");
    info!("  Listening on 0.0.0.0:{}", LISTEN_PORT);
    info!("  Leader: {}  Helper: {}", LEADER_URL, HELPER_URL);
    info!("  Reconstruct: {}", RECONSTRUCT_URL);
    info!("  Crypto: SDPF_gen once per face, SDPF_enc(raw_score) per tick");
    info!("════════════════════════════════════════════════════════════");

    axum::serve(
        tokio::net::TcpListener::bind(format!("0.0.0.0:{}", LISTEN_PORT))
            .await
            .unwrap(),
        app,
    )
    .await
    .unwrap();
}