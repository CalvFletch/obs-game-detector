#include "gd_api.h"
#include "gd_skip_rules.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <callback/calldata.h>

#include <QCoreApplication>
#include <QMainWindow>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define GD_GROUP_NAME GD_DEFAULT_GROUP
#define GD_COLOR      GD_COLOR_ORANGE

#define TRACK_BY_PID     0
#define TRACK_BY_GAME_ID 1
#define TRACK_BY_BOTH    2

static GD_State g_state;

static GD_Event s_event_batch[GD_EVENT_QUEUE_CAP];

// tracker

static void tracker_init(GD_Tracker *t)
{
	memset(t, 0, sizeof(*t));
}

typedef bool (*tracker_match_fn)(GD_TrackedGame *g, void *ctx);

static int tracker_scan(GD_Tracker *t, GD_GameId id, DWORD pid, bool match_id, bool match_pid, tracker_match_fn fn,
			void *ctx)
{
	int n = 0;
	for (int i = 0; i < t->count; i++) {
		GD_TrackedGame *g = &t->games[i];
		if (match_id && g->id != id)
			continue;
		if (match_pid && g->pid != pid)
			continue;
		n++;
		if (fn && !fn(g, ctx))
			return n;
	}
	return n;
}

typedef struct {
	GD_TrackedGame *found;
} tracker_find_ctx;

static bool tracker_find_stop(GD_TrackedGame *g, void *vp)
{
	((tracker_find_ctx *)vp)->found = g;
	return false;
}

static GD_TrackedGame *tracker_find(GD_Tracker *t, GD_GameId id, DWORD pid, int mode)
{
	bool match_id = (mode == TRACK_BY_GAME_ID || mode == TRACK_BY_BOTH);
	bool match_pid = (mode == TRACK_BY_PID || mode == TRACK_BY_BOTH);
	tracker_find_ctx ctx = {NULL};
	tracker_scan(t, id, pid, match_id, match_pid, tracker_find_stop, &ctx);
	return ctx.found;
}

static int tracker_count_by_id(const GD_Tracker *t, GD_GameId id)
{
	return tracker_scan((GD_Tracker *)t, id, 0, true, false, NULL, NULL);
}

typedef void (*tracker_visit_fn)(GD_TrackedGame *g, void *ctx);

static void tracker_foreach_by_id(GD_Tracker *t, GD_GameId id, tracker_visit_fn fn, void *ctx)
{
	for (int i = 0; i < t->count; i++)
		if (t->games[i].id == id)
			fn(&t->games[i], ctx);
}

static void tracker_visit(GD_Tracker *t, tracker_visit_fn fn, void *ctx)
{
	for (int i = 0; i < t->count; i++)
		fn(&t->games[i], ctx);
}

static void make_obs_source_name(GD_State *state, const char *display_name, DWORD pid, GD_GameId id, char *out,
				 size_t cap)
{
	int same_id = tracker_count_by_id(&state->tracker, id);
	bool need_pid = same_id > 0 || (tracker_find(&state->tracker, id, pid, TRACK_BY_BOTH) != NULL);

	if (!need_pid) {
		gd_strlcpy(out, display_name, cap);
		return;
	}

	snprintf(out, cap, "%s (%lu)", display_name, (unsigned long)pid);
}

static void strip_battleye(const char *exe, char *capture_exe, size_t cap)
{
	gd_strlcpy(capture_exe, exe, cap);
	size_t elen = strlen(exe);
	if (elen > 7 && strcmp(exe + elen - 7, "_be.exe") == 0) {
		char base[GD_MAX_PATH];
		gd_strlcpy(base, exe, sizeof(base));
		char *be = strstr(base, "_be.exe");
		if (be) {
			memcpy(be, ".exe\0", 5);
			gd_strlcpy(capture_exe, base, cap);
		}
	}
}

// obs (forward decls)

typedef enum {
	OBS_MUT_RUNNING,
} ObsMutMode;

static void obs_mut_begin(GD_State *state, ObsMutMode mode);
static void obs_mut_end(void);
static void obs_mut_assert_active(void);
static void obs_mut_remove_capture(obs_source_t *src);
static void obs_mut_remove_capture_by_name(const char *name);
static void obs_mut_sceneitem_remove(obs_sceneitem_t *item);
static void remove_source_from_group(const char *name);

static GD_State *obs_state(void);
static void place_audio(GD_State *state, GD_TrackedGame *g, bool notify);
static void remove_audio(GD_State *state, GD_TrackedGame *g);
static void remove_audio_by_id(GD_State *state, GD_GameId id, DWORD pid, const char *name_fallback);
static void sync_group_to_scenes(void);
static void apply_audio_tracks(GD_State *state);
static void handle_remove_by_id(GD_State *state, GD_GameId id);
static void handle_add_by_id(GD_State *state, GD_GameId id);
static void obs_teardown(GD_State *state);
static void schedule_tick(void);
static void schedule_processor(void);
static void processor_tick(void *unused);
static void boot_reconcile(GD_State *state);
static void schedule_boot_reconcile(void);
static void request_place_audio(GD_State *state, GD_GameId id, DWORD pid);
static void handle_place_audio(GD_State *state, GD_GameId id);
static void reinstate_disconnect(GD_TrackedGame *g);
static void remove_all_captures_for_id(GD_GameId id);

// event ring

static bool event_pending(void)
{
	GD_EventRing *ring = &g_state.events;
	pthread_mutex_lock(&ring->mutex);
	bool pending = ring->head != ring->tail;
	pthread_mutex_unlock(&ring->mutex);
	return pending;
}

static void event_reset(GD_State *state)
{
	if (!state)
		return;

	GD_EventRing *ring = &state->events;
	pthread_mutex_lock(&ring->mutex);
	ring->head = ring->tail = 0;
	pthread_mutex_unlock(&ring->mutex);
}

static bool pop_event_batch(GD_State *state, GD_Event *batch, int *n, int cap)
{
	*n = 0;

	GD_EventRing *ring = &state->events;
	pthread_mutex_lock(&ring->mutex);
	while (ring->head != ring->tail && *n < cap) {
		batch[(*n)++] = ring->queue[ring->head];
		ring->head = (ring->head + 1) % GD_EVENT_QUEUE_CAP;
	}
	pthread_mutex_unlock(&ring->mutex);
	return *n > 0;
}

