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
		obs_queue_task(OBS_TASK_UI, gd_start_task, NULL, false);
	} else if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		obs_log(LOG_INFO, "[obs-game-detector] OBS shutting down, stopping detector");
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
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_frontend_add_tools_menu_item("Game Detector Settings", open_dialog_cb, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	gd_stop();
	obs_log(LOG_INFO, "[obs-game-detector] module unload");
}