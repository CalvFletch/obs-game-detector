/*
 * obs-game-detector – game-detector.cpp
 *
 * Event-driven game detection via WMI Win32_ProcessStartTrace /
 * Win32_ProcessStopTrace.  Zero polling — callbacks fire the moment
 * a process starts or stops.
 *
 * On OBS "finished loading" a one-time snapshot catches games that
 * were already running before OBS opened.
 *
 * Supports: Steam, Epic Games, GOG (registry / manifest scan).
 * Handles:  BattlEye launchers (_BE.exe → actual game exe).
 */

#define _WIN32_DCOM

#include "game-detector.h"
#include "gd-config.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <callback/calldata.h>

#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <comdef.h>
#include <wbemidl.h>

#include <QMainWindow>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QMetaObject>

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* ── tunables ──────────────────────────────────────────────────── */
#define MAX_GAMES    64
#define MAX_PATH_LEN 1024
#define MAP_CAP      512
#define REBUILD_MS   (30ULL * 60 * 1000)

/* ── data structures ────────────────────────────────────────────── */
typedef struct {
	char install_dir[MAX_PATH_LEN];
	char display_name[256];
} game_entry_t;

typedef struct {
	char               exe_lower[MAX_PATH_LEN];
	char               display_name[256];
	char               capture_exe[MAX_PATH_LEN];
	void              *reinstate_cb; /* SourceReinstateCB* — for disconnect */
} tracked_t;

typedef struct {
	char  exe[MAX_PATH_LEN];
	char  path[MAX_PATH_LEN];
	DWORD pid;
} proc_entry_t;

/* ── shared state ───────────────────────────────────────────────── */
static pthread_mutex_t s_mutex;

static char s_scene[256] = GD_DEFAULT_SCENE;
static char s_group[256] = GD_DEFAULT_GROUP;
static int  s_color      = GD_DEFAULT_COLOR;

static game_entry_t s_map[MAP_CAP];
static int          s_map_count    = 0;
static uint64_t     s_last_rebuild = 0;

static tracked_t    s_seen[MAX_GAMES];
static int          s_seen_count = 0;

/* ── WMI globals ────────────────────────────────────────────────── */
static IWbemServices   *s_wmi_svc    = NULL;
static IWbemObjectSink *s_start_sink = NULL;
static IWbemObjectSink *s_stop_sink  = NULL;
static HANDLE           s_wmi_thread = NULL;
static HANDLE           s_wmi_stop   = NULL;

/* ── helpers ────────────────────────────────────────────────────── */
static void str_lower(char *dst, const char *src, size_t cap)
{
	size_t i = 0;
	for (; i < cap - 1 && src[i]; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

static void rstrip_bs(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && s[n - 1] == '\\')
		s[--n] = '\0';
}

/* ── skip lists ─────────────────────────────────────────────────── */
static const char *SKIP_EXE[] = {
	"unitycrashandler64.exe", "unitycrashandler.exe", "crashpad_handler.exe",
	"easyanticheats.exe",    "easyanticheat_launcher.exe",
	"beservice.exe",         "bedaisy.exe",
	"steamservice.exe",      "steam.exe",      "steamwebhelper.exe",
	"gameoverlayui.exe",     "gameoverlayui64.exe",
	"cefsubprocess.exe",     "unrealcefsubprocess.exe",
	"dotnet.exe",            "node.exe",        "java.exe", "javaw.exe",
	"steamshim.exe",         "battleye.exe",    "battleyelauncher.exe",
	"nprotect.exe",          "gameguard.exe",
	NULL,
};

static const char *SKIP_SUBSTR[] = {
	"crash", "handler", "service", "setup", "install", "uninstall",
	"redist", "helper", "updater", "anticheat", "anti_cheat",
	"overlay", "cefsubprocess", "shim", "monitor", "report", "sender",
	NULL,
};

static bool should_skip_exe(const char *exe_lower)
{
	for (int i = 0; SKIP_EXE[i]; i++)
		if (strcmp(exe_lower, SKIP_EXE[i]) == 0)
			return true;

	char base[MAX_PATH_LEN];
	strncpy(base, exe_lower, sizeof(base) - 1);
	base[sizeof(base) - 1] = '\0';
	char *dot = strrchr(base, '.');
	if (dot) *dot = '\0';

	for (int i = 0; SKIP_SUBSTR[i]; i++)
		if (strstr(base, SKIP_SUBSTR[i]))
			return true;

	size_t blen = strlen(base);
	const char *suf  = "launcher";
	size_t      slen = strlen(suf);
	if (blen >= slen && strcmp(base + blen - slen, suf) == 0)
		return true;

	return false;
}

static bool is_game_path(const char *path_lower)
{
	return gd_config_is_game_path(path_lower);
}

/* ── game map ────────────────────────────────────────────────────── */
static void map_add(const char *dir, const char *name)
{
	if (s_map_count >= MAP_CAP) return;
	char lower[MAX_PATH_LEN];
	str_lower(lower, dir, sizeof(lower));
	for (char *p = lower; *p; p++) if (*p == '/') *p = '\\';
	rstrip_bs(lower);
	for (int i = 0; i < s_map_count; i++)
		if (strcmp(s_map[i].install_dir, lower) == 0) return;
	strncpy(s_map[s_map_count].install_dir,  lower, MAX_PATH_LEN - 1);
	strncpy(s_map[s_map_count].display_name, name,  255);
	s_map_count++;
}

static bool vdf_get(const char *buf, const char *key, char *out, size_t cap)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char *p = strstr(buf, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ' || *p == '\t') p++;
	if (*p != '"') return false;
	p++;
	const char *end = strchr(p, '"');
	if (!end) return false;
	size_t len = (size_t)(end - p);
	if (len >= cap) len = cap - 1;
	strncpy(out, p, len);
	out[len] = '\0';
	return true;
}

static char *read_file_alloc(const char *path)
{
	FILE *f;
	if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
	char *buf = (char *)bmalloc((size_t)sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	return buf;
}

static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '"') return false;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < cap - 1) {
		if (*p == '\\') p++;
		out[i++] = *p++;
	}
	out[i] = '\0';
	return i > 0;
}