static void coalesce_events(GD_Event *evts, int *count)
{
	// coalesce in place; a second batch[256] on the stack was another ~512KB
	// and overflowed same as the boot snapshot
	int n = 0;

	for (int i = 0; i < *count; i++) {
		GD_Event e = evts[i];
		bool merged = false;

		if (e.kind == GD_EVT_SYNC_SCENES || e.kind == GD_EVT_APPLY_TRACKS) {
			for (int j = 0; j < n; j++) {
				if (evts[j].kind == e.kind) {
					merged = true;
					break;
				}
			}
		} else if (e.kind == GD_EVT_REMOVE_BY_ID) {
			for (int j = 0; j < n; j++) {
				if (evts[j].kind == GD_EVT_REMOVE_BY_ID && evts[j].game_id == e.game_id) {
					merged = true;
					break;
				}
			}
		} else if (e.kind == GD_EVT_PLACE_AUDIO) {
			for (int j = 0; j < n; j++) {
				if (evts[j].kind == GD_EVT_PLACE_AUDIO && evts[j].game_id == e.game_id) {
					evts[j].pid = e.pid;
					merged = true;
					break;
				}
			}
		} else if (e.kind == GD_EVT_PROCESS_START) {
			for (int j = 0; j < n; j++) {
				if (evts[j].kind == GD_EVT_PROCESS_START && evts[j].pid == e.pid) {
					evts[j] = e;
					merged = true;
					break;
				}
			}
		} else if (e.kind == GD_EVT_PROCESS_STOP) {
			for (int j = 0; j < n; j++) {
				if (evts[j].kind == GD_EVT_PROCESS_START && evts[j].pid == e.pid) {
					evts[j] = evts[--n];
					merged = true;
					break;
				}
			}
		}

		if (!merged)
			evts[n++] = e;
	}

	*count = n;
}

static void handle_process_start(GD_State *state, const GD_Event *evt)
{
	const char *exe_lower = evt->exe_lower;
	const char *full_path = evt->full_path;

	if (gd_skip_exact(exe_lower))
		return;

	if (!full_path || !full_path[0]) {
		blog(LOG_ERROR, "[obs-game-detector] no image path for pid %lu %s: untracked", (unsigned long)evt->pid,
		     exe_lower);
		return;
	}

	if (tracker_find(&state->tracker, 0, evt->pid, TRACK_BY_PID))
		return;

	if (!gd_lookup_matches_full_path(&state->lookup, full_path)) {
		if (gd_skip_heuristic(exe_lower))
			return;
		blog(LOG_DEBUG, "[obs-game-detector] SKIP (not game path) %s", exe_lower);
		return;
	}

	GD_GameId id;
	char display_name[256];
	char install_dir[GD_MAX_PATH];
	if (!gd_lookup_resolve(state, full_path, &id, display_name, sizeof(display_name), install_dir,
			       sizeof(install_dir))) {
		blog(LOG_ERROR, "[obs-game-detector] install index miss for %s: untracked", full_path);
		return;
	}

	gd_config_record_game(state, id, display_name);

	const GD_ConfigSnap *cfg = &state->config;
	if (!gd_config_is_enabled(cfg, id)) {
		blog(LOG_INFO, "[obs-game-detector] SKIP (disabled) %s -> %s", exe_lower, display_name);
		return;
	}

	char capture_exe[GD_MAX_PATH];
	strip_battleye(exe_lower, capture_exe, sizeof(capture_exe));

	GD_TrackedGame *g = tracker_find(&state->tracker, id, 0, TRACK_BY_GAME_ID);
	if (g) {
		if (g->pid == evt->pid)
			return;

		blog(LOG_INFO, "[obs-game-detector] START pid %lu %s -> %s (pid %lu -> %lu)", (unsigned long)evt->pid,
		     exe_lower, g->obs_source_name, (unsigned long)g->pid, (unsigned long)evt->pid);

		reinstate_disconnect(g);
		gd_watch_disarm_exit(g->pid);
		g->pid = evt->pid;
		g->state = GD_GAME_DETECTED;
		gd_strlcpy(g->exe_lower, exe_lower, sizeof(g->exe_lower));
		gd_strlcpy(g->capture_exe, capture_exe, sizeof(g->capture_exe));
	} else {
		if (state->tracker.count >= GD_MAX_GAMES) {
			blog(LOG_ERROR, "[obs-game-detector] tracked game cap (%d) reached: ignoring pid %lu",
			     GD_MAX_GAMES, (unsigned long)evt->pid);
			return;
		}

		char obs_name[256];
		make_obs_source_name(state, display_name, evt->pid, id, obs_name, sizeof(obs_name));

		g = &state->tracker.games[state->tracker.count++];
		g->id = id;
		g->pid = evt->pid;
		g->state = GD_GAME_DETECTED;
		g->reinstate_connected = false;
		g->reinstate_hook_pid = 0;
		gd_strlcpy(g->display_name, display_name, sizeof(g->display_name));
		gd_strlcpy(g->exe_lower, exe_lower, sizeof(g->exe_lower));
		gd_strlcpy(g->capture_exe, capture_exe, sizeof(g->capture_exe));
		gd_strlcpy(g->install_dir, install_dir, sizeof(g->install_dir));
		gd_strlcpy(g->obs_source_name, obs_name, sizeof(g->obs_source_name));

		blog(LOG_INFO, "[obs-game-detector] START pid %lu %s -> %s", (unsigned long)evt->pid, exe_lower,
		     g->obs_source_name);
	}

	gd_watch_arm_exit(g->pid, g->exe_lower);

	if (state->phase == GD_PHASE_RUNNING)
		place_audio(state, g, true);
}

