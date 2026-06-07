#pragma once

#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scene / group the plugin targets (user-configurable via Tools → obs-game-detector) */
#define GD_DEFAULT_SCENE  "Gaming"
#define GD_DEFAULT_GROUP  "Gaming Audio"
#define GD_DEFAULT_POLL   5000  /* ms */
/* OBS colour-label index: 0=none 1=cyan 2=magenta 3=yellow 4=red 5=green 6=blue 7=orange */
#define GD_DEFAULT_COLOR  7

/* Start / stop the background poll timer.
   call gd_start() from obs_module_load, gd_stop() from obs_module_unload. */
void gd_start(void);
void gd_stop(void);

/* Opens the Tools menu settings dialog (Qt main thread) */
void gd_open_dialog(void);

/* Remove the OBS audio source for a game (e.g. when user disables it in settings) */
void gd_remove_source(const char *game_name);

/* If game_name is currently running, add/restore its OBS audio source */
void gd_add_source_if_running(const char *game_name);

#ifdef __cplusplus
}
#endif
