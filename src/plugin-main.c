/*
 * obs-game-detector
 * Copyright (C) 2026 CIsaa
 *
 * Auto-detects running Steam / Epic / GOG games and creates
 * wasapi_process_output_capture audio sources inside a configured
 * OBS group, with a colour label.
 *
 * GPL-2.0 - see LICENSE
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "gd_api.h"

/* --------------------------------------------------------------------------
 * Auto Game Audio marker source
 *
 * A zero-output INPUT source. Adding it to a scene is equivalent to
 * checking that scene in the Target Scenes tab — the engine scans for
 * it in get_target_scenes() every processing tick.
 * -------------------------------------------------------------------------- */
#define GD_MARKER_SOURCE_ID "gd_auto_game_audio"

static const char *gd_marker_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("GD.AutoGameAudio.Name");
}

static void *gd_marker_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	(void)source;
	return (void *)(uintptr_t)1;
}

static void gd_marker_destroy(void *data)
{
	(void)data;
}

static const struct obs_source_info gd_marker_source = {
	.id = GD_MARKER_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = 0,
	.get_name = gd_marker_get_name,
	.create = gd_marker_create,
	.destroy = gd_marker_destroy,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void gd_start_task(void *unused)
{
	(void)unused;
	gd_start();
}

static void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	(void)unused;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		obs_log(LOG_INFO, "[obs-game-detector] OBS finished loading, starting detector");
		// dont mutate the scene graph in this callback
		// obs may still be wiring auth ui and holding source refs
		obs_queue_task(OBS_TASK_UI, gd_start_task, NULL, false);
	} else if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		obs_log(LOG_INFO, "[obs-game-detector] OBS shutting down, stopping detector");
		// synchronous so we set STOPPING and disconnect hooks before
		// closeEvent -> ClearSceneData -> ClearVolumeControls
		gd_teardown();
	}
}

static void open_dialog_cb(void *unused)
{
	(void)unused;
	gd_open_dialog();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[obs-game-detector] module load");
	obs_register_source(&gd_marker_source);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_frontend_add_tools_menu_item("Game Detector Settings", open_dialog_cb, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	// SCRIPTING_SHUTDOWN already tore down in on_frontend_event; gd_stop is a
	// no-op if we never started
	gd_stop();
	obs_log(LOG_INFO, "[obs-game-detector] module unload");
}
