// reconstruct/src/main.rs
//
// P6: Misaligned users (l_count ≠ h_count) are EXCLUDED — reconstruction skipped
// P7: mean_attention = (1/N)·Σ avg[u]  always in [0,255], not total/n_users
// Dynamic: /user/remove removes user from state immediately
// Dynamic: users are discovered from Leader/Helper at each poll — no hardcoded IDs

use axum::{extract::State, http::StatusCode, routing::{get, post}, Json, Router};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::time;
use tower_http::cors::CorsLayer;
use tracing::info;

const LEADER_URL:  &str = "http://127.0.0.1:8080";
const HELPER_URL:  &str = "http://127.0.0.1:8081";
const POLL_S:      u64  = 5;
const LISTEN_PORT: u16  = 8090;

// ── Browser-facing types (NO raw shares) ─────────────────────────────────────

#[derive(Clone, Serialize)]
struct UserResult {
    user_id:       u32,
    value:         u32,   // Σ attention — only meaningful when valid=true
    count:         u64,
    avg:           f64,   // value/count, in [0,255]
    percent:       f64,   // value/global_total × 100
    count_aligned: bool,  // P6: false → reconstruction skipped
    valid:         bool,  // alias for count_aligned
}

#[derive(Clone, Serialize)]
struct GlobalResult {
    n_users:        usize,   // valid users with count > 0
    total:          u32,     // Σ value[u] for valid users
    mean_attention: f64,     // P7: (1/N)·Σ avg[u] — always in [0,255]
    grand_avg:      f64,     // Σ value / Σ count (measurement-weighted)
    n_invalid:      usize,   // users with count mismatch
    last_updated:   u64,
}

#[derive(Clone, Serialize)]
struct AggregateResp {
    users:  HashMap<String, UserResult>,
    global: GlobalResult,
}

struct ReconstructState {
    latest:  AggregateResp,
    removed: std::collections::HashSet<u32>,  // user_ids removed from this service
}

impl ReconstructState {
    fn new() -> Self {
        Self {
            latest: AggregateResp {
                users: HashMap::new(),
                global: GlobalResult {
                    n_users: 0, total: 0, mean_attention: 0.0,
                    grand_avg: 0.0, n_invalid: 0, last_updated: 0,
                },
            },
            removed: std::collections::HashSet::new(),
        }
    }
}

type SharedState = Arc<Mutex<ReconstructState>>;

#[derive(Deserialize)] struct RawUserAgg { share: u32, count: u64 }
#[derive(Deserialize)] struct RawAllAgg  { users: HashMap<String, RawUserAgg> }
#[derive(Deserialize)] struct RemoveReq  { user_id: u32 }

// ── Poll loop ─────────────────────────────────────────────────────────────────

