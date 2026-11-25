/* Copyright 2026 Matthew Stan. PolyForm Noncommercial 1.0.0. */
#ifndef RECOMP_MOD_H
#define RECOMP_MOD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rmod_context rmod_context;
/* Calls on one context must be serialized. Different contexts are independent.
 * Engine and plugin callbacks must not throw across the C interface.
 * All strings are UTF-8. Error text remains valid until the next mutation. */
/* SHA-256 identity for original loaded bytes or a resolved-plan description. */
int rmod_sha256_bytes(const void* data,size_t size,uint8_t out[32]);
rmod_context* rmod_create(void);
void rmod_destroy(rmod_context*);
int rmod_initialize(rmod_context*, const char* catalog_root, const char* game_id,
                    const char* original_image_sha256);
/* Import an engine-resolved plan when the engine owns richer manifest
 * extensions. Caller must already validate catalog selection, target hash and
 * resource contents. The core binds that identity and still validates every
 * operation, expected byte, overlap and rollback during rmod_apply. */
int rmod_bind_plan(rmod_context*,const char* game_id,const char* original_sha256,
                   const char* resolved_plan_sha256);
int rmod_install(rmod_context*, const char* archive_path);
int rmod_enable(rmod_context*, const char* package, const char* feature, int enabled);
int rmod_option_set(rmod_context*, const char* package, const char* feature,
                     const char* option, const char* value);
int rmod_resource_set(rmod_context*, const char* package, const char* feature,
                       const char* resource, const char* original_image_path);
/* Reads and hashes the original image; never opens it for writing. */
int rmod_commit(rmod_context*, const char* original_image_path);
void rmod_identity(const rmod_context*, uint8_t out[32]);
const char* rmod_error(const rmod_context*);
typedef void (*rmod_plugin_callback)(void);
/* Only statically trusted callbacks can be selected by packages. */
int rmod_register_plugin(rmod_context*, const char* id, const char* exclusive_resource,
                          rmod_plugin_callback);
int rmod_activate(rmod_context*);
int rmod_option_get(rmod_context*, const char* id, char* out, unsigned capacity);
int rmod_asset_get(rmod_context*, const char* relative, char* out, unsigned capacity);
int rmod_resource_get(rmod_context*, const char* id, char* out, unsigned capacity);
typedef struct rmod_feature_info {
    const char* package_id;
    const char* feature_id;
    const char* name;
    const char* description;
    int enabled;
} rmod_feature_info;
/* Borrowed strings remain valid until a catalog mutation. */
size_t rmod_feature_count(rmod_context*);
int rmod_feature_at(rmod_context*, size_t index, rmod_feature_info*);
typedef struct rmod_diagnostic_info {
    const char* package_id;
    const char* feature_id;
    const char* related_package_id;
    const char* related_feature_id;
    const char* resource;
    const char* message;
} rmod_diagnostic_info;
size_t rmod_diagnostic_count(rmod_context*);
int rmod_diagnostic_at(rmod_context*, size_t index, rmod_diagnostic_info*);

/* Space is a consumer-defined stable id (e.g. "ram", "cartridge"). Both are
 * valid writable destinations. Disk files are never a patch destination.
 * expected/replacement arrays must both contain size bytes. */
typedef struct rmod_patch {
    const char* space;
    uint64_t address;
    const uint8_t* expected;
    const uint8_t* replacement;
    size_t size;
} rmod_patch;
typedef struct rmod_hook {
    const char* id;
} rmod_hook;
typedef struct rmod_engine {
    void* user;
    /* Identity of the engine's loaded input, retained across in-memory mods. */
    const char* game_id;
    const char* original_image_sha256;
    /* begin holds engine mutation/execution exclusively through finish.
     * begin failure must leave the engine unchanged. */
    int (*begin)(void*);
    int (*read)(void*, const char*, uint64_t, uint8_t*, size_t);
    int (*write)(void*, const char*, uint64_t, const uint8_t*, size_t);
    int (*hook)(void*, const char*);
    /* finish(false) must roll back hook/private changes. Core restores bytes
     * before finish(false). finish(true) must be atomic and non-failing. */
    void (*finish)(void*, int committed);
} rmod_engine;
/* Validates all expected bytes and overlapping claims before mutation. On a
 * write/hook failure restores bytes and requests engine rollback. Returns -1
 * if restoration fails: caller must stop guest execution. Plan identity binds
 * the committed catalog plus exact operations, independent of host paths. */
int rmod_apply(rmod_context*, const rmod_patch*, size_t patch_count,
                 const rmod_hook*, size_t hook_count, const rmod_engine*,
                 uint8_t plan_identity[32]);
#ifdef __cplusplus
}
#endif
#endif
