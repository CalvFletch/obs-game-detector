#pragma once

#include "gd_types.h"

#include <stddef.h>
#include <stdint.h>

#define GD_MAX_AUDIO_TRACKS 6

typedef struct {
	int track_nums[GD_MAX_AUDIO_TRACKS];
	int track_count;
	uint32_t mask;
} GD_RecTracks;

#ifdef __cplusplus
extern "C" {
#endif

// lifecycle (plugin-main)
void gd_start(void);
void gd_stop(void);
void gd_teardown(void);
void gd_open_dialog(void);

// state
GD_State *gd_state(void);

// index + config (dialog, engine, watch)
void gd_config_load(GD_State *state);
bool gd_config_apply(GD_State *state, const GD_ConfigSnap *scratch);
const GD_ConfigRecord *gd_config_find(const GD_ConfigSnap *cfg, GD_GameId id);
bool gd_config_is_enabled(const GD_ConfigSnap *cfg, GD_GameId id);
uint32_t gd_config_mixer_mask(const GD_ConfigSnap *cfg, GD_GameId id);
void gd_config_record_game(GD_State *state, GD_GameId id, const char *display_name);

void gd_recording_tracks(GD_RecTracks *out);
uint32_t gd_tracks_sanitize_mask(uint32_t mask, uint32_t allowed);

bool gd_install_index_load_or_build(GD_InstallIndex *idx, const char *cache_path);
void gd_install_index_build(GD_InstallIndex *idx);
void gd_install_index_save(const GD_InstallIndex *idx, const char *cache_path);
void gd_install_index_rebuild_async(GD_State *state);
void gd_install_index_worker_join(void);
uint64_t gd_install_index_normalize_built_ms(uint64_t raw);
void gd_index_apply_ready(GD_State *state, uint64_t built_ms);

void gd_lookup_build(GD_LookupTable *out, const GD_InstallIndex *idx, const GD_ConfigSnap *cfg);
bool gd_lookup_path_matches(const GD_LookupTable *lt, const char *path_lower);
bool gd_lookup_resolve(const GD_State *state, const char *full_path, GD_GameId *id_out, char *display_out,
		       size_t display_cap, char *install_dir_out, size_t install_cap);
bool gd_lookup_matches_full_path(const GD_LookupTable *lt, const char *full_path);
bool gd_lookup_dir_covered_by_index(const GD_InstallIndex *idx, const char *dir);
void gd_lookup_default_dirs(const GD_InstallIndex *idx, char dirs[][GD_MAX_PATH], int *count, int cap);
bool gd_lookup_sanitize_index_dirs(GD_InstallIndex *idx);
GD_GameId gd_index_id_for_display(const GD_InstallIndex *idx, const char *display_name);
const char *gd_index_display_name(const GD_InstallIndex *idx, GD_GameId id);

void gd_strlcpy(char *dst, const char *src, size_t cap);
void gd_strlower(char *dst, const char *src, size_t cap);
void gd_game_id_to_hex(GD_GameId id, char *out, size_t cap);
bool gd_game_id_from_hex(const char *hex, GD_GameId *out);
bool gd_dir_add_unique(char dirs[][GD_MAX_PATH], int *count, int cap, const char *path);
uint64_t gd_wall_ms(void);
bool gd_install_index_cache_path(char *out, size_t cap);

// engine (dialog) obs mutations are queued, run on processor_tick
void gd_request_remove_source_by_id(GD_GameId id);
void gd_request_index_rebuild(void);
void gd_request_apply_audio_tracks(void);
void gd_request_sync_scenes(void);
void gd_ensure_group_in_scene(const char *scene_name);

// watch
void gd_watch_snapshot(const GD_LookupTable *lt);
bool gd_watch_start(void);
void gd_watch_stop(void);
void gd_watch_arm_exit(DWORD pid, const char *exe_lower);
void gd_watch_disarm_exit(DWORD pid);

// event post (watch via state->post_event; engine wires this)
void gd_event_post(const GD_Event *evt);

#ifdef __cplusplus
}
#endif