async fn poll_loop(state: SharedState, client: Client) {
    let mut interval = time::interval(Duration::from_secs(POLL_S));
    loop {
        interval.tick().await;

        let (l_res, h_res) = tokio::join!(
            client.get(format!("{}/aggregate", LEADER_URL)).send(),
            client.get(format!("{}/aggregate", HELPER_URL)).send(),
        );

        let (l_http, h_http) = match (l_res, h_res) {
            (Ok(l), Ok(h)) => (l, h),
            (Err(e), _) => { tracing::warn!("[R] Leader: {}", e); continue; }
            (_, Err(e)) => { tracing::warn!("[R] Helper: {}", e); continue; }
        };

        let (leader, helper): (RawAllAgg, RawAllAgg) = match tokio::join!(
            l_http.json::<RawAllAgg>(),
            h_http.json::<RawAllAgg>(),
        ) {
            (Ok(l), Ok(h)) => (l, h),
            _ => { tracing::warn!("[R] JSON parse error"); continue; }
        };

        let removed = state.lock().unwrap().removed.clone();

        let mut all_ids: Vec<u32> = leader.users.keys()
            .chain(helper.users.keys())
            .filter_map(|s| s.parse::<u32>().ok())
            .filter(|id| !removed.contains(id))
            .collect();
        all_ids.sort(); all_ids.dedup();

        let mut users: HashMap<String, UserResult> = HashMap::new();

        for uid in &all_ids {
            let key     = uid.to_string();
            let share0  = leader.users.get(&key).map(|u| u.share).unwrap_or(0);
            let share1  = helper.users.get(&key).map(|u| u.share).unwrap_or(0);
            let l_count = leader.users.get(&key).map(|u| u.count).unwrap_or(0);
            let h_count = helper.users.get(&key).map(|u| u.count).unwrap_or(0);

            // P6: if counts differ, shares are from different ctr — DO NOT reconstruct
            let aligned = l_count == h_count;
            if !aligned {
                tracing::warn!("[R] MISMATCH uid={} L={} H={} — SKIP reconstruction", uid, l_count, h_count);
                users.insert(key, UserResult {
                    user_id: *uid, value: 0, count: l_count,
                    avg: 0.0, percent: 0.0, count_aligned: false, valid: false,
                });
                continue;
            }

            // Reconstruction — shares exist as local variables for ~microseconds only
            let value = sfss_sys::reconstruct(share0, share1);
            let avg   = if l_count > 0 { value as f64 / l_count as f64 } else { 0.0 };
            users.insert(key, UserResult {
                user_id: *uid, value, count: l_count, avg,
                percent: 0.0,  // filled below
                count_aligned: true, valid: true,
            });
        }

        // P7: global aggregation — valid users only
        let valid: Vec<_> = users.values().filter(|u| u.valid && u.count > 0).collect();
        let n_users  = valid.len();
        let n_invalid = users.values().filter(|u| !u.valid).count();
        let total    = valid.iter().fold(0u32, |a, u| a.wrapping_add(u.value));
        // P7 correct mean: (1/N)·Σ avg[u] — always in [0,255]
        let mean_attention = if n_users > 0 {
            valid.iter().map(|u| u.avg).sum::<f64>() / n_users as f64
        } else { 0.0 };
        let grand_total_count: u64 = valid.iter().map(|u| u.count).sum();
        let grand_avg = if grand_total_count > 0 {
            total as f64 / grand_total_count as f64
        } else { 0.0 };

        // Back-fill percent
        for u in users.values_mut() {
            u.percent = if u.valid && total > 0 {
                u.value as f64 / total as f64 * 100.0
            } else { 0.0 };
        }

        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();

        info!("[R] {} valid | total={} | mean_attn={:.1} | grand={:.1} | {} invalid",
            n_users, total, mean_attention, grand_avg, n_invalid);

        let mut s = state.lock().unwrap();
        s.latest = AggregateResp {
            users,
            global: GlobalResult { n_users, total, mean_attention, grand_avg, n_invalid, last_updated: now },
        };
    }
}

// ── Handlers ──────────────────────────────────────────────────────────────────

async fn handle_aggregate(State(s): State<SharedState>) -> Json<AggregateResp> {
    Json(s.lock().unwrap().latest.clone())
}

async fn handle_remove(
    State(state): State<SharedState>,
    Json(req): Json<RemoveReq>,
) -> (StatusCode, Json<serde_json::Value>) {
    let mut s = state.lock().unwrap();
    s.removed.insert(req.user_id);
    s.latest.users.remove(&req.user_id.to_string());
    // Recompute global without the removed user
    let valid: Vec<_> = s.latest.users.values().filter(|u| u.valid && u.count > 0).collect();
    let n = valid.len();
    let total = valid.iter().fold(0u32, |a, u| a.wrapping_add(u.value));
    let mean  = if n > 0 { valid.iter().map(|u| u.avg).sum::<f64>() / n as f64 } else { 0.0 };
    let grand_total_count: u64 = valid.iter().map(|u| u.count).sum();
    let grand_avg = if grand_total_count > 0 {
        total as f64 / grand_total_count as f64
    } else { 0.0 };
    let n_invalid = s.latest.users.values().filter(|u| !u.valid).count();
    s.latest.global.n_users         = n;
    s.latest.global.total           = total;
    s.latest.global.mean_attention  = mean;
    s.latest.global.grand_avg       = grand_avg;
    s.latest.global.n_invalid       = n_invalid;
    info!("[R] Removed uid={}", req.user_id);
    (StatusCode::OK, Json(serde_json::json!({"status":"removed","user_id":req.user_id})))
}

// ── Main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt().with_env_filter("reconstruct=info").init();
    sfss_sys::init();
    let state  = Arc::new(Mutex::new(ReconstructState::new()));
    let client = Client::new();
    let ps = state.clone(); let pc = client.clone();
    tokio::spawn(async move { poll_loop(ps, pc).await; });
    let app = Router::new()
        .route("/aggregate",   get(handle_aggregate))
        .route("/user/remove", post(handle_remove))
        .layer(CorsLayer::permissive())
        .with_state(state);
    info!("[Reconstruct] Listening on 0.0.0.0:{}", LISTEN_PORT);
    axum::serve(
        tokio::net::TcpListener::bind(format!("0.0.0.0:{}", LISTEN_PORT)).await.unwrap(),
        app,
    ).await.unwrap();
}