static void scan_steam_library(const char *library_path)
{
	char glob_path[MAX_PATH_LEN];
	snprintf(glob_path, sizeof(glob_path), "%s\\appmanifest_*.acf", library_path);

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(glob_path, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		char acf_path[MAX_PATH_LEN];
		snprintf(acf_path, sizeof(acf_path), "%s\\%s", library_path, fd.cFileName);
		char *buf = read_file_alloc(acf_path);
		if (!buf) continue;
		char name[256] = {0}, idir[MAX_PATH_LEN] = {0};
		if (vdf_get(buf, "name", name, sizeof(name)) &&
		    vdf_get(buf, "installdir", idir, sizeof(idir))) {
			char full[MAX_PATH_LEN];
			snprintf(full, sizeof(full), "%s\\common\\%s", library_path, idir);
			map_add(full, name);
		}
		bfree(buf);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static void build_game_map(void)
{
	s_map_count = 0;

	HKEY hk;
	char steam_path[MAX_PATH_LEN] = {0};
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam",
	                  0, KEY_READ, &hk) == ERROR_SUCCESS) {
		DWORD sz = sizeof(steam_path);
		RegQueryValueExA(hk, "SteamPath", NULL, NULL, (LPBYTE)steam_path, &sz);
		RegCloseKey(hk);
		for (char *p = steam_path; *p; p++) if (*p == '/') *p = '\\';
	}

	if (steam_path[0]) {
		char lib0[MAX_PATH_LEN];
		snprintf(lib0, sizeof(lib0), "%s\\steamapps", steam_path);
		scan_steam_library(lib0);

		char vdf[MAX_PATH_LEN];
		snprintf(vdf, sizeof(vdf), "%s\\steamapps\\libraryfolders.vdf", steam_path);
		char *buf = read_file_alloc(vdf);
		if (buf) {
			const char *p = buf;
			while ((p = strstr(p, "\"path\"")) != NULL) {
				char extra[MAX_PATH_LEN];
				if (vdf_get(p, "path", extra, sizeof(extra))) {
					for (char *q = extra; *q; q++) if (*q == '/') *q = '\\';
					char unesc[MAX_PATH_LEN];
					char *d = unesc;
					for (const char *s2 = extra; *s2; s2++, d++) {
						*d = *s2;
						if (*s2 == '\\' && *(s2+1) == '\\') s2++;
					}
					*d = '\0';
					char lib[MAX_PATH_LEN];
					snprintf(lib, sizeof(lib), "%s\\steamapps", unesc);
					scan_steam_library(lib);
				}
				p++;
			}
			bfree(buf);
		}
	}

	const char *epic_dir =
		"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
	WIN32_FIND_DATAA fd2;
	char epic_glob[MAX_PATH_LEN];
	snprintf(epic_glob, sizeof(epic_glob), "%s\\*.item", epic_dir);
	HANDLE h2 = FindFirstFileA(epic_glob, &fd2);
	if (h2 != INVALID_HANDLE_VALUE) {
		do {
			char item[MAX_PATH_LEN];
			snprintf(item, sizeof(item), "%s\\%s", epic_dir, fd2.cFileName);
			char *buf = read_file_alloc(item);
			if (!buf) continue;
			char dn[256] = {0}, loc[MAX_PATH_LEN] = {0};
			if (json_str(buf, "DisplayName",    dn,  sizeof(dn)) &&
			    json_str(buf, "InstallLocation", loc, sizeof(loc)))
				map_add(loc, dn);
			bfree(buf);
		} while (FindNextFileA(h2, &fd2));
		FindClose(h2);
	}

	HKEY hgog;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	                  "SOFTWARE\\WOW6432Node\\GOG.com\\Games",
	                  0, KEY_READ, &hgog) == ERROR_SUCCESS) {
		char sub[256];
		DWORD idx = 0, sublen = sizeof(sub);
		while (RegEnumKeyExA(hgog, idx++, sub, &sublen, NULL,
		                     NULL, NULL, NULL) == ERROR_SUCCESS) {
			sublen = sizeof(sub);
			HKEY hg;
			if (RegOpenKeyExA(hgog, sub, 0, KEY_READ, &hg) == ERROR_SUCCESS) {
				char path[MAX_PATH_LEN] = {0}, gname[256] = {0};
				DWORD psz = sizeof(path), nsz = sizeof(gname);
				RegQueryValueExA(hg, "path",     NULL, NULL, (LPBYTE)path,  &psz);
				RegQueryValueExA(hg, "GAMENAME", NULL, NULL, (LPBYTE)gname, &nsz);
				if (path[0] && gname[0]) map_add(path, gname);
				RegCloseKey(hg);
			}
		}
		RegCloseKey(hgog);
	}

	/* ── Ubisoft Connect ───────────────────────────────── */
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
				char idir[MAX_PATH_LEN] = {0};
				DWORD dsz = sizeof(idir);
				RegQueryValueExA(hg, "InstallDir", NULL, NULL, (LPBYTE)idir, &dsz);
				RegCloseKey(hg);
				if (idir[0]) {
					size_t slen = strlen(idir);
					while (slen > 0 && (idir[slen-1] == '\\'  || idir[slen-1] == '/'))
						idir[--slen] = '\0';
					const char *nm = strrchr(idir, '\\');
					nm = nm ? nm + 1 : idir;
					if (*nm) map_add(idir, nm);
				}
			}
		}
		RegCloseKey(hub);
	}

	s_last_rebuild = (uint64_t)(os_gettime_ns() / 1000000);
	blog(LOG_INFO, "[obs-game-detector] game map built: %d entries", s_map_count);
}