static void handle_process_stop(GD_State *state, const GD_Event *evt)
{
	GD_TrackedGame *g = tracker_find(&state->tracker, 0, evt->pid, TRACK_BY_PID);
	if (!g)
		return;

	blog(LOG_INFO, "[obs-game-detector] STOP pid %lu %s -> %s", (unsigned long)evt->pid, evt->exe_lower,
	     g->obs_source_name);

	if (state->phase == GD_PHASE_RUNNING)
		remove_audio(state, g);

	gd_watch_disarm_exit(evt->pid);

	for (int i = 0; i < state->tracker.count; i++) {
		if (state->tracker.games[i].pid == evt->pid) {
			state->tracker.games[i] = state->tracker.games[--state->tracker.count];
			break;
		}
	}
}

static void handle_event(GD_State *state, const GD_Event *evt)
{
	if (!state || !evt)
		return;

	switch (evt->kind) {
	case GD_EVT_PROCESS_START:
		handle_process_start(state, evt);
		break;
	case GD_EVT_PROCESS_STOP:
		handle_process_stop(state, evt);
		break;
	case GD_EVT_REBUILD_INDEX:
		if (!state->index_rebuild_pending) {
			state->index_rebuild_pending = true;
			gd_install_index_rebuild_async(state);
		}
		break;
	case GD_EVT_INDEX_READY:
		gd_index_apply_ready(state, evt->index_built_ms);
		break;
	case GD_EVT_SYNC_SCENES:
		sync_group_to_scenes();
		break;
	case GD_EVT_REMOVE_BY_ID:
		handle_remove_by_id(state, evt->game_id);
		break;
	case GD_EVT_ADD_BY_ID:
		handle_add_by_id(state, evt->game_id);
		break;
	case GD_EVT_APPLY_TRACKS:
		apply_audio_tracks(state);
		break;
	case GD_EVT_PLACE_AUDIO:
		handle_place_audio(state, evt->game_id);
		break;
	}
}

static void maybe_schedule_index_rebuild(GD_State *state)
{
	if (!state || state->phase != GD_PHASE_RUNNING || state->index_rebuild_pending)
		return;

	uint64_t now_ms = gd_wall_ms();
	if (now_ms <= state->index_built_ms || now_ms - state->index_built_ms <= GD_INDEX_REBUILD_MS)
		return;

	state->index_rebuild_pending = true;
	GD_Event evt = {};
	evt.kind = GD_EVT_REBUILD_INDEX;
	gd_event_post(&evt);
}

static void processor_tick(void *unused)
{
	(void)unused;
	GD_State *state = gd_state();
	if (!state || state->phase == GD_PHASE_IDLE || state->phase == GD_PHASE_STOPPING)
		return;

	state->obs_tick_scheduled = false;
	state->obs_processor_depth++;

	const bool mut = state->phase == GD_PHASE_RUNNING;
	if (mut)
		obs_mut_begin(state, OBS_MUT_RUNNING);

	if (state->obs_processor_depth == 1)
		maybe_schedule_index_rebuild(state);

	for (;;) {
		int n = 0;
		if (!pop_event_batch(state, s_event_batch, &n, GD_EVENT_QUEUE_CAP))
			break;
		coalesce_events(s_event_batch, &n);
		for (int i = 0; i < n; i++)
			handle_event(state, &s_event_batch[i]);
	}

	if (mut)
		obs_mut_end();

	state->obs_processor_depth--;

	if (state->obs_processor_depth == 0 && event_pending() && state->phase == GD_PHASE_RUNNING)
		schedule_tick();
}

static void schedule_tick(void)
{
	GD_State *state = gd_state();
	if (!state || state->phase == GD_PHASE_IDLE || state->phase == GD_PHASE_STOPPING)
		return;
	if (state->obs_tick_scheduled)
		return;
	state->obs_tick_scheduled = true;
	obs_queue_task(OBS_TASK_UI, processor_tick, NULL, false);
}

static void schedule_processor(void)
{
	GD_State *state = gd_state();
	if (!state || state->phase != GD_PHASE_RUNNING || state->obs_processor_depth > 0)
		return;
	schedule_tick();
}

// obs implementation
//
// all scene/source mutations go through obs_mut_* below
// ui thread only (obs_mut depth > 0 in debug)
// never obs_source_remove / obs_sceneitem_remove inside enum callbacks
// RUNNING phase only for place/remove/dedupe/sync/tracks
// on STOPPING dont call obs_source_remove; ClearSceneData owns teardown
// VolControl widgets still hold refs until ClearVolumeControls runs

static thread_local int s_obs_mut_depth;
static thread_local bool s_gd_removing_captures;

static void obs_mut_begin(GD_State *state, ObsMutMode mode)
{
#if !defined(NDEBUG)
	assert(state != NULL);
	assert(mode == OBS_MUT_RUNNING);
	assert(state->phase == GD_PHASE_RUNNING);
#endif
	(void)state;
	(void)mode;
	s_obs_mut_depth++;
}

static void obs_mut_end(void)
{
#if !defined(NDEBUG)
	assert(s_obs_mut_depth > 0);
#endif
	s_obs_mut_depth--;
}

static void obs_mut_assert_active(void)
{
#if !defined(NDEBUG)
	assert(s_obs_mut_depth > 0);
#endif
}

static void obs_mut_sceneitem_remove(obs_sceneitem_t *item)
{
	obs_mut_assert_active();
	if (item)
		obs_sceneitem_remove(item);
}

static void obs_mut_remove_capture(obs_source_t *src)
{
	obs_mut_assert_active();
	if (!src)
		return;

	const char *name = obs_source_get_name(src);
	blog(LOG_INFO, "[obs-game-detector] removing capture '%s'", name ? name : "?");
	if (name)
		remove_source_from_group(name);
	obs_source_remove(src);
	obs_source_release(src);
}

static void obs_mut_remove_capture_by_name(const char *name)
{
	obs_mut_assert_active();
	if (!name || !name[0])
		return;

	s_gd_removing_captures = true;
	remove_source_from_group(name);
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src) {
		s_gd_removing_captures = false;
		return;
	}

	obs_source_remove(src);
	obs_source_release(src);
	s_gd_removing_captures = false;
	blog(LOG_INFO, "[obs-game-detector] Removed '%s'", name);
}

static GD_State *obs_state(void)
{
	GD_State *state = gd_state();
	if (!state || state->phase == GD_PHASE_IDLE || state->phase == GD_PHASE_STOPPING)
		return NULL;
	return state;
}

static void set_item_color(obs_sceneitem_t *item, int color)
{
	obs_data_t *ps = obs_sceneitem_get_private_settings(item);
	if (ps) {
		obs_data_set_int(ps, "color-preset", color > 0 ? color + 1 : 0);
		obs_data_release(ps);
	}
}

static GD_GameId source_game_id(obs_source_t *src)
{
	obs_data_t *ps = obs_source_get_private_settings(src);
	if (!ps)
		return 0;
	int64_t v = obs_data_get_int(ps, GD_SOURCE_KEY_GAME_ID);
	obs_data_release(ps);
	return (GD_GameId)v;
}

static DWORD source_pid(obs_source_t *src)
{
	obs_data_t *ps = obs_source_get_private_settings(src);
	if (!ps)
		return 0;
	int64_t v = obs_data_get_int(ps, GD_SOURCE_KEY_PID);
	obs_data_release(ps);
	return (DWORD)v;
}

static void tag_source(obs_source_t *src, GD_GameId id, DWORD pid)
{
	obs_data_t *ps = obs_source_get_private_settings(src);
	if (!ps)
		return;
	obs_data_set_int(ps, GD_SOURCE_KEY_GAME_ID, (int64_t)id);
	obs_data_set_int(ps, GD_SOURCE_KEY_PID, (int64_t)pid);
	obs_data_release(ps);
}

static bool is_gd_wasapi_capture(obs_source_t *src)
{
	const char *sid = obs_source_get_id(src);
	return sid && strcmp(sid, "wasapi_process_output_capture") == 0;
}

static void get_target_scenes(const GD_ConfigSnap *cfg, char scenes[][GD_MAX_SCENE_LEN], int *count)
{
	*count = 0;
	if (cfg) {
		*count = cfg->scene_count;
		for (int i = 0; i < cfg->scene_count; i++)
			gd_strlcpy(scenes[i], cfg->scenes[i], GD_MAX_SCENE_LEN);
	}
}

static void show_obs_notification(const char *game_name);

static void on_group_item_removed(void *data, calldata_t *cd)
{
	if (s_gd_removing_captures)
		return;

	DWORD pid = (DWORD)(uintptr_t)data;
	GD_State *state = gd_state();
	if (!state || state->phase != GD_PHASE_RUNNING || !pid)
		return;

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(cd, "item");
	if (!item)
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return;

	GD_GameId gid = source_game_id(src);
	DWORD spid = source_pid(src);
	GD_TrackedGame *g = tracker_find(&state->tracker, 0, spid, TRACK_BY_PID);
	if (!g || g->id != gid || g->pid != pid)
		return;

	const GD_ConfigSnap *cfg = &state->config;
	if (!gd_config_is_enabled(cfg, g->id))
		return;
	if (cfg && cfg->scene_count == 0)
		return;

	blog(LOG_INFO, "[obs-game-detector] Item for '%s' removed from group: reinstating", g->obs_source_name);

	request_place_audio(state, g->id, g->pid);
}

static void reinstate_disconnect(GD_TrackedGame *g)
{
	if (!g || !g->reinstate_connected)
		return;

	DWORD hook_pid = g->reinstate_hook_pid;

	obs_source_t *grp_src = obs_get_source_by_name(GD_GROUP_NAME);
	if (grp_src) {
		if (obs_source_get_id(grp_src) && strcmp(obs_source_get_id(grp_src), "group") == 0) {
			signal_handler_disconnect(obs_source_get_signal_handler(grp_src), "item_remove",
						  on_group_item_removed, (void *)(uintptr_t)hook_pid);
		}
		obs_source_release(grp_src);
	}
	g->reinstate_connected = false;
	g->reinstate_hook_pid = 0;
}

static bool is_group_source(obs_source_t *src)
{
	const char *id = obs_source_get_id(src);
	return id && strcmp(id, "group") == 0;
}

static obs_source_t *get_group_source(bool log_wrong_type)
{
	obs_source_t *grp_src = obs_get_source_by_name(GD_GROUP_NAME);
	if (!grp_src)
		return NULL;
	if (!is_group_source(grp_src)) {
		if (log_wrong_type) {
			blog(LOG_ERROR, "[obs-game-detector] '%s' exists but is not a group source", GD_GROUP_NAME);
		}
		obs_source_release(grp_src);
		return NULL;
	}
	return grp_src;
}

static void reinstate_connect(GD_TrackedGame *g)
{
	if (!g || g->reinstate_connected)
		return;

	obs_source_t *grp_src = get_group_source(false);
	if (!grp_src)
		return;

	g->reinstate_hook_pid = g->pid;
	signal_handler_connect(obs_source_get_signal_handler(grp_src), "item_remove", on_group_item_removed,
			       (void *)(uintptr_t)g->reinstate_hook_pid);
	g->reinstate_connected = true;
	obs_source_release(grp_src);
}

// NOTE(zaddish): scene enum callbacks, libobs ref ownership
// obs_scene_enum_items: item is +1 ref only for the callback; enum releases it
// after return. pls do not ever obs_sceneitem_release(item) here
// obs_sceneitem_get_source(item) is borrowed too so never obs_source_release on it

struct find_group_item_ctx {
	const char *source_name;
	obs_sceneitem_t *found;
};

static bool find_or_scrub_group_item(obs_scene_t *, obs_sceneitem_t *item, void *vp)
{
	auto *ctx = (find_group_item_ctx *)vp;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	const char *name = obs_source_get_name(src);
	bool match = name && ctx->source_name && strcmp(name, ctx->source_name) == 0;
	if (match) {
		ctx->found = item;
		return false;
	}
	return true;
}

static obs_sceneitem_t *find_live_item_in_group(obs_scene_t *grp_scene, const char *source_name)
{
	if (!grp_scene || !source_name || !source_name[0])
		return NULL;

	find_group_item_ctx ctx = {source_name, NULL};
	obs_scene_enum_items(grp_scene, find_or_scrub_group_item, &ctx);
	return ctx.found;
}

