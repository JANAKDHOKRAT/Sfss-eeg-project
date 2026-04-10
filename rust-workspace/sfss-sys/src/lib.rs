#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case)]
// Allows non-Rust naming conventions because bindings.rs comes from C/C++ (FFI)
// These names come from sfss_ffi.h (C interface)

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
// Includes the generated Rust FFI bindings created by bindgen at build time
// Source: build.rs → bindgen → sfss_ffi.h → produces bindings.rs
// This exposes functions like:
//   sfss_init()
//   sfss_sdpf_gen()
//   sfss_sdpf_enc()
//   sfss_sdpf_eval()
//   sfss_reconstruct()

use std::sync::OnceLock;
// OnceLock ensures a value is initialized only once (thread-safe global init)

static INIT: OnceLock<()> = OnceLock::new();
// Global one-time initialization guard for SFSS library

pub fn init() {
    INIT.get_or_init(|| unsafe { sfss_init() });
    // Calls sfss_init() ONLY ONCE across entire program
    // sfss_init() is defined in C++ (sfss_ffi.cpp)
    // It initializes:
    //   - EMP toolkit AES context
    //   - PRG / PRP internal state
}

// ───────── STRUCTS ─────────

#[derive(Clone)]
pub struct ServerKey(pub [u8; SFSS_KEY_SIZE as usize]);
// Wrapper around raw byte array for server key
// Size defined in sfss_ffi.h:
//   SFSS_KEY_SIZE = 593 bytes
// Represents:
//   sdpf_key_class<32> from sfss.h (C++)

#[derive(Clone)]
pub struct StreamKey(pub [u8; SFSS_STREAM_KEY_SIZE as usize]);
// Wrapper for streaming key
// Size = 25 bytes
// Represents:
//   sdpf_stream_key (C++ struct)
// Contains:
//   - PRF key
//   - last tree bit
//   - counter (ctr)

#[derive(Clone, serde::Serialize, serde::Deserialize)]
pub struct Ctx(pub [u8; SFSS_CTX_SIZE as usize]);
// Wrapper for ciphertext (Ctx)
// Size = 12 bytes
// Represents:
//   stream_ctx_type<uint32_t> from sfss.h
// Contains:
//   - ctr (counter)
//   - encrypted value (masked)

// ───────── HIGH LEVEL API ─────────

pub fn gen(user_id: u32) -> (ServerKey, ServerKey, StreamKey, StreamKey) {
    init();
    // Ensures sfss_init() has been called before any crypto operation

    let mut key0 = ServerKey([0u8; SFSS_KEY_SIZE as usize]);
    let mut key1 = ServerKey([0u8; SFSS_KEY_SIZE as usize]);
    let mut sk0  = StreamKey([0u8; SFSS_STREAM_KEY_SIZE as usize]);
    let mut sk1  = StreamKey([0u8; SFSS_STREAM_KEY_SIZE as usize]);
    // Allocate memory buffers for:
    //   key0 → Leader server key
    //   key1 → Helper server key
    //   sk0  → stream key for share 0
    //   sk1  → stream key for share 1

    let ret = unsafe {
        sfss_sdpf_gen(
            user_id,
            key0.0.as_mut_ptr(),
            key1.0.as_mut_ptr(),
            sk0.0.as_mut_ptr(),
            sk1.0.as_mut_ptr(),
        )
    };
    // Calls C function: sfss_sdpf_gen (from sfss_ffi.cpp)
    // Which internally calls:
    //   SDPF_gen<32>() from sfss.h (C++)
    //
    // What it does:
    //   - Generates two independent server keys (key0, key1)
    //   - Generates two stream keys (sk0, sk1)
    //   - Encodes user_id into the DPF tree
    //
    // Crypto source:
    //   - emp::PRG (randomness)
    //   - TwoKeyPRP (AES-based PRF)
    //   - RDPF / SDPF tree construction

    assert_eq!(ret, 0, "sfss_sdpf_gen failed");
    // Ensures generation succeeded (C function returns 0 on success)

    (key0, key1, sk0, sk1)
    // Returns:
    //   key0 → send to Leader
    //   key1 → send to Helper
    //   sk0/sk1 → kept by Agent for streaming encryption
}

pub fn encrypt(sk0: &mut StreamKey, sk1: &mut StreamKey, value: u8) -> Ctx {
    init();
    // Ensure SFSS initialized

    let mut ctx = Ctx([0u8; SFSS_CTX_SIZE as usize]);
    // Allocate ciphertext buffer

    let ret = unsafe {
        sfss_sdpf_enc(
            sk0.0.as_mut_ptr(),
            sk1.0.as_mut_ptr(),
            value as u32,
            ctx.0.as_mut_ptr(),
        )
    };
    // Calls C function: sfss_sdpf_enc (from sfss_ffi.cpp)
    // Which internally calls:
    //   SDPF_enc<uint32_t>() from sfss.h
    //
    // What it does:
    //   - Uses sk0 and sk1 (stream keys)
    //   - Uses internal counter (ctr)
    //   - Generates masked value using PRF:
    //         value XOR F(k, ctr)
    //   - Produces ciphertext ctx
    //   - Increments ctr inside sk0 and sk1
    //
    // Crypto source:
    //   F(k, ctr) → implemented in field.h using AES PRP
    //   PRP comes from:
    //     TwoKeyPRP (twokeyprp.h → EMP toolkit AES)

    assert_eq!(ret, 0, "sfss_sdpf_enc failed");
    // Ensure encryption succeeded

    ctx
    // Returns encrypted context (sent to Leader + Helper)
}

pub fn eval(key: &ServerKey, ctx: &Ctx, user_id: u32) -> u32 {
    init();
    // Ensure SFSS initialized

    let mut out: u32 = 0;
    // Output share (either share0 or share1)

    let ret = unsafe {
        sfss_sdpf_eval(
            key.0.as_ptr(),
            ctx.0.as_ptr(),
            user_id,
            &mut out,
        )
    };
    // Calls C function: sfss_sdpf_eval (from sfss_ffi.cpp)
    // Which internally calls:
    //   SDPF_eval<uint32_t, 32>() from sfss.h
    //
    // What it does:
    //   - Evaluates DPF tree using server key
    //   - Uses ctx (contains ctr + masked value)
    //   - Checks if input user_id matches encoded point
    //
    // Output behavior:
    //   if user_id == target:
    //       returns share (partial value)
    //   else:
    //       returns 0
    //
    // Security:
    //   - Each server sees ONLY its share
    //   - share alone reveals nothing (information-theoretic split)

    assert_eq!(ret, 0, "sfss_sdpf_eval failed");
    // Ensure evaluation succeeded

    out
    // Return share (to be combined later)
}

pub fn reconstruct(a: u32, b: u32) -> u32 {
    unsafe { sfss_reconstruct(a, b) }
    // Calls C function: sfss_reconstruct (from sfss_ffi.cpp)
    //
    // What it does:
    //   - Combines share0 (a) and share1 (b)
    //   - Returns original value:
    //         value = a + b  (mod 2^32)
    //
    // Source:
    //   Defined in sfss_ffi.cpp
    //   Based on additive secret sharing over Z/2^32
}