static bool name_from_map(const char *exe_path, char *out_name, size_t cap)
{
	char dir_lower[MAX_PATH_LEN];
	str_lower(dir_lower, exe_path, sizeof(dir_lower));

	char *last_bs = strrchr(dir_lower, '\\');
	if (last_bs) *last_bs = '\0';
	rstrip_bs(dir_lower);

	for (int depth = 0; depth < 6; depth++) {
		for (int i = 0; i < s_map_count; i++) {
			if (strcmp(s_map[i].install_dir, dir_lower) == 0) {
				strncpy(out_name, s_map[i].display_name, cap - 1);
				out_name[cap - 1] = '\0';
				return true;
			}
		}
		char *bs = strrchr(dir_lower, '\\');
		if (!bs) break;
		*bs = '\0';
		rstrip_bs(dir_lower);
	}

	const char *common = strstr(dir_lower, "\\common\\");
	if (common) {
		const char *start = common + 8;
		const char *end   = strchr(start, '\\');
		size_t len = end ? (size_t)(end - start) : strlen(start);
		if (len > 0 && len < cap) {
			strncpy(out_name, start, len);
			out_name[len] = '\0';
			out_name[0] = (char)toupper((unsigned char)out_name[0]);
			return true;
		}
	}

	char base[256];
	str_lower(base, exe_path, sizeof(base));
	char *fn = strrchr(base, '\\');
	fn = fn ? fn + 1 : base;
	char *dot = strrchr(fn, '.');
	if (dot) *dot = '\0';
	fn[0] = (char)toupper((unsigned char)fn[0]);
	strncpy(out_name, fn, cap - 1);
	out_name[cap - 1] = '\0';
	return true;
}

/* ── OBS source helpers ─────────────────────────────────────────── */
static void set_item_color(obs_sceneitem_t *item, int color)
{
	obs_data_t *ps = obs_sceneitem_get_private_settings(item);
	if (ps) {
		obs_data_set_int(ps, "color-preset", color > 0 ? color + 1 : 0);
		obs_data_release(ps);
	}
}

/* Forward declaration */
static void add_game_source(const char *game_name, const char *capture_exe);

/* Callback data for the group scene "item_remove" signal */
struct SourceReinstateCB {
	char game_name[256];
	char capture_exe[MAX_PATH_LEN];
};

/* Fired when ANY item is removed from the group scene.
 * Checks whether it was our tracked source and reinstates if game still runs. */
static void on_group_item_removed(void *data, calldata_t *cd)
{
	SourceReinstateCB *cb = (SourceReinstateCB *)data;

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(cd, "item");
	if (!item) return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src) return;
	const char *name = obs_source_get_name(src);
	if (!name || strcmp(name, cb->game_name) != 0) return;

	bool still_tracked = false;
	pthread_mutex_lock(&s_mutex);
	for (int i = 0; i < s_seen_count; i++) {
		if (strcmp(s_seen[i].display_name, cb->game_name) == 0) {
			still_tracked = true;
			break;
		}
	}
	pthread_mutex_unlock(&s_mutex);

	if (!still_tracked) return;

	blog(LOG_INFO,
	     "[obs-game-detector] Item for '%s' removed from group \u2014 reinstating",
	     cb->game_name);

	std::string gn(cb->game_name);
	std::string ce(cb->capture_exe);
	auto *mw = (QMainWindow *)obs_frontend_get_main_window();
	if (mw) {
		QMetaObject::invokeMethod(mw, [gn, ce]() {
			add_game_source(gn.c_str(), ce.c_str());
		}, Qt::QueuedConnection);
	}
}

static void add_game_source(const char *game_name, const char *capture_exe)
{
	/* If this source is already in the group, nothing to do */
	{
		pthread_mutex_lock(&s_mutex);
		char grp[256];
		strncpy(grp, s_group, 255);
		pthread_mutex_unlock(&s_mutex);
		obs_source_t *grp_src = obs_get_source_by_name(grp);
		if (grp_src) {
			obs_scene_t *grp_scene = obs_group_from_source(grp_src);
			bool already_placed = grp_scene &&
				obs_scene_find_source(grp_scene, game_name) != nullptr;
			obs_source_release(grp_src);
			if (already_placed) {
				blog(LOG_INFO,
				     "[obs-game-detector] '%s' already in group — skipping",
				     game_name);
				return;
			}
		}
	}

	char cap_title[MAX_PATH_LEN];
	strncpy(cap_title, capture_exe, sizeof(cap_title) - 1);
	cap_title[sizeof(cap_title) - 1] = '\0';
	char *ext = strrchr(cap_title, '.');
	if (ext) *ext = '\0';
	char window_str[512];
	snprintf(window_str, sizeof(window_str), "%s::%s", cap_title, capture_exe);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "window",   window_str);
	obs_data_set_int   (settings, "priority", 2);

	obs_source_t *source = obs_get_source_by_name(game_name);
	if (!source) {
		source = obs_source_create("wasapi_process_output_capture",
		                           game_name, settings, NULL);
	} else {
		obs_source_update(source, settings);
	}
	obs_data_release(settings);

	if (!source) {
		blog(LOG_WARNING, "[obs-game-detector] Failed to create source '%s'",
		     game_name);
		return;
	}

	obs_source_set_audio_mixers(source, 0x03);

	pthread_mutex_lock(&s_mutex);
	char group_name[256], scene_name[256];
	strncpy(group_name, s_group, 255);
	strncpy(scene_name, s_scene, 255);
	int color = s_color;
	pthread_mutex_unlock(&s_mutex);

	bool placed = false;

	obs_source_t *grp_src = obs_get_source_by_name(group_name);
	if (!grp_src) {
		obs_source_t *sc_src = obs_get_source_by_name(scene_name);
		if (sc_src) {
			obs_scene_t *scene = obs_scene_from_source(sc_src);
			if (scene) {
				grp_src = obs_source_create("group", group_name, NULL, NULL);
				if (grp_src) {
					obs_scene_add(scene, grp_src);
					blog(LOG_INFO,
					     "[obs-game-detector] Created group '%s' in scene '%s'",
					     group_name, scene_name);
				}
			}
			obs_source_release(sc_src);
		}
	}

	if (grp_src) {
		obs_scene_t *grp_scene = obs_group_from_source(grp_src);
		if (grp_scene) {
			obs_sceneitem_t *item = obs_scene_add(grp_scene, source);
			if (item) {
				set_item_color(item, color);
				placed = true;
				blog(LOG_INFO,
				     "[obs-game-detector] Added '%s' \u2192 group '%s'",
				     game_name, group_name);
			}
		}
		/* Hook item_remove on the group scene so we detect when the user
		 * removes our source from the group and can reinstate it.
		 * Disconnect any old hook first (reinstatement re-enters add_game_source). */
		void *old_cb = NULL;
		pthread_mutex_lock(&s_mutex);
		for (int i = 0; i < s_seen_count; i++) {
			if (strcmp(s_seen[i].display_name, game_name) == 0) {
				old_cb = s_seen[i].reinstate_cb;
				s_seen[i].reinstate_cb = NULL;
				break;
			}
		}
		pthread_mutex_unlock(&s_mutex);
		if (old_cb)
			signal_handler_disconnect(obs_source_get_signal_handler(grp_src),
			                          "item_remove", on_group_item_removed, old_cb);
		delete (SourceReinstateCB *)old_cb;

		auto *cb = new SourceReinstateCB;
		strncpy(cb->game_name,   game_name,   sizeof(cb->game_name) - 1);
		strncpy(cb->capture_exe, capture_exe, sizeof(cb->capture_exe) - 1);
		signal_handler_connect(obs_source_get_signal_handler(grp_src),
		                       "item_remove", on_group_item_removed, cb);
		pthread_mutex_lock(&s_mutex);
		for (int i = 0; i < s_seen_count; i++) {
			if (strcmp(s_seen[i].display_name, game_name) == 0) {
				s_seen[i].reinstate_cb = cb;
				break;
			}
		}
		pthread_mutex_unlock(&s_mutex);
		obs_source_release(grp_src);
	}

	if (placed) {
		/* Emit "reorder" on the outer scene so the OBS scene panel
		 * rebuilds the group's item list without needing a manual
		 * collapse/uncollapse. */
		obs_source_t *sc_ui = obs_get_source_by_name(scene_name);
		if (sc_ui) {
			obs_scene_t *sc = obs_scene_from_source(sc_ui);
			if (sc) {
				calldata_t cd = {};
				calldata_set_ptr(&cd, "scene", sc);
				signal_handler_signal(
					obs_source_get_signal_handler(sc_ui),
					"reorder", &cd);
				calldata_free(&cd);
			}
			obs_source_release(sc_ui);
		}
	}

	if (!placed) {
		obs_source_t *sc_src = obs_get_source_by_name(scene_name);
		if (sc_src) {
			obs_scene_t *scene = obs_scene_from_source(sc_src);
			if (scene) {
				obs_sceneitem_t *item = obs_scene_add(scene, source);
				if (item) set_item_color(item, color);
				blog(LOG_INFO,
				     "[obs-game-detector] Added '%s' \u2192 scene root (group not found)",
				     game_name);
			}
			obs_source_release(sc_src);
		}
	}

	obs_source_release(source);
}

static void remove_game_source(const char *game_name)
{
	obs_source_t *src = obs_get_source_by_name(game_name);
	if (src) {
		/* Disconnect our reinstate hook with the exact cb pointer so the
		 * intentional game-exit removal doesn't trigger reinstatement. */
		void *cb_ptr = NULL;
		pthread_mutex_lock(&s_mutex);
		for (int i = 0; i < s_seen_count; i++) {
			if (strcmp(s_seen[i].display_name, game_name) == 0) {
				cb_ptr = s_seen[i].reinstate_cb;
				s_seen[i].reinstate_cb = NULL;
				break;
			}
		}
		pthread_mutex_unlock(&s_mutex);
		if (cb_ptr) {
			/* Disconnect from group scene, not from the source itself */
			pthread_mutex_lock(&s_mutex);
			char grp[256];
			strncpy(grp, s_group, 255);
			pthread_mutex_unlock(&s_mutex);
			obs_source_t *grp_src = obs_get_source_by_name(grp);
			if (grp_src) {
				signal_handler_disconnect(
					obs_source_get_signal_handler(grp_src),
					"item_remove", on_group_item_removed, cb_ptr);
				obs_source_release(grp_src);
			}
			delete (SourceReinstateCB *)cb_ptr;
		}
		obs_source_remove(src);
		obs_source_release(src);
		blog(LOG_INFO, "[obs-game-detector] Removed '%s'", game_name);
	}
}

/* Public API — called from dialog when user unchecks a game */
extern "C" void gd_remove_source(const char *game_name)
{
	remove_game_source(game_name);
}

/* Public API — called from dialog when user re-checks a game;
 * adds the source immediately if the game is currently running. */