struct find_by_game_id_ctx {
	GD_GameId game_id;
	obs_sceneitem_t *found;
};

static bool find_group_item_by_game_id(obs_scene_t *, obs_sceneitem_t *item, void *vp)
{
	auto *ctx = (find_by_game_id_ctx *)vp;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	if (!is_gd_wasapi_capture(src))
		return true;

	if (ctx->game_id != 0 && source_game_id(src) == ctx->game_id) {
		ctx->found = item;
		return false;
	}

	return true;
}

typedef struct reconcile_desired_t {
	bool desired[GD_MAX_GAMES];
	DWORD desired_pid[GD_MAX_GAMES];
	char desired_name[GD_MAX_GAMES][256];
	GD_GameId desired_id[GD_MAX_GAMES];
	char desired_cap[GD_MAX_GAMES][GD_MAX_PATH];
	int desired_count;
} reconcile_desired_t;

typedef struct {
	obs_source_t *list[128];
	int count;
} gd_pending_rm_t;

typedef bool (*gd_capture_visit_fn)(obs_source_t *src, void *userdata);

typedef struct {
	GD_GameId filter_id;
	gd_capture_visit_fn visitor;
	void *userdata;
} gd_visit_ctx;

static bool visit_gd_capture_filter_cb(void *vp, obs_source_t *src)
{
	auto *ctx = (gd_visit_ctx *)vp;
	if (!is_gd_wasapi_capture(src))
		return true;

	GD_GameId gid = source_game_id(src);
	if (gid == 0)
		return true;
	if (ctx->filter_id != 0 && gid != ctx->filter_id)
		return true;

	return ctx->visitor(src, ctx->userdata);
}

static void visit_gd_captures(GD_GameId filter_id, gd_capture_visit_fn visitor, void *userdata)
{
	gd_visit_ctx ctx = {filter_id, visitor, userdata};
	obs_enum_sources(visit_gd_capture_filter_cb, &ctx);
}

static void pending_rm_add(gd_pending_rm_t *pending, obs_source_t *src)
{
	if (!pending)
		return;
	if (pending->count >= (int)(sizeof(pending->list) / sizeof(pending->list[0]))) {
		const char *name = obs_source_get_name(src);
		blog(LOG_ERROR, "[obs-game-detector] pending_rm full: dropping '%s'", name ? name : "?");
		return;
	}
	obs_source_get_ref(src);
	pending->list[pending->count++] = src;
}

static void pending_rm_flush(gd_pending_rm_t *pending)
{
	if (!pending)
		return;

	s_gd_removing_captures = true;
	for (int i = 0; i < pending->count; i++)
		obs_mut_remove_capture(pending->list[i]);
	pending->count = 0;
	s_gd_removing_captures = false;
}

typedef struct {
	gd_pending_rm_t *pending;
} rm_queue_ctx;

static bool queue_capture_rm_visit(obs_source_t *src, void *vp)
{
	auto *ctx = (rm_queue_ctx *)vp;
	const char *name = obs_source_get_name(src);
	blog(LOG_INFO, "[obs-game-detector] queue capture '%s' for removal", name ? name : "?");
	pending_rm_add(ctx->pending, src);
	return true;
}

static void queue_gd_captures_for_removal(GD_GameId filter_id, gd_capture_visit_fn should_queue, void *userdata)
{
	gd_pending_rm_t pending = {};
	rm_queue_ctx qctx = {&pending};

	if (!should_queue) {
		visit_gd_captures(filter_id, queue_capture_rm_visit, &qctx);
	} else {
		struct selective_ctx {
			gd_capture_visit_fn should_queue;
			void *userdata;
			gd_pending_rm_t *pending;
		} sctx = {should_queue, userdata, &pending};

		auto selective_visit = +[](obs_source_t *src, void *vp) -> bool {
			auto *sc = (selective_ctx *)vp;
			if (!sc->should_queue(src, sc->userdata))
				return true;
			const char *name = obs_source_get_name(src);
			blog(LOG_INFO, "[obs-game-detector] queue capture '%s' for removal", name ? name : "?");
			pending_rm_add(sc->pending, src);
			return true;
		};

		visit_gd_captures(filter_id, selective_visit, &sctx);
	}

	pending_rm_flush(&pending);
}

static bool find_first_visit(obs_source_t *src, void *vp)
{
	auto **found = (obs_source_t **)vp;
	obs_source_get_ref(src);
	*found = src;
	return false;
}

static obs_source_t *find_capture_by_game_id(GD_GameId id)
{
	obs_source_t *found = NULL;
	visit_gd_captures(id, find_first_visit, &found);
	return found;
}

static void dedupe_captures_for_game_id(GD_GameId id, obs_source_t *keep)
{
	if (!id || !keep)
		return;

	gd_pending_rm_t pending = {};
	struct dedupe_rm_ctx {
		obs_source_t *keep;
		gd_pending_rm_t *pending;
	} dctx = {keep, &pending};

	auto visit = +[](obs_source_t *src, void *vp) -> bool {
		auto *dc = (dedupe_rm_ctx *)vp;
		if (src == dc->keep)
			return true;
		const char *dup_name = obs_source_get_name(src);
		blog(LOG_INFO, "[obs-game-detector] queue duplicate capture '%s' for removal",
		     dup_name ? dup_name : "?");
		pending_rm_add(dc->pending, src);
		return true;
	};

	visit_gd_captures(id, visit, &dctx);
	pending_rm_flush(&pending);
}

static bool reconcile_queue_visit(obs_source_t *src, void *vp)
{
	auto *desired = (reconcile_desired_t *)vp;
	GD_GameId gid = source_game_id(src);
	for (int i = 0; i < desired->desired_count; i++) {
		if (desired->desired_id[i] == gid)
			return false;
	}
	const char *stale_name = obs_source_get_name(src);
	blog(LOG_INFO, "[obs-game-detector] boot reconcile: queue stale '%s'", stale_name ? stale_name : "?");
	return true;
}

static void remove_all_captures_for_id(GD_GameId id)
{
	queue_gd_captures_for_removal(id, NULL, NULL);
}

