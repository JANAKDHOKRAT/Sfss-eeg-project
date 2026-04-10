use rand::Rng;
use std::time::Duration;
use tokio::time;
use tracing::info;
use serde_json::json;

// ── Config ────────────────────────────────────────────────────────────────────

const LEADER_URL : &str = "http://127.0.0.1:8080";
const HELPER_URL : &str = "http://127.0.0.1:8081";
const INTERVAL_S : u64  = 30;
const USER_ID    : u32  = 42;   // Fixed user identity for this prototype.

// ── Main ─────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter("agent=info")
        .init();

    sfss_sys::init();

    let client = reqwest::Client::new();

    // ── Step 1: Key generation ────────────────────────────────────────────────
    info!("[Agent] Generating SDPF keys for user_id={}", USER_ID);
    let (key0, key1, mut sk0, mut sk1) = sfss_sys::gen(USER_ID);

    info!("[Agent] key0 = {} bytes  key1 = {} bytes",
        sfss_sys::SFSS_KEY_SIZE, sfss_sys::SFSS_KEY_SIZE);

    // ── Step 2: Send server keys ──────────────────────────────────────────────
    let setup_payload_leader = json!({
        "user_id": USER_ID,
        "key": hex::encode(&key0.0),
    });
    let setup_payload_helper = json!({
        "user_id": USER_ID,
        "key": hex::encode(&key1.0),
    });

    client
        .post(format!("{}/setup", LEADER_URL))
        .json(&setup_payload_leader)
        .send()
        .await
        .expect("Could not reach Leader /setup")
        .error_for_status()
        .expect("Leader /setup returned error");
    info!("[Agent] Sent key0 to Leader.");

    client
        .post(format!("{}/setup", HELPER_URL))
        .json(&setup_payload_helper)
        .send()
        .await
        .expect("Could not reach Helper /setup")
        .error_for_status()
        .expect("Helper /setup returned error");
    info!("[Agent] Sent key1 to Helper.");

    // ── Step 3: Timed loop ────────────────────────────────────────────────────
    let mut interval = time::interval(Duration::from_secs(INTERVAL_S));
    let mut rng      = rand::thread_rng();
    let mut tick     = 0u64;

    info!("[Agent] Starting — will transmit every {} seconds.", INTERVAL_S);

    loop {
        interval.tick().await;
        tick += 1;

        // Simulated attention metric: uniform integer in [0, 255].
        let attention: u8 = rng.gen_range(0..=255);

        // ── Real crypto: SDPF_enc ─────────────────────────────────────────────
        // Encrypts `attention` using the two AES-based stream keys.
        // sk0 and sk1 are updated in place (counter incremented).
        // The ciphertext is computationally indistinguishable from random.
        let ctx = sfss_sys::encrypt(&mut sk0, &mut sk1, attention);

        let ctx_hex = hex::encode(&ctx.0);
        info!(
            "[Agent] Tick #{}: attention={:3}  ctx={}",
            tick, attention, ctx_hex
        );

        // Send the SAME ciphertext to both servers.
        let payload = json!({
            "user_id": USER_ID,
            "ctx": ctx_hex,
        });

        // Fire-and-forget style; tolerate transient failures.
        match client
            .post(format!("{}/submit", LEADER_URL))
            .json(&payload)
            .send()
            .await
        {
            Ok(r) => info!("[Agent] Leader /submit → HTTP {}", r.status()),
            Err(e) => tracing::warn!("[Agent] Leader /submit failed: {}", e),
        }

        match client
            .post(format!("{}/submit", HELPER_URL))
            .json(&payload)
            .send()
            .await
        {
            Ok(r) => info!("[Agent] Helper /submit → HTTP {}", r.status()),
            Err(e) => tracing::warn!("[Agent] Helper /submit failed: {}", e),
        }
    }
}
