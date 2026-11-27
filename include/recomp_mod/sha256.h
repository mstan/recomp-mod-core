/* SPDX-License-Identifier: MIT */
#ifndef RECOMP_MOD_SHA256_H
#define RECOMP_MOD_SHA256_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller-owned incremental state. Initialize before update/final; do not
 * access its fields or overlap input with the state. NULL data is permitted
 * only for an empty update. Final writes 32 bytes; initialize to reuse. */
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    size_t used;
    uint8_t block[64];
} rmod_sha256_state;
void rmod_sha256_init(rmod_sha256_state *);
void rmod_sha256_update(rmod_sha256_state *, const uint8_t *, size_t);
void rmod_sha256_final(rmod_sha256_state *, uint8_t out[32]);
/* One-shot computation; output may overlap input. For checked arguments use
 * rmod_sha256_bytes from recomp_mod.h. */
void rmod_sha256_compute(const uint8_t *, size_t, uint8_t out[32]);
#ifdef __cplusplus
}
#endif
#endif