static bool apply_tracks_visit(obs_source_t *src, void *vp)
{
	GD_State *state = (GD_State *)vp;
	obs_mut_assert_active();
	uint32_t mixers = gd_config_mixer_mask(&state->config, source_game_id(src));
	obs_source_set_audio_mixers(src, mixers);
	return true;
}

static void apply_audio_tracks(GD_State *state)
{
	if (!state)
		return;
	visit_gd_captures(0, apply_tracks_visit, state);
}

static void apply_wasapi_capture(obs_source_t *source, GD_TrackedGame *g, const GD_ConfigSnap *cfg)
{
	obs_mut_assert_active();
	char cap_title[GD_MAX_PATH];
	gd_strlcpy(cap_title, g->capture_exe, sizeof(cap_title));
	char *ext = strrchr(cap_title, '.');
	if (ext)
		*ext = '\0';
	char window_str[512];
	snprintf(window_str, sizeof(window_str), "%s:.:%s", cap_title, g->capture_exe);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "window", window_str);
	obs_data_set_int(settings, "priority", 2);
	obs_source_update(source, settings);
	obs_data_release(settings);

	tag_source(source, g->id, g->pid);
	obs_source_set_audio_mixers(source, gd_config_mixer_mask(cfg, g->id));
}

static void remove_source_from_group(const char *name)
{
	if (!name || !name[0])
		return;

	obs_source_t *grp_src = get_group_source(false);
	if (!grp_src)
		return;

	obs_scene_t *grp_scene = obs_group_from_source(grp_src);
	if (grp_scene) {
		obs_sceneitem_t *it = find_live_item_in_group(grp_scene, name);
		if (it)
			obs_mut_sceneitem_remove(it);
	}
	obs_source_release(grp_src);
}

static bool capture_in_group(obs_scene_t *grp_scene, GD_GameId id)
{
	if (!grp_scene || id == 0)
		return false;

	find_by_game_id_ctx ctx = {id, NULL};
	obs_scene_enum_items(grp_scene, find_group_item_by_game_id, &ctx);
	return ctx.found != NULL;
}

static obs_source_t *ensure_capture_source(GD_TrackedGame *g, const GD_ConfigSnap *cfg)
{
	obs_mut_assert_active();
	obs_source_t *source = find_capture_by_game_id(g->id);
	if (!source)
		source = obs_get_source_by_name(g->obs_source_name);
	if (!source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "priority", 2);
		source = obs_source_create("wasapi_process_output_capture", g->obs_source_name, settings, NULL);
		obs_data_release(settings);
	}
	if (!source)
		return NULL;

	apply_wasapi_capture(source, g, cfg);
	const char *name = obs_source_get_name(source);
	if (name)
		gd_strlcpy(g->obs_source_name, name, sizeof(g->obs_source_name));
	return source;
}

static bool place_capture_in_group(obs_source_t *source, char scenes[][GD_MAX_SCENE_LEN], int scene_count)
{
	obs_mut_assert_active();

	bool placed = false;
	obs_source_t *grp_src = get_group_source(false);

	for (int i = 0; i < scene_count; i++) {
		obs_source_t *sc_src = obs_get_source_by_name(scenes[i]);
		if (!sc_src)
			continue;
		obs_scene_t *scene = obs_scene_from_source(sc_src);
		if (scene) {
			if (!grp_src)
				grp_src = obs_source_create("group", GD_GROUP_NAME, NULL, NULL);
			if (grp_src && !obs_scene_find_source(scene, GD_GROUP_NAME)) {
				obs_sceneitem_t *grp_item = obs_scene_add(scene, grp_src);
				if (grp_item)
					obs_sceneitem_set_locked(grp_item, true);
			}
		}
		obs_source_release(sc_src);
	}

	if (grp_src) {
		obs_scene_t *grp_scene = obs_group_from_source(grp_src);
		if (grp_scene) {
			obs_sceneitem_t *item = obs_scene_add(grp_scene, source);
			if (item) {
				set_item_color(item, GD_COLOR);
				obs_sceneitem_set_locked(item, true);
				placed = true;
				blog(LOG_INFO, "[obs-game-detector] Added '%s' -> group '%s'",
				     obs_source_get_name(source), GD_GROUP_NAME);
			}
		}
		obs_source_release(grp_src);
	}

	if (placed) {
		obs_source_t *grp_refresh = get_group_source(false);
		if (grp_refresh) {
			obs_scene_t *grp_scene = obs_group_from_source(grp_refresh);
			if (grp_scene) {
				calldata_t cd = {};
				calldata_set_ptr(&cd, "scene", grp_scene);
				signal_handler_signal(obs_source_get_signal_handler(grp_refresh), "reorder", &cd);
				calldata_free(&cd);
			}
			obs_source_release(grp_refresh);
		}
	}

	return placed;
}

static void wire_reinstate(GD_TrackedGame *g, bool placed)
{
	if (!g)
		return;
	reinstate_disconnect(g);
	if (placed) {
		g->state = GD_GAME_CAPTURING;
		reinstate_connect(g);
	}
}

static void place_audio(GD_State *state, GD_TrackedGame *g, bool notify)
{
	if (!state || !g || !g->obs_source_name[0])
		return;

	const GD_ConfigSnap *cfg = &state->config;

	if (!gd_config_is_enabled(cfg, g->id)) {
		blog(LOG_INFO, "[obs-game-detector] '%s' is disabled: skipping add", g->obs_source_name);
		return;
	}

	obs_source_t *source = ensure_capture_source(g, cfg);
	if (!source) {
		blog(LOG_WARNING, "[obs-game-detector] Failed to create source '%s'", g->obs_source_name);
		return;
	}

	obs_source_t *grp_src = get_group_source(true);
	bool already_in_group = false;
	if (grp_src) {
		obs_scene_t *grp_scene = obs_group_from_source(grp_src);
		if (grp_scene)
			already_in_group = capture_in_group(grp_scene, g->id);
	}

	if (already_in_group) {
		obs_source_release(grp_src);
		dedupe_captures_for_game_id(g->id, source);
		wire_reinstate(g, true);
		blog(LOG_INFO, "[obs-game-detector] '%s' already in group: skipping", obs_source_get_name(source));
		obs_source_release(source);
		return;
	}

	if (grp_src)
		obs_source_release(grp_src);

	char scenes[GD_MAX_SCENES][GD_MAX_SCENE_LEN];
	int scene_count = 0;
	get_target_scenes(cfg, scenes, &scene_count);

	bool placed = place_capture_in_group(source, scenes, scene_count);
	dedupe_captures_for_game_id(g->id, source);
	wire_reinstate(g, placed);

	if (placed && notify)
		show_obs_notification(g->display_name);
	else if (!placed && scene_count == 0) {
		blog(LOG_WARNING, "[obs-game-detector] Could not place '%s': no target scenes configured",
		     g->obs_source_name);
	}

	obs_source_release(source);
}