extern "C" void gd_add_source_if_running(const char *game_name)
{
	char gn[256] = {}, ce[MAX_PATH_LEN] = {};
	pthread_mutex_lock(&s_mutex);
	for (int i = 0; i < s_seen_count; i++) {
		if (strcmp(s_seen[i].display_name, game_name) == 0) {
			strncpy(gn, s_seen[i].display_name, sizeof(gn) - 1);
			strncpy(ce, s_seen[i].capture_exe,  sizeof(ce) - 1);
			break;
		}
	}
	pthread_mutex_unlock(&s_mutex);
	if (gn[0])
		add_game_source(gn, ce);
}

static bool remove_nonplayer_source_cb(void *unused, obs_source_t *src)
{
	(void)unused;
	const char *id   = obs_source_get_id(src);
	const char *name = obs_source_get_name(src);
	if (id && strcmp(id, "wasapi_process_output_capture") == 0 &&
	    strcmp(name, "Discord") != 0) {
		obs_source_remove(src);
	}
	return true;
}

static void clear_all_game_sources(void)
{
	obs_enum_sources(remove_nonplayer_source_cb, NULL);
}

/* ── Desktop + OBS status-bar notification ───────────────────── */
static void show_obs_notification(const char *game_name)
{
	QString title = "OBS Game Detector";
	QString body  = QString("Now capturing audio for: %1")
	                .arg(QString::fromUtf8(game_name));

	/* Use the OBS system tray icon for the OS notification */
	auto *tray = (QSystemTrayIcon *)obs_frontend_get_system_tray();
	if (tray) {
		QMetaObject::invokeMethod(tray, [tray, title, body]() {
			tray->showMessage(title, body,
			                  QSystemTrayIcon::Information, 6000);
		}, Qt::QueuedConnection);
	}

	/* Also show in OBS status bar */
	auto *mw = (QMainWindow *)obs_frontend_get_main_window();
	if (!mw) return;
	QString msg = QString("Game Detector: now capturing audio for \"%1\"")
	              .arg(QString::fromUtf8(game_name));
	QMetaObject::invokeMethod(mw, [msg]() {
		auto *w = (QMainWindow *)obs_frontend_get_main_window();
		if (w) w->statusBar()->showMessage(msg, 6000);
	}, Qt::QueuedConnection);
}

