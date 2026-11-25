/* Adapted from snesrecomp; PolyForm Noncommercial 1.0.0.
 * See ../LICENSE. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>


namespace RecompMod {

/*
 * Initialize the package catalog rooted at <exe>/mods for one verified game.
 * The ROM digest is the canonical lowercase SHA-256 used by package targets.
 */
bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            const std::string& rom_sha256,
                            std::string* error = nullptr);

/*
 * Resolve the staged feature selections, validate the selected ROM, persist
 * state, and prepare the trusted-plugin activation plan.
 */
bool mod_runtime_commit(const std::filesystem::path& rom_path = {},
                        std::string* error = nullptr);

/* Invoke the trusted, statically linked plugins selected by the committed plan. */
void mod_runtime_activate_plugins();


}  // namespace RecompMod
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*InternalModActivationCallback)(void);
typedef void (*InternalModFrameCallback)(void);

struct RecompLauncherCModProvider;

int internal_mod_runtime_initialize_c(const char* root,
                                  const char* game_id,
                                  const char* rom_sha256);
int internal_mod_runtime_commit_c(const char* rom_path);
void internal_mod_runtime_activate_plugins_c(void);
/* Fixed at a successful commit. Hashes enabled features/options and exact
 * package/resource contents, excluding host paths and timestamps. Failed
 * staged plans keep the previous identity. No polling during a match. */
void internal_mod_runtime_identity_c(uint8_t out[32]);
const struct RecompLauncherCModProvider*
internal_mod_runtime_launcher_provider_c(void);
const char* internal_mod_runtime_last_error_c(void);

/*
 * Register a trusted implementation. A .vbmod archive may select only this
 * stable id; archives never provide native code, symbols, or library paths.
 */
int internal_mod_register_activation_plugin(const char* id,
                                        InternalModActivationCallback callback);
/* Optional exclusive resource, e.g. "video.renderer". Conflicts are resolved
 * before activation, even when two features select different plugin IDs. */
int internal_mod_register_exclusive_plugin(const char* id, const char* resource,
                                      InternalModActivationCallback callback);

/*
 * Register game-owned startup policy used to make a mod authoritative over a
 * legacy config flag. The reset callback runs before active plugins, so a
 * disabled feature reliably restores the stock behavior on every launch.
 */
int internal_mod_register_reset_callback(InternalModActivationCallback callback);

/* Frame callbacks run at guest GAME_START, independent of host presentation.
 * Register inside plugin activation: activation/deactivation clears this list. */
int internal_mod_register_frame_callback(InternalModFrameCallback callback);
void internal_mod_runtime_frame_tick_c(void);

/* Headless and GUI hosts share the same package state. Mutations are staged;
 * commit verifies the original ROM and persists them before activation. */
int internal_mod_install_archive(const char* path);
int internal_mod_enable(const char* package, const char* feature, int enabled);
int internal_mod_set_option(const char* package, const char* feature,
                      const char* option, const char* value);
/* Activation callback scope only: copied values, never borrowed UI storage. */
int internal_mod_option(const char* option, char* out, unsigned capacity);
int internal_mod_asset(const char* relative_path, char* out, unsigned capacity);
/* Explicit owner-selected file/folder, validated and snapshotted at commit. */
int internal_mod_resource(const char* id, char* out, unsigned capacity);
void internal_mod_runtime_deactivate(void);


#ifdef __cplusplus
}
#endif