static void remove_audio(GD_State *state, GD_TrackedGame *g)
{
	if (!state || !g)
		return;

	reinstate_disconnect(g);

	obs_mut_remove_capture_by_name(g->obs_source_name);
}

static void remove_audio_by_id(GD_State *state, GD_GameId id, DWORD pid, const char *name_fallback)
{
	if (!state)
		return;

	GD_TrackedGame *g = tracker_find(&state->tracker, id, pid, TRACK_BY_BOTH);
	if (g) {
		remove_audio(state, g);
		return;
	}

	const char *name = name_fallback;
	if (!name || !name[0]) {
		const GD_ConfigRecord *r = gd_config_find(&state->config, id);
		if (r)
			name = r->display_name;
	}
	if (!name || !name[0])
		return;

	obs_mut_remove_capture_by_name(name);
}

static void handle_place_audio(GD_State *state, GD_GameId id)
{
	if (!state)
		return;

	GD_TrackedGame *g = tracker_find(&state->tracker, id, 0, TRACK_BY_GAME_ID);
	if (!g || g->state == GD_GAME_STOPPING)
		return;

	place_audio(state, g, false);
}

static void request_place_audio(GD_State *state, GD_GameId id, DWORD pid)
{
	if (!state || state->phase != GD_PHASE_RUNNING)
		return;

	GD_Event evt = {};
	evt.kind = GD_EVT_PLACE_AUDIO;
	evt.game_id = id;
	evt.pid = pid;
	gd_event_post(&evt);
}

static void sync_visit_cb(GD_TrackedGame *game, void *vp)
{
	(void)vp;
	if (game->state != GD_GAME_CAPTURING && game->state != GD_GAME_DETECTED)
		return;
	if (!game->obs_source_name[0])
		return;

	GD_State *state = gd_state();
	if (!state)
		return;

	const GD_ConfigSnap *cfg = &state->config;
	if (!gd_config_is_enabled(cfg, game->id))
		return;

	place_audio(state, game, false);
}

static void sync_group_to_scenes(void)
{
	GD_State *state = obs_state();
	if (!state)
		return;

	const GD_ConfigSnap *cfg = &state->config;

	struct sync_ctx {
		char scenes[GD_MAX_SCENES][GD_MAX_SCENE_LEN];
		int scene_count;
	} ctx = {};
	get_target_scenes(cfg, ctx.scenes, &ctx.scene_count);

	obs_source_t *grp_src = get_group_source(false);
	if (grp_src) {
		obs_source_t *sc_enum[64];
		int sc_count = 0;
		char **all = obs_frontend_get_scene_names();
		if (all) {
			for (int i = 0; all[i] && sc_count < 64; i++) {
				bool wanted = false;
				for (int j = 0; j < ctx.scene_count; j++)
					if (strcmp(all[i], ctx.scenes[j]) == 0) {
						wanted = true;
						break;
					}
				if (!wanted) {
					obs_source_t *sc = obs_get_source_by_name(all[i]);
					if (sc)
						sc_enum[sc_count++] = sc;
				}
			}
			bfree(all);
		}

		for (int i = 0; i < sc_count; i++) {
			obs_scene_t *scene = obs_scene_from_source(sc_enum[i]);
			if (scene) {
				obs_sceneitem_t *it = obs_scene_find_source(scene, GD_GROUP_NAME);
				if (it)
					obs_mut_sceneitem_remove(it);
			}
			obs_source_release(sc_enum[i]);
		}
		obs_source_release(grp_src);
	}

	if (ctx.scene_count == 0)
		return;

	tracker_visit(&state->tracker, sync_visit_cb, &ctx);
}

static void show_obs_notification(const char *game_name)
{
	QString title = "OBS Game Detector";
	QString body = QString("Now capturing audio for: %1").arg(QString::fromUtf8(game_name));

	auto *tray = (QSystemTrayIcon *)obs_frontend_get_system_tray();
	if (tray)
		tray->showMessage(title, body, QSystemTrayIcon::Information, 6000);

	auto *mw = (QMainWindow *)obs_frontend_get_main_window();
	if (!mw)
		return;
	QString msg = QString("Game Detector: now capturing audio for \"%1\"").arg(QString::fromUtf8(game_name));
	mw->statusBar()->showMessage(msg, 6000);
}

static void obs_teardown(GD_State *state)
{
	if (!state)
		return;

	for (int i = 0; i < state->tracker.count; i++)
		reinstate_disconnect(&state->tracker.games[i]);

	state->obs_tick_scheduled = false;
	state->schedule_processor = NULL;
	state->post_event = NULL;

	blog(LOG_INFO, "[obs-game-detector] OBS hooks released");
}

static void collect_desired_cb(GD_TrackedGame *game, void *vp)
{
	auto *d = (reconcile_desired_t *)vp;
	GD_State *state = gd_state();
	if (!state)
		return;
	const GD_ConfigSnap *cfg = &state->config;
	if (!gd_config_is_enabled(cfg, game->id))
		return;
	if (d->desired_count >= GD_MAX_GAMES)
		return;

	int i = d->desired_count++;
	d->desired[i] = true;
	d->desired_pid[i] = game->pid;
	d->desired_id[i] = game->id;
	gd_strlcpy(d->desired_name[i], game->obs_source_name, 256);
	gd_strlcpy(d->desired_cap[i], game->capture_exe, GD_MAX_PATH);
}

