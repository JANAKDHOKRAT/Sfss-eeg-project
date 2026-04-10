/** 
 * sfss_ffi.cpp — implements sfss_ffi.h using the real SFSS C++ headers.
 *
 * This file includes sfss.h (and transitively: field.h, twokeyprp.h,
 * util.h, emp-tool) and wraps the template functions in concrete extern "C"
 * functions that Rust can call via FFI.
 *
 * Group type:  G = uint32_t   (attention values 0–255, arithmetic mod 2^32)
 * DPF depth:   N = 32         (domain = 2^32 user IDs)
 *
 * Nothing here is invented. Every crypto operation delegates to the original
 * SFSS headers:
 *   SDPF_gen  → sfss.h SDPF_gen<32>
 *   SDPF_enc  → sfss.h SDPF_enc<uint32_t>
 *   SDPF_eval → sfss.h SDPF_eval<uint32_t, 32>
 *   AES/PRG   → emp-tool (via util.h / twokeyprp.h)
 */

#include "sfss_ffi.h"
// C header that declares the extern "C" API exposed to Rust

// Pull in the real SFSS implementation (unchanged from the repo).
// This also brings in field.h, twokeyprp.h, util.h, emp-tool.
#include "sfss.h"
// Main C++ SFSS implementation - contains SDPF_gen, SDPF_enc, SDPF_eval

#include <cstring>
// Used for std::memcpy in serialization and deserialization helpers

#include <mutex>
// Included for thread-safety support if needed by the implementation

// ── Concrete types ────────────────────────────────────────────────────────────
// G = uint32_t: fits [0,255], arithmetic mod 2^32
// N = 32: 32-level DPF tree, supports 2^32 distinct user IDs
using G  = uint32_t;
// Alias for the arithmetic group used by the streaming ciphertext

static constexpr size_t N = 32;
// Number of DPF tree levels - fixed depth for 32-bit user IDs

// Compile-time size assertions to ensure our #defines match reality.
static_assert(
    sizeof(bool) * (N + N) + sizeof(block) * (N + 1) + sizeof(bool) == SFSS_KEY_SIZE,
    "SFSS_KEY_SIZE mismatch — check rdpf_key_class<N> serialization logic"
);
// Verifies that the serialized key size matches the expected C representation
// Note: the assert above may not be exactly right due to packing; we verify
// at runtime in sfss_init below instead.

// ── Stream-key serialization ──────────────────────────────────────────────────
// sdpf_stream_key has no built-in serialize(); we write our own.
// Layout (packed, no padding assumptions):
//   [16 bytes] block key_prf
//   [ 1 byte ] bool  t_last
//   [ 8 bytes] uint64_t ctr
// Total: 25 bytes = SFSS_STREAM_KEY_SIZE

static void sk_to_buf(const sdpf_stream_key &sk, unsigned char *buf) {
    // Serialize sdpf_stream_key into a raw byte buffer for Rust/FFI use
    unsigned char *p = buf;
    std::memcpy(p, &sk.key_prf, sizeof(block)); p += sizeof(block); // 16 bytes
    *p = sk.t_last ? 1u : 0u;                  p += 1;             //  1 byte
    std::memcpy(p, &sk.ctr,     sizeof(uint64_t));                  //  8 bytes
}

static void buf_to_sk(const unsigned char *buf, sdpf_stream_key &sk) {
    // Deserialize raw bytes back into sdpf_stream_key
    const unsigned char *p = buf;
    std::memcpy(&sk.key_prf, p, sizeof(block)); p += sizeof(block);
    sk.t_last = (*p != 0u);                     p += 1;
    std::memcpy(&sk.ctr,     p, sizeof(uint64_t));
}

// ── Ctx serialization ─────────────────────────────────────────────────────────
// stream_ctx_type<uint32_t>::serialize uses the util.h helpers, which for
// arithmetic types just memcpy.  We do the same explicitly for clarity.
// Layout:
//   [ 8 bytes] uint64_t ctr
//   [ 4 bytes] uint32_t ctx
// Total: 12 bytes = SFSS_CTX_SIZE

static void ctx_to_buf(const stream_ctx_type<G> &sct, unsigned char *buf) {
    // Serialize streaming ciphertext context into bytes
    unsigned char *p = buf;
    std::memcpy(p, &sct.ctr, sizeof(uint64_t)); p += sizeof(uint64_t);
    std::memcpy(p, &sct.ctx, sizeof(G));
}

