#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute SHA-256 of `len` bytes at `data`. Writes 32 bytes to `out`.
 * Self-contained reference implementation (public domain). */
typedef struct { uint32_t h[8]; uint64_t bytes; size_t used; uint8_t block[64]; } rmod_sha256_state;
void rmod_sha256_init(rmod_sha256_state*);
void rmod_sha256_update(rmod_sha256_state*, const uint8_t*, size_t);
void rmod_sha256_final(rmod_sha256_state*, uint8_t out[32]);
void rmod_sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