static void boot_reconcile(GD_State *state)
{
	if (!state)
		return;

	reconcile_desired_t desired = {};
	tracker_visit(&state->tracker, collect_desired_cb, &desired);
	queue_gd_captures_for_removal(0, reconcile_queue_visit, &desired);
}

static void place_all_tracked(GD_State *state)
{
	if (!state)
		return;
	for (int i = 0; i < state->tracker.count; i++)
		place_audio(state, &state->tracker.games[i], false);
}

static void boot_reconcile_task(void *unused)
{
	(void)unused;
	GD_State *state = gd_state();
	if (!state || state->phase != GD_PHASE_RUNNING)
		return;

	obs_mut_begin(state, OBS_MUT_RUNNING);
	place_all_tracked(state);
	boot_reconcile(state);
	obs_mut_end();
}

static void schedule_boot_reconcile(void)
{
	QCoreApplication *app = QCoreApplication::instance();
	if (app) {
		QTimer::singleShot(0, app, []() { boot_reconcile_task(NULL); });
		return;
	}
	obs_queue_task(OBS_TASK_UI, boot_reconcile_task, NULL, false);
}

typedef struct {
	GD_State *state;
	GD_GameId id;
} gd_by_id_ctx;

static void request_remove_cb(GD_TrackedGame *g, void *vp);
static void request_add_cb(GD_TrackedGame *g, void *vp);

static void handle_remove_by_id(GD_State *state, GD_GameId id)
{
	gd_by_id_ctx ctx = {state, id};
	tracker_foreach_by_id(&state->tracker, id, request_remove_cb, &ctx);
	remove_all_captures_for_id(id);
}

static void handle_add_by_id(GD_State *state, GD_GameId id)
{
	gd_by_id_ctx ctx = {state, id};
	tracker_foreach_by_id(&state->tracker, id, request_add_cb, &ctx);
}

static void request_remove_cb(GD_TrackedGame *g, void *vp)
{
	auto *ctx = (gd_by_id_ctx *)vp;
	remove_audio(ctx->state, g);
}

static void request_add_cb(GD_TrackedGame *g, void *vp)
{
	auto *ctx = (gd_by_id_ctx *)vp;
	place_audio(ctx->state, g, false);
}

// exported api

#ifdef __cplusplus
extern "C" {
#endif

GD_State *gd_state(void)
{
	return &g_state;
}

void gd_event_post(const GD_Event *evt)
{
	if (!evt)
		return;

	GD_State *state = gd_state();
	if (!state || state->phase == GD_PHASE_IDLE || state->phase == GD_PHASE_STOPPING)
		return;

	const bool defer_schedule = state->phase == GD_PHASE_BOOT_RECONCILE || state->obs_processor_depth > 0;

	GD_EventRing *ring = &state->events;

	pthread_mutex_lock(&ring->mutex);
	int next = (ring->tail + 1) % GD_EVENT_QUEUE_CAP;
	if (next == ring->head) {
		pthread_mutex_unlock(&ring->mutex);
		blog(LOG_ERROR, "[obs-game-detector] event queue full: event %d", (int)evt->kind);
		return;
	}
	ring->queue[ring->tail] = *evt;
	ring->tail = next;
	pthread_mutex_unlock(&ring->mutex);

	if (!defer_schedule && state->schedule_processor)
		state->schedule_processor();
}

void gd_start(void)
{
	GD_State *state = gd_state();
	if (state->phase != GD_PHASE_IDLE)
		return;

	memset(state, 0, sizeof(*state));
	pthread_mutex_init(&state->events.mutex, NULL);

	gd_skip_rules_load();

	char index_cache[1024];
	if (gd_install_index_cache_path(index_cache, sizeof(index_cache))) {
		gd_install_index_load_or_build(&state->index, index_cache);
		state->index_built_ms = gd_install_index_normalize_built_ms(state->index.built_ms);
	} else {
		gd_install_index_build(&state->index);
		state->index_built_ms = state->index.built_ms;
	}

	gd_config_load(state);
	gd_lookup_build(&state->lookup, &state->index, &state->config);

	tracker_init(&state->tracker);
	state->obs_tick_scheduled = false;
	state->obs_processor_depth = 0;
	state->schedule_processor = schedule_processor;
	state->post_event = gd_event_post;

	state->phase = GD_PHASE_BOOT_RECONCILE;
	gd_watch_snapshot(&state->lookup);
	while (event_pending())
		processor_tick(NULL);
	state->phase = GD_PHASE_RUNNING;

	gd_watch_start();
	schedule_boot_reconcile();

	blog(LOG_INFO, "[obs-game-detector] detector started");
}

void gd_session_end(void)
{
	GD_State *state = gd_state();
	if (state->phase == GD_PHASE_IDLE)
		return;

	state->phase = GD_PHASE_STOPPING;
	state->obs_tick_scheduled = false;
	state->schedule_processor = NULL;
	state->post_event = NULL;

	gd_watch_stop();
	obs_teardown(state);

	gd_install_index_worker_join();
	event_reset(state);

	pthread_mutex_destroy(&state->events.mutex);
	memset(state, 0, sizeof(*state));

	blog(LOG_INFO, "[obs-game-detector] detector stopped");
}

void gd_teardown(void)
{
	gd_session_end();
}

void gd_stop(void)
{
	gd_session_end();
}

static void request_event(GD_EventKind kind, GD_GameId game_id)
{
	GD_Event evt = {};
	evt.kind = kind;
	evt.game_id = game_id;
	gd_event_post(&evt);
}

void gd_request_remove_source_by_id(GD_GameId id)
{
	request_event(GD_EVT_REMOVE_BY_ID, id);
}

void gd_request_add_source_if_running_by_id(GD_GameId id)
{
	request_event(GD_EVT_ADD_BY_ID, id);
}

void gd_request_index_rebuild(void)
{
	request_event(GD_EVT_REBUILD_INDEX, 0);
}

void gd_request_apply_audio_tracks(void)
{
	request_event(GD_EVT_APPLY_TRACKS, 0);
}

void gd_request_sync_scenes(void)
{
	request_event(GD_EVT_SYNC_SCENES, 0);
}

#ifdef __cplusplus
}
#endif