/* ── initial boot scan ──────────────────────────────────────────── */
static int scan_processes(proc_entry_t *out, int cap)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;

	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(pe);
	int count = 0;

	if (Process32First(snap, &pe)) {
		do {
			if (count >= cap) break;
			proc_entry_t *e = &out[count];
			e->exe[0]  = '\0';
			e->pid     = pe.th32ProcessID;
			e->path[0] = '\0';

			HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
			                        FALSE, pe.th32ProcessID);
			if (hp) {
				DWORD sz = (DWORD)sizeof(e->path) - 1;
				QueryFullProcessImageNameA(hp, 0, e->path, &sz);
				e->path[sz] = '\0';
				CloseHandle(hp);
			}
			if (e->path[0]) {
				const char *fn = strrchr(e->path, '\\');
				fn = fn ? fn + 1 : e->path;
				str_lower(e->exe, fn, sizeof(e->exe));
				count++;
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return count;
}

/* ── shared detection logic ─────────────────────────────────────── */

/* Called for every process start — from boot scan and WMI callbacks */
static void on_process_start_with_path(const char *exe, const char *full_path)
{
	if (should_skip_exe(exe)) return;
	if (!full_path || !full_path[0]) return;

	char path_lower[MAX_PATH_LEN];
	str_lower(path_lower, full_path, sizeof(path_lower));
	if (!is_game_path(path_lower)) {
		blog(LOG_DEBUG, "[obs-game-detector] SKIP (not game path) %s", exe);
		return;
	}

	/* Rebuild game map if stale (30 min) */
	uint64_t now_ms = (uint64_t)(os_gettime_ns() / 1000000);
	if (now_ms - s_last_rebuild > REBUILD_MS)
		build_game_map();

	pthread_mutex_lock(&s_mutex);
	bool already = false;
	for (int j = 0; j < s_seen_count; j++) {
		if (strcmp(s_seen[j].exe_lower, exe) == 0) { already = true; break; }
	}
	pthread_mutex_unlock(&s_mutex);
	if (already) return;

	/* BattlEye strip: rainbowsix_be.exe → rainbowsix.exe */
	char capture_exe[MAX_PATH_LEN];
	strncpy(capture_exe, exe, sizeof(capture_exe) - 1);
	capture_exe[sizeof(capture_exe) - 1] = '\0';
	size_t elen = strlen(exe);
	if (elen > 7 && strcmp(exe + elen - 7, "_be.exe") == 0) {
		char base[MAX_PATH_LEN];
		strncpy(base, exe, sizeof(base) - 1);
		base[sizeof(base) - 1] = '\0';
		char *be = strstr(base, "_be.exe");
		if (be) {
			memcpy(be, ".exe\0", 5);
			strncpy(capture_exe, base, sizeof(capture_exe) - 1);
		}
	}

	char game_name[256];
	name_from_map(full_path, game_name, sizeof(game_name));

	pthread_mutex_lock(&s_mutex);
	if (s_seen_count < MAX_GAMES) {
		strncpy(s_seen[s_seen_count].exe_lower,    exe,         MAX_PATH_LEN - 1);
		strncpy(s_seen[s_seen_count].display_name, game_name,   255);
		strncpy(s_seen[s_seen_count].capture_exe,  capture_exe, MAX_PATH_LEN - 1);
		s_seen_count++;
	}
	pthread_mutex_unlock(&s_mutex);

	gd_config_record_game(game_name);

	if (gd_config_is_disabled(game_name)) {
		blog(LOG_INFO, "[obs-game-detector] SKIP (disabled) %s \u2192 %s",
		     exe, game_name);
		return;
	}

	blog(LOG_INFO, "[obs-game-detector] START %s \u2192 %s", exe, game_name);
	add_game_source(game_name, capture_exe);
	show_obs_notification(game_name);
}

/* Called for every process stop — from WMI callbacks */
static void on_process_stop(const char *exe_lower)
{
	pthread_mutex_lock(&s_mutex);
	char game_name[256] = {0};
	for (int i = 0; i < s_seen_count; i++) {
		if (strcmp(s_seen[i].exe_lower, exe_lower) == 0) {
			strncpy(game_name, s_seen[i].display_name, 255);
			s_seen[i] = s_seen[--s_seen_count]; /* compact */
			break;
		}
	}
	pthread_mutex_unlock(&s_mutex);

	if (game_name[0]) {
		blog(LOG_INFO, "[obs-game-detector] STOP %s \u2192 %s", exe_lower, game_name);
		remove_game_source(game_name);
	}
}

static void initial_scan(void)
{
	static proc_entry_t procs[512];
	int nprocs = scan_processes(procs, 512);
	for (int i = 0; i < nprocs; i++)
		on_process_start_with_path(procs[i].exe, procs[i].path);
}

/* ── WMI event sink ─────────────────────────────────────────────── */
class ProcessEventSink : public IWbemObjectSink {
	LONG m_ref;
	bool m_is_start;

public:
	ProcessEventSink(bool is_start) : m_ref(1), m_is_start(is_start) {}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return (ULONG)InterlockedIncrement(&m_ref);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return (ULONG)r;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
			*ppv = static_cast<IWbemObjectSink *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE Indicate(LONG count,
	                                   IWbemClassObject **objs) override
	{
		for (LONG i = 0; i < count; i++) {
			VARIANT vName;
			VariantInit(&vName);
			if (FAILED(objs[i]->Get(L"ProcessName", 0, &vName, NULL, NULL))
			    || vName.vt != VT_BSTR) {
				VariantClear(&vName);
				continue;
			}

			char exe_lower[MAX_PATH_LEN];
			WideCharToMultiByte(CP_ACP, 0, vName.bstrVal, -1,
			                    exe_lower, MAX_PATH_LEN, NULL, NULL);
			VariantClear(&vName);
			for (char *p = exe_lower; *p; p++)
				*p = (char)tolower((unsigned char)*p);

			if (m_is_start) {
				VARIANT vPid;
				VariantInit(&vPid);
				DWORD pid = 0;
				if (SUCCEEDED(objs[i]->Get(L"ProcessID", 0,
				                           &vPid, NULL, NULL))) {
					if      (vPid.vt == VT_UI4) pid = vPid.uintVal;
					else if (vPid.vt == VT_I4)  pid = (DWORD)vPid.intVal;
					VariantClear(&vPid);
				}

				char full_path[MAX_PATH_LEN] = {0};
				if (pid) {
					HANDLE hp = OpenProcess(
						PROCESS_QUERY_LIMITED_INFORMATION,
						FALSE, pid);
					if (hp) {
						DWORD sz = MAX_PATH_LEN - 1;
						QueryFullProcessImageNameA(hp, 0,
						                          full_path, &sz);
						full_path[sz] = '\0';
						CloseHandle(hp);
					}
				}

				on_process_start_with_path(exe_lower, full_path);
			} else {
				on_process_stop(exe_lower);
			}
		}
		return WBEM_S_NO_ERROR;
	}

	HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR,
	                                    IWbemClassObject *) override
	{
		return WBEM_S_NO_ERROR;
	}
};