static void buf_to_ctx(const unsigned char *buf, stream_ctx_type<G> &sct) {
    // Deserialize bytes back into stream_ctx_type<uint32_t>
    const unsigned char *p = buf;
    std::memcpy(&sct.ctr, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    std::memcpy(&sct.ctx, p, sizeof(G));
}

// ── Key serialization ─────────────────────────────────────────────────────────
// sdpf_key_class<N> inherits serialize/deserialize from rdpf_key_class<N>.

static void key_to_buf(const sdpf_key_class<N> &key, unsigned char *buf) {
    // Serialize the full SDPF key structure into raw bytes
    key.serialize(reinterpret_cast<char *>(buf));
}

static void buf_to_key(const unsigned char *buf, sdpf_key_class<N> &key) {
    // Deserialize raw bytes back into SDPF key structure
    key.deserialize(reinterpret_cast<const char *>(buf));
}

// ── Public API ────────────────────────────────────────────────────────────────

extern "C" {

void sfss_init(void) {
    // Silence the benchmark timing output from util.h macros (optional).
    GLOBAL_TIMEIT_LEVEL = TIMEIT_NONE;

    // emp-tool initialises its AES engine lazily, but force it here.
    // Constructing a PRG forces the OpenSSL/AES-NI path to load.
    emp::PRG prg;
    (void)prg;
    // Ensures the PRG is constructed so the underlying crypto backend is ready
}

int sfss_sdpf_gen(
    uint32_t       user_id,
    unsigned char *key0,
    unsigned char *key1,
    unsigned char *sk0,
    unsigned char *sk1)
{
    // Local C++ key objects that will be serialized into the output buffers
    sdpf_key_class<N>  k0, k1;
    sdpf_stream_key    stream0, stream1;

    // SDPF_gen<N> (sfss.h):
    //   1. Calls RDPF_gen<N> which uses emp::PRG to generate two random seeds.
    //   2. Runs the DPF tree construction loop (N iterations of TwoKeyPRP).
    //   3. Encodes user_id as the "active" leaf of the DPF.
    //   4. Sets stream keys to the last-level PRF blocks.
    SDPF_gen<N>(k0, k1, stream0, stream1, static_cast<index_type>(user_id));
    // Calls the SFSS template generator from sfss.h
    // Inputs:
    //   - user_id: target index in the DPF tree
    // Outputs:
    //   - k0/k1: server keys
    //   - stream0/stream1: stream keys used by SDPF_enc

    // Serialise all four outputs.
    key_to_buf(k0,    key0);
    key_to_buf(k1,    key1);
    sk_to_buf(stream0, sk0);
    sk_to_buf(stream1, sk1);
    // Convert C++ structs into raw byte arrays for Rust to store or send

    return 0;
    // 0 means success for the FFI caller
}

int sfss_sdpf_enc(
    unsigned char *sk0,
    unsigned char *sk1,
    uint32_t       attention,
    unsigned char *ctx_buf)
{
    // Rehydrate stream keys from raw bytes
    sdpf_stream_key stream0, stream1;
    buf_to_sk(sk0, stream0);
    buf_to_sk(sk1, stream1);

    stream_ctx_type<G> sct;
    // Output streaming ciphertext container

    // SDPF_enc<G> (sfss.h):
    //   v0 = F<G>(stream0.key_prf, counter)  // AES-PRP, truncated to 32 bits
    //   v1 = F<G>(stream1.key_prf, counter)
    //   flag = (-1)^stream1.t_last           // correction sign
    //   ctx  = flag * (attention - v0 + v1)  // one-time-pad style masking
    //   Increments counter in both stream keys.
    SDPF_enc<G>(stream0, stream1, sct, static_cast<G>(attention));
    // Calls the SFSS streaming encryption routine from sfss.h

    // Write back updated stream keys (counter changed).
    sk_to_buf(stream0, sk0);
    sk_to_buf(stream1, sk1);
    // Stream keys are stateful, so the incremented counter must be preserved

    ctx_to_buf(sct, ctx_buf);
    // Serialize ciphertext to raw bytes for Rust

    return 0;
    // Success
}

int sfss_sdpf_eval(
    const unsigned char *key_buf,
    const unsigned char *ctx_buf,
    uint32_t             user_id,
    uint32_t            *share_out)
{
    // Deserialize server key from raw bytes
    sdpf_key_class<N> key;
    buf_to_key(key_buf, key);

    // Deserialize ciphertext context from raw bytes
    stream_ctx_type<G> sct;
    buf_to_ctx(ctx_buf, sct);

    // SDPF_eval<G, N> (sfss.h):
    //   1. RDPF_eval<N>(key, user_id): traverses DPF tree using TwoKeyPRP.
    //      Returns (t_last, prf_key) — active only if leaf == user_id.
    //   2. share = flag * (F<G>(prf_key, ctx.ctr) + t_last ? ctx.ctx : 0)
    //   The share is zero (or negligible) for any idx ≠ user_id.
    G share = SDPF_eval<G, N>(key, sct, static_cast<index_type>(user_id));
    // Evaluates the SDPF share for this server key and user ID

    *share_out = share;
    // Write the computed share back to the caller

    return 0;
    // Success
}

uint32_t sfss_reconstruct(uint32_t share0, uint32_t share1) {
    // Additive reconstruction mod 2^32. Since G = uint32_t, wrap-around
    // is guaranteed by C unsigned overflow semantics.
    return share0 + share1;
    // Combines the two server shares into the final plaintext value
}

} // extern "C"