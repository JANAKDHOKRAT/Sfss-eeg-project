/**
 * sfss_ffi.h — C-compatible interface to the SFSS C++ library.
 *
 * What this exposes:
 *   The SDPF (Streaming Distributed Point Function) from sfss.h,
 *   wrapped in extern "C" so Rust can call it via FFI.
 *
 * Crypto stack (all from the SFSS repo, unchanged):
 *   - TwoKeyPRP (twokeyprp.h): AES-based PRG, G(k) = AES_k0(x) XOR x || AES_k1(x) XOR x
 *   - F<G>(block, counter): AES-PRP pseudorandom function used for streaming masks
 *   - RDPF_gen / RDPF_eval (sfss.h): Randomized DPF tree over the 2^32 domain
 *   - SDPF_gen / SDPF_enc / SDPF_eval (sfss.h): Streaming DPF full protocol
 *   - emp::PRG (emp-tool): OS-backed CSPRNG used in key generation
 *
 * Sizes (N=32, G=uint32_t):
 *   SFSS_KEY_SIZE        = 593  bytes  — sdpf_key_class<32> serialized
 *   SFSS_STREAM_KEY_SIZE = 25   bytes  — sdpf_stream_key serialized
 *   SFSS_CTX_SIZE        = 12   bytes  — stream_ctx_type<uint32_t> serialized
 */

// Header guard-style setup for C and C++ builds
#pragma once

#include <stdint.h>
// Provides fixed-width integer types like uint32_t

#ifdef __cplusplus
extern "C" {
#endif
// Exposes the declarations with C linkage so Rust FFI can link to them safely

#define SFSS_KEY_SIZE        593   /* sdpf_key_class<32> serialized bytes */
// Size of one serialized SDPF server key buffer

#define SFSS_STREAM_KEY_SIZE  25   /* sdpf_stream_key serialized bytes   */
// Size of one serialized streaming key buffer

#define SFSS_CTX_SIZE         12   /* stream_ctx_type<uint32_t> bytes     */
// Size of one serialized ciphertext/context buffer

/**
 * sfss_init — must be called once before any other function.
 * Initialises the emp-tool AES hardware context.
 */
void sfss_init(void);
// One-time library initialization function
// Implemented in sfss_ffi.cpp
// Typically prepares EMP / AES / PRG state

/**
 * sfss_sdpf_gen — generates all keys for one user session.
 *
 * user_id : 32-bit identity of the agent (domain of the DPF tree).
 *
 * Outputs (all caller-allocated with the exact sizes above):
 *   key0    — SDPF server key for Leader   (send once via POST /setup)
 *   key1    — SDPF server key for Helper   (send once via POST /setup)
 *   sk0     — streaming key share 0        (agent keeps, updated each enc)
 *   sk1     — streaming key share 1        (agent keeps, updated each enc)
 *
 * Security: key0 and key1 are generated from independent AES seeds
 * via emp::PRG (hardware CSPRNG). Neither key reveals anything about
 * future messages; their XOR structure is what encodes the user_id.
 *
 * Returns 0 on success.
 */
int sfss_sdpf_gen(
    uint32_t        user_id,
    unsigned char  *key0,   /* SFSS_KEY_SIZE bytes */
// Output buffer for Leader's server key

    unsigned char  *key1,   /* SFSS_KEY_SIZE bytes */
// Output buffer for Helper's server key

    unsigned char  *sk0,    /* SFSS_STREAM_KEY_SIZE bytes */
// Output buffer for first stream key share

    unsigned char  *sk1     /* SFSS_STREAM_KEY_SIZE bytes */
// Output buffer for second stream key share
);

/**
 * sfss_sdpf_enc — encrypts one attention value.
 *
 * Takes sk0 and sk1 IN/OUT — both are updated (counter incremented).
 * The same ctx must be sent to BOTH Leader and Helper.
 *
 * Internally computes:
 *   v0 = AES(sk0.key_prf, counter)  [mod 2^32]
 *   v1 = AES(sk1.key_prf, counter)  [mod 2^32]
 *   ctx = flag * (attention - v0 + v1)   where flag = (-1)^t1_last
 *
 * Security: ctx is computationally indistinguishable from random to
 * anyone who does not hold BOTH sk0 and sk1.
 *
 * Returns 0 on success.
 */
int sfss_sdpf_enc(
    unsigned char  *sk0,       /* SFSS_STREAM_KEY_SIZE bytes, IN/OUT */
// First stream key buffer, updated in place because the counter advances

    unsigned char  *sk1,       /* SFSS_STREAM_KEY_SIZE bytes, IN/OUT */
// Second stream key buffer, updated in place because the counter advances

    uint32_t        attention,
// Plain attention value to encrypt

    unsigned char  *ctx        /* SFSS_CTX_SIZE bytes, OUT */
// Output ciphertext/context buffer
);

/**
 * sfss_sdpf_eval — server-side evaluation.
 *
 * Called by Leader (with key0) or Helper (with key1).
 * Evaluates the DPF at user_id, then uses the result to unmask
 * the share of attention from ctx.
 *
 * Returns via share_out the additive share for this server.
 * share0 + share1 (mod 2^32) = attention value from that tick.
 *
 * Returns 0 on success.
 */
int sfss_sdpf_eval(
    const unsigned char  *key,       /* SFSS_KEY_SIZE bytes     */
// Input serialized server key

    const unsigned char  *ctx,       /* SFSS_CTX_SIZE bytes     */
// Input serialized ciphertext/context

    uint32_t              user_id,
// User index to evaluate against the DPF tree

    uint32_t             *share_out
// Output pointer for the computed additive share
);

/**
 * sfss_reconstruct — combines two accumulated shares.
 * Simple: (share0 + share1) mod 2^32.
 */
uint32_t sfss_reconstruct(uint32_t share0, uint32_t share1);
// Final reconstruction function, typically used by the reconstruct service

#ifdef __cplusplus
}
#endif
// Ends the C linkage block for C++ compilation