/* ── WMI setup / teardown ───────────────────────────────────────── */
static bool setup_wmi(void)
{
	IWbemLocator *pLoc = NULL;
	HRESULT hr = CoCreateInstance(CLSID_WbemLocator, NULL,
	                              CLSCTX_INPROC_SERVER, IID_IWbemLocator,
	                              (LPVOID *)&pLoc);
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI: CoCreateInstance failed 0x%08lX", hr);
		return false;
	}

	hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, NULL,
	                         0, NULL, NULL, &s_wmi_svc);
	pLoc->Release();
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI: ConnectServer failed 0x%08lX", hr);
		return false;
	}

	hr = CoSetProxyBlanket(s_wmi_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
	                       NULL, RPC_C_AUTHN_LEVEL_CALL,
	                       RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (FAILED(hr)) {
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI: CoSetProxyBlanket failed 0x%08lX", hr);
		s_wmi_svc->Release();
		s_wmi_svc = NULL;
		return false;
	}

	BSTR lang    = SysAllocString(L"WQL");
	BSTR start_q = SysAllocString(L"SELECT * FROM Win32_ProcessStartTrace");
	BSTR stop_q  = SysAllocString(L"SELECT * FROM Win32_ProcessStopTrace");

	s_start_sink = new ProcessEventSink(true);
	hr = s_wmi_svc->ExecNotificationQueryAsync(
		lang, start_q, WBEM_FLAG_SEND_STATUS, NULL, s_start_sink);
	if (FAILED(hr))
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI start trace failed 0x%08lX"
		     " — run OBS as admin for real-time process events", hr);

	s_stop_sink = new ProcessEventSink(false);
	hr = s_wmi_svc->ExecNotificationQueryAsync(
		lang, stop_q, WBEM_FLAG_SEND_STATUS, NULL, s_stop_sink);
	if (FAILED(hr))
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI stop trace failed 0x%08lX", hr);

	SysFreeString(lang);
	SysFreeString(start_q);
	SysFreeString(stop_q);

	blog(LOG_INFO, "[obs-game-detector] WMI event subscriptions active");
	return true;
}

static void teardown_wmi(void)
{
	if (s_wmi_svc) {
		if (s_start_sink) {
			s_wmi_svc->CancelAsyncCall(s_start_sink);
			s_start_sink->Release();
			s_start_sink = NULL;
		}
		if (s_stop_sink) {
			s_wmi_svc->CancelAsyncCall(s_stop_sink);
			s_stop_sink->Release();
			s_stop_sink = NULL;
		}
		s_wmi_svc->Release();
		s_wmi_svc = NULL;
	}
}

static DWORD WINAPI wmi_thread_func(LPVOID)
{
	HRESULT hr    = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool own_com  = SUCCEEDED(hr); /* RPC_E_CHANGED_MODE = already init'd */

	if (FAILED(hr) && hr != (HRESULT)RPC_E_CHANGED_MODE) {
		blog(LOG_WARNING,
		     "[obs-game-detector] WMI thread: CoInitializeEx failed 0x%08lX",
		     hr);
	} else {
		setup_wmi();
	}

	/* No heartbeat needed — source "remove" signal handles reinstatement */
	WaitForSingleObject(s_wmi_stop, INFINITE);

	teardown_wmi();
	if (own_com) CoUninitialize();
	return 0;
}

/* ── public API ─────────────────────────────────────────────────── */
void gd_start(void)
{
	pthread_mutex_init(&s_mutex, NULL);

	gd_config_load();
	build_game_map();
	clear_all_game_sources();
	initial_scan();

	s_wmi_stop   = CreateEvent(NULL, TRUE, FALSE, NULL);
	s_wmi_thread = CreateThread(NULL, 0, wmi_thread_func, NULL, 0, NULL);
}

void gd_stop(void)
{
	if (s_wmi_stop)   SetEvent(s_wmi_stop);
	if (s_wmi_thread) {
		WaitForSingleObject(s_wmi_thread, 10000);
		CloseHandle(s_wmi_thread);
		s_wmi_thread = NULL;
	}
	if (s_wmi_stop) {
		CloseHandle(s_wmi_stop);
		s_wmi_stop = NULL;
	}

	pthread_mutex_lock(&s_mutex);
	s_seen_count = 0;
	pthread_mutex_unlock(&s_mutex);
	pthread_mutex_destroy(&s_mutex);

	gd_config_unload();
}
