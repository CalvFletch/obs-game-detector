#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GD_MAX_GAMES        64
#define GD_MAP_CAP          512
#define GD_MAX_PATH         1024
#define GD_MAX_LOOKUP_DIRS  128
#define GD_MAX_SCENES       64
#define GD_MAX_SCENE_LEN    256
#define GD_EVENT_QUEUE_CAP  256

#define GD_DEFAULT_GROUP    "Game Audio"
#define GD_COLOR_ORANGE     7
#define GD_INDEX_REBUILD_MS (30ULL * 60 * 1000)

#define GD_SOURCE_KEY_GAME_ID "gd_game_id"
#define GD_SOURCE_KEY_PID     "gd_pid"

typedef uint64_t GD_GameId;

typedef enum {
	GD_GAME_DETECTED,
	GD_GAME_CAPTURING,
	GD_GAME_STOPPING,
} GD_GameState;

typedef struct {
	GD_GameId id;
	char install_dir[GD_MAX_PATH];
	char display_name[256];
} GD_InstallEntry;

typedef struct {
	GD_InstallEntry entries[GD_MAP_CAP];
	int entry_count;
	char lookup_dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int lookup_dir_count;
	uint64_t built_ms;
} GD_InstallIndex;

typedef struct {
	GD_GameId id;
	DWORD pid;
	GD_GameState state;
	char display_name[256];
	char exe_lower[GD_MAX_PATH];
	char capture_exe[GD_MAX_PATH];
	char install_dir[GD_MAX_PATH];
	char obs_source_name[256];
	bool reinstate_connected;
	DWORD reinstate_hook_pid;
} GD_TrackedGame;

typedef struct {
	GD_TrackedGame games[GD_MAX_GAMES];
	int count;
} GD_Tracker;

typedef struct {
	GD_GameId id;
	char display_name[256];
	char last_seen[16];
	bool enabled;
	uint32_t tracks;
	bool tracks_override;
} GD_ConfigRecord;

typedef struct GD_ConfigSnap {
	GD_ConfigRecord games[GD_MAX_GAMES];
	int game_count;
	uint32_t default_tracks;
	char custom_dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int custom_dir_count;
	/* Dirs (discovered or custom) excluded from game lookup. */
	char hidden_dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int hidden_dir_count;
	char scenes[GD_MAX_SCENES][GD_MAX_SCENE_LEN];
	int scene_count;
} GD_ConfigSnap;

typedef struct {
	char dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int dir_count;
} GD_LookupTable;

typedef enum {
	GD_EVT_PROCESS_START,
	GD_EVT_PROCESS_STOP,
	GD_EVT_REBUILD_INDEX,
	GD_EVT_INDEX_READY,
	GD_EVT_SYNC_SCENES,
	GD_EVT_REMOVE_BY_ID,
	GD_EVT_ADD_BY_ID,
	GD_EVT_APPLY_TRACKS,
	GD_EVT_PLACE_AUDIO,
} GD_EventKind;

typedef struct {
	GD_EventKind kind;
	DWORD pid;
	GD_GameId game_id;
	char exe_lower[GD_MAX_PATH];
	char full_path[GD_MAX_PATH];
	uint64_t index_built_ms;
} GD_Event;

#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	GD_PHASE_IDLE,
	GD_PHASE_BOOT_RECONCILE,
	GD_PHASE_RUNNING,
	GD_PHASE_STOPPING,
} GD_Phase;

typedef struct {
	pthread_mutex_t mutex;
	GD_Event queue[GD_EVENT_QUEUE_CAP];
	int head;
	int tail;
} GD_EventRing;

typedef struct {
	GD_Phase phase;
	GD_InstallIndex index;
	GD_InstallIndex index_scratch;
	GD_ConfigSnap config;
	GD_LookupTable lookup;
	GD_Tracker tracker;
	GD_EventRing events;
	uint64_t index_built_ms;
	bool index_rebuild_pending;
	bool obs_tick_scheduled;
	int obs_processor_depth;
	void (*schedule_processor)(void);
	void (*post_event)(const GD_Event *evt);
	/* Scene item IDs for the "Game Audio" group item in each target scene.
	 * Index corresponds to config.scenes[]. 0 means not yet placed. */
	int64_t grp_item_ids[GD_MAX_SCENES];
} GD_State;

static inline const GD_ConfigSnap *gd_config_of(const GD_State *state)
{
	return state ? &state->config : NULL;
}

#ifdef __cplusplus
}
#endif
