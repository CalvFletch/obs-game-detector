#pragma once
#include <stdbool.h>

#ifdef __cplusplus
#include <string>
#include <vector>

struct GDGameRecord {
	std::string name;
	std::string last_seen;
	bool        enabled;
};

/* C++ API — dialog only */
std::vector<GDGameRecord>  gd_config_get_games(void);
void                       gd_config_set_games(const std::vector<GDGameRecord> &g);
std::vector<std::string>   gd_config_get_dirs(void);
void                       gd_config_set_dirs(const std::vector<std::string> &d);
std::vector<std::string>   gd_config_get_scenes(void);
void                       gd_config_set_scenes(const std::vector<std::string> &s);
std::vector<std::string>   gd_config_get_default_dirs(void);
std::vector<std::string>   gd_config_resolve_default_dirs(void);

extern "C" {
#endif

/* C API */
void gd_config_load(void);
void gd_config_save(void);
void gd_config_unload(void);

/* Called from detector thread when a game is first seen */
void gd_config_record_game(const char *display_name);

/* Returns true when audio source creation should be suppressed */
bool gd_config_is_disabled(const char *display_name);

/* Returns true when path_lower matches any entry in the lookup dirs list */
bool gd_config_is_game_path(const char *path_lower);

#ifdef __cplusplus
}
#endif
