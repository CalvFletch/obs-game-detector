/*
 * gd-config.cpp — persistent config for obs-game-detector
 *
 * Stored at %APPDATA%\obs-studio\plugin_config\obs-game-detector\config.json
 *
 * Thread-safe: detector thread reads, Qt main thread writes via dialog.
 */

#include "gd-config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winreg.h>
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <string>
#include <vector>
#include <cctype>
#include <ctime>
#include <cstring>
#include <cstdio>

static pthread_mutex_t           s_cfg_mutex;
static std::vector<GDGameRecord> s_games;
static std::vector<std::string>  s_dirs;

/* ── helpers ──────────────────────────────────────────────────────── */
static std::string today_str()
{
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char buf[16];
	strftime(buf, sizeof(buf), "%Y-%m-%d", t);
	return buf;
}

/* ── C++ API (dialog) ─────────────────────────────────────────────── */
std::vector<GDGameRecord> gd_config_get_games()
{
	pthread_mutex_lock(&s_cfg_mutex);
	auto c = s_games;
	pthread_mutex_unlock(&s_cfg_mutex);
	return c;
}

void gd_config_set_games(const std::vector<GDGameRecord> &g)
{
	pthread_mutex_lock(&s_cfg_mutex);
	s_games = g;
	pthread_mutex_unlock(&s_cfg_mutex);
	gd_config_save();
}

std::vector<std::string> gd_config_get_dirs()
{
	pthread_mutex_lock(&s_cfg_mutex);
	auto c = s_dirs;
	pthread_mutex_unlock(&s_cfg_mutex);
	return c;
}

void gd_config_set_dirs(const std::vector<std::string> &d)
{
	pthread_mutex_lock(&s_cfg_mutex);
	s_dirs = d;
	pthread_mutex_unlock(&s_cfg_mutex);
	gd_config_save();
}

/* ── C API ────────────────────────────────────────────────────────── */
void gd_config_load()
{
	pthread_mutex_init(&s_cfg_mutex, NULL);

	char *path = obs_module_config_path("config.json");
	if (!path) return;

	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	if (!data) return;

	obs_data_array_t *arr = obs_data_get_array(data, "games");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *nm = obs_data_get_string(item, "name");
			if (nm && *nm) {
				obs_data_set_default_bool(item, "enabled", true);
				GDGameRecord r;
				r.name      = nm;
				r.last_seen = obs_data_get_string(item, "last_seen");
				r.enabled   = obs_data_get_bool(item, "enabled");
				s_games.push_back(r);
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	/* Try "dirs" first; fall back to old "custom_dirs" key for migration */
	obs_data_array_t *darr = obs_data_get_array(data, "dirs");
	if (!darr) darr = obs_data_get_array(data, "custom_dirs");
	if (darr) {
		size_t n = obs_data_array_count(darr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(darr, i);
			const char *v = obs_data_get_string(item, "value");
			if (v && *v) s_dirs.emplace_back(v);
			obs_data_release(item);
		}
		obs_data_array_release(darr);
	} else {
		/* First run — seed with resolved system paths */
		s_dirs = gd_config_resolve_default_dirs();
	}

	obs_data_release(data);

	/* Merge any newly-resolved paths not already in the list.
	 * This ensures new launchers (e.g. Ubisoft) are picked up
	 * even when upgrading from an older saved config. */
	{
		auto resolved = gd_config_resolve_default_dirs();
		bool changed  = false;
		for (auto &r : resolved) {
			bool found = false;
			for (auto &d : s_dirs)
				if (_stricmp(d.c_str(), r.c_str()) == 0) { found = true; break; }
			if (!found) { s_dirs.push_back(r); changed = true; }
		}
		if (changed) gd_config_save();
	}
}

void gd_config_save()
{
	char *path = obs_module_config_path("config.json");
	if (!path) return;

	/* Ensure the plugin_config directory exists */
	char dir[1024];
	strncpy(dir, path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	char *sep = strrchr(dir, '/');
	if (!sep) sep = strrchr(dir, '\\');
	if (sep) { *sep = '\0'; os_mkdirs(dir); }

	/* Snapshot under lock */
	pthread_mutex_lock(&s_cfg_mutex);
	auto games_snap = s_games;
	auto dirs_snap  = s_dirs;
	pthread_mutex_unlock(&s_cfg_mutex);

	obs_data_t *root = obs_data_create();

	obs_data_array_t *arr = obs_data_array_create();
	for (auto &r : games_snap) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name",      r.name.c_str());
		obs_data_set_string(item, "last_seen", r.last_seen.c_str());
		obs_data_set_bool  (item, "enabled",   r.enabled);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "games", arr);
	obs_data_array_release(arr);

	obs_data_array_t *darr = obs_data_array_create();
	for (auto &d : dirs_snap) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", d.c_str());
		obs_data_array_push_back(darr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "dirs", darr);
	obs_data_array_release(darr);

	obs_data_save_json_safe(root, path, ".tmp", ".bak");
	obs_data_release(root);
	bfree(path);
}

void gd_config_unload()
{
	gd_config_save();
	s_games.clear();
	s_dirs.clear();
	pthread_mutex_destroy(&s_cfg_mutex);
}

void gd_config_record_game(const char *display_name)
{
	if (!display_name || !*display_name) return;
	std::string today = today_str();

	pthread_mutex_lock(&s_cfg_mutex);
	for (auto &r : s_games) {
		if (r.name == display_name) {
			r.last_seen = today;
			pthread_mutex_unlock(&s_cfg_mutex);
			gd_config_save();
			return;
		}
	}
	GDGameRecord r;
	r.name      = display_name;
	r.last_seen = today;
	r.enabled   = true;
	s_games.push_back(r);
	pthread_mutex_unlock(&s_cfg_mutex);
	gd_config_save();
}

bool gd_config_is_disabled(const char *display_name)
{
	if (!display_name || !*display_name) return false;
	pthread_mutex_lock(&s_cfg_mutex);
	for (auto &r : s_games) {
		if (r.name == display_name) {
			bool dis = !r.enabled;
			pthread_mutex_unlock(&s_cfg_mutex);
			return dis;
		}
	}
	pthread_mutex_unlock(&s_cfg_mutex);
	return false; /* not in list → enabled by default */
}

/* Substrings that identify a path as a game install directory.
 * Checked by the detector (is_game_path) and displayed read-only in the dialog. */
static const char *const DEFAULT_GAME_DIRS[] = {
	"steamapps\\common",
	"steamlibrary",
	"epic games",
	"gog games",
	"origin games",
	"ea games",
	"ubisoft game launcher\\games",
	NULL,
};

std::vector<std::string> gd_config_get_default_dirs()
{
	std::vector<std::string> v;
	for (int i = 0; DEFAULT_GAME_DIRS[i]; i++)
		v.emplace_back(DEFAULT_GAME_DIRS[i]);
	return v;
}

/* Resolves actual full game-install directories from the current system
 * (Steam library folders, Epic, GOG, EA, Origin, Ubisoft).
 * Falls back to the substring-pattern defaults if nothing is detected. */
static bool _dir_exists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void _add_unique(std::vector<std::string> &v, const char *path)
{
	if (!path || !*path) return;
	for (auto &s : v)
		if (_stricmp(s.c_str(), path) == 0) return;
	v.emplace_back(path);
}

std::vector<std::string> gd_config_resolve_default_dirs()
{
	std::vector<std::string> v;

	/* ── Steam ─────────────────────────────────────────────────── */
	HKEY hk;
	char steam[MAX_PATH] = {0};
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam",
	                  0, KEY_READ, &hk) == ERROR_SUCCESS) {
		DWORD sz = sizeof(steam);
		RegQueryValueExA(hk, "SteamPath", NULL, NULL, (LPBYTE)steam, &sz);
		RegCloseKey(hk);
		for (char *p = steam; *p; p++) if (*p == '/') *p = '\\';
	}
	if (steam[0]) {
		/* Primary library */
		char common[MAX_PATH];
		snprintf(common, sizeof(common), "%s\\steamapps\\common", steam);
		_add_unique(v, common);

		/* Additional libraries from libraryfolders.vdf */
		char vdf_path[MAX_PATH];
		snprintf(vdf_path, sizeof(vdf_path),
		         "%s\\steamapps\\libraryfolders.vdf", steam);
		FILE *vf = fopen(vdf_path, "rb");
		if (vf) {
			fseek(vf, 0, SEEK_END);
			long fsz = ftell(vf);
			rewind(vf);
			if (fsz > 0 && fsz < 2 * 1024 * 1024) {
				char *vbuf = (char *)bmalloc((size_t)fsz + 1);
				fread(vbuf, 1, (size_t)fsz, vf);
				vbuf[fsz] = '\0';
				const char *p = vbuf;
				while ((p = strstr(p, "\"path\"")) != NULL) {
					p += 6;
					while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
					if (*p == '"') {
						p++;
						char lib[MAX_PATH] = {0};
						int ci = 0;
						while (*p && *p != '"' && ci < MAX_PATH - 2) {
							if (*p == '\\' && *(p + 1) == '\\') { lib[ci++] = '\\'; p += 2; }
							else { lib[ci++] = *p++; }
						}
						for (char *q = lib; *q; q++) if (*q == '/') *q = '\\';
						if (lib[0]) {
							char lc[MAX_PATH];
							snprintf(lc, sizeof(lc), "%s\\steamapps\\common", lib);
							_add_unique(v, lc);
						}
					}
				}
				bfree(vbuf);
			}
			fclose(vf);
		}
	}

	/* ── GOG ───────────────────────────────────────────────────── */
	HKEY hgog;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	                  "SOFTWARE\\WOW6432Node\\GOG.com\\Games",
	                  0, KEY_READ, &hgog) == ERROR_SUCCESS) {
		char sub[256]; DWORD idx = 0, sublen = sizeof(sub);
		while (RegEnumKeyExA(hgog, idx++, sub, &sublen, NULL,
		                     NULL, NULL, NULL) == ERROR_SUCCESS) {
			sublen = sizeof(sub);
			HKEY hg;
			if (RegOpenKeyExA(hgog, sub, 0, KEY_READ, &hg) == ERROR_SUCCESS) {
				char path[MAX_PATH] = {0};
				DWORD psz = sizeof(path);
				RegQueryValueExA(hg, "path", NULL, NULL, (LPBYTE)path, &psz);
				RegCloseKey(hg);
				/* Add the game's parent directory */
				if (path[0]) {
					char parent[MAX_PATH];
					strncpy(parent, path, MAX_PATH - 1);
					char *sep = (char *)strrchr((const char *)parent, '\\');
					if (sep) { *sep = '\0'; _add_unique(v, parent); }
				}
			}
		}
		RegCloseKey(hgog);
	}

	/* ── Epic Games ────────────────────────────────────────────── */
	{
		const char *pf  = getenv("ProgramFiles");
		const char *pf86 = getenv("ProgramFiles(x86)");
		char epic[MAX_PATH];
		if (pf) {
			snprintf(epic, sizeof(epic), "%s\\Epic Games", pf);
			if (_dir_exists(epic)) _add_unique(v, epic);
		}
		if (pf86) {
			snprintf(epic, sizeof(epic), "%s\\Epic Games", pf86);
			if (_dir_exists(epic)) _add_unique(v, epic);
		}
	}

	/* ── EA / Origin ───────────────────────────────────────────── */
	{
		const char *pf   = getenv("ProgramFiles");
		const char *pf86 = getenv("ProgramFiles(x86)");
		char buf[MAX_PATH];
		const char *ea_names[] = { "EA Games", "Origin Games", NULL };
		for (int i = 0; ea_names[i]; i++) {
			if (pf)   { snprintf(buf, sizeof(buf), "%s\\%s", pf,   ea_names[i]); if (_dir_exists(buf)) _add_unique(v, buf); }
			if (pf86) { snprintf(buf, sizeof(buf), "%s\\%s", pf86, ea_names[i]); if (_dir_exists(buf)) _add_unique(v, buf); }
		}
	}

	/* ── Ubisoft Connect ──────────────────────────────────────── */
	{
		/* Default install folder (if it exists) */
		const char *pf86 = getenv("ProgramFiles(x86)");
		const char *pf   = getenv("ProgramFiles");
		char buf[MAX_PATH];
		if (pf86) { snprintf(buf, sizeof(buf), "%s\\Ubisoft\\Ubisoft Game Launcher\\games", pf86); if (_dir_exists(buf)) _add_unique(v, buf); }
		if (pf)   { snprintf(buf, sizeof(buf), "%s\\Ubisoft\\Ubisoft Game Launcher\\games", pf);   if (_dir_exists(buf)) _add_unique(v, buf); }

		/* Registry scan for all installed games (covers custom install paths) */
		HKEY hub;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
		                  "SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs",
		                  0, KEY_READ, &hub) == ERROR_SUCCESS) {
			char sub[256]; DWORD uidx = 0, sublen = sizeof(sub);
			while (RegEnumKeyExA(hub, uidx++, sub, &sublen, NULL,
			                     NULL, NULL, NULL) == ERROR_SUCCESS) {
				sublen = sizeof(sub);
				HKEY hg;
				if (RegOpenKeyExA(hub, sub, 0, KEY_READ, &hg) == ERROR_SUCCESS) {
					char idir[MAX_PATH] = {0};
					DWORD dsz = sizeof(idir);
					RegQueryValueExA(hg, "InstallDir", NULL, NULL, (LPBYTE)idir, &dsz);
					RegCloseKey(hg);
					if (idir[0]) {
						/* Trim trailing slash */
						size_t slen = strlen(idir);
						while (slen > 0 && (idir[slen-1] == '\\'  || idir[slen-1] == '/'))
							idir[--slen] = '\0';
						_add_unique(v, idir); /* individual install dir */
						/* Also add parent folder so sibling games are covered */
						char par[MAX_PATH];
						strncpy(par, idir, MAX_PATH - 1);
						char *sep = (char *)strrchr((const char *)par, '\\');
						if (sep) { *sep = '\0'; _add_unique(v, par); }
					}
				}
			}
			RegCloseKey(hub);
		}
	}
	/* Fallback if nothing resolved */
	if (v.empty())
		v = gd_config_get_default_dirs();
	return v;
}

bool gd_config_is_game_path(const char *path_lower)
{
	if (!path_lower || !*path_lower) return false;
	pthread_mutex_lock(&s_cfg_mutex);
	for (auto &d : s_dirs) {
		std::string dl = d;
		for (auto &c : dl) c = (char)tolower((unsigned char)c);
		for (auto &c : dl) if (c == '/') c = '\\';
		if (strstr(path_lower, dl.c_str())) {
			pthread_mutex_unlock(&s_cfg_mutex);
			return true;
		}
	}
	pthread_mutex_unlock(&s_cfg_mutex);
	return false;
}
