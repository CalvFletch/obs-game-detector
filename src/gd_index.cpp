#define WIN32_LEAN_AND_MEAN
#include "gd_api.h"
#include "gd_io.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// path / string helpers
void gd_strlcpy(char *dst, const char *src, size_t cap) {
	if (!dst || cap == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = '\0';
}

void gd_strlower(char *dst, const char *src, size_t cap) {
	size_t i = 0;
	if (!dst || cap == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	for (; i < cap - 1 && src[i]; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

static void path_rstrip_bs(char *path) {
	if (!path)
		return;
	size_t n = strlen(path);
	while (n > 0 && path[n - 1] == '\\')
		path[--n] = '\0';
}

static void path_normalize(char *path) {
	if (!path)
		return;

	char *w = path;
	char *r = path;
	while (*r) {
		if (*r == '/')
			*r = '\\';
		if (*r == '\\') {
			while (r[1] == '\\' || r[1] == '/')
				r++;
			*w++ = '\\';
			r++;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
	path_rstrip_bs(path);
}

static void path_to_install_dir(char *path) {
	if (!path || !path[0])
		return;
	gd_strlower(path, path, GD_MAX_PATH);
	path_normalize(path);
}

static void path_to_watch_dir(char *path) {
	if (!path || !path[0])
		return;

	gd_strlower(path, path, GD_MAX_PATH);
	path_normalize(path);

	static const char *roots[] = {
		"\\steamapps\\common",
		"\\epic games",
		"\\ubisoft game launcher\\games",
		"\\ea games",
		"\\origin games",
		NULL,
	};

	for (int i = 0; roots[i]; i++) {
		char *hit = strstr(path, roots[i]);
		if (!hit)
			continue;
		size_t end = (size_t)(hit - path) + strlen(roots[i]);
		if (end >= GD_MAX_PATH)
			end = GD_MAX_PATH - 1;
		path[end] = '\0';
		return;
	}
}

static GD_GameId game_id_hash(const char *normalized_path) {
	const uint64_t FNV_OFFSET = 14695981039346656037ULL;
	const uint64_t FNV_PRIME  = 1099511628211ULL;
	uint64_t       h          = FNV_OFFSET;

	if (!normalized_path)
		return 0;

	for (const unsigned char *p = (const unsigned char *)normalized_path; *p;
	     p++) {
		h ^= (uint64_t)*p;
		h *= FNV_PRIME;
	}
	return h;
}

void gd_game_id_to_hex(GD_GameId id, char *out, size_t cap) {
	if (!out || cap == 0)
		return;
	snprintf(out, cap, "%016llx", (unsigned long long)id);
}

bool gd_game_id_from_hex(const char *hex, GD_GameId *out) {
	if (!hex || !out)
		return false;
	unsigned long long v = 0;
	if (sscanf(hex, "%llx", &v) != 1)
		return false;
	*out = (GD_GameId)v;
	return true;
}

static bool path_config_file(char *out, size_t cap, const char *name) {
	if (!out || cap == 0 || !name || !name[0])
		return false;

	char *path = obs_module_config_path(name);
	if (!path)
		return false;

	gd_strlcpy(out, path, cap);
	bfree(path);
	return out[0] != '\0';
}

static bool dir_list_contains(char dirs[][GD_MAX_PATH], int count,
                              const char *path) {
	for (int i = 0; i < count; i++)
		if (_stricmp(dirs[i], path) == 0)
			return true;
	return false;
}

bool gd_dir_add_unique(char dirs[][GD_MAX_PATH], int *count, int cap,
                       const char *path) {
	if (!dirs || !count || !path || !path[0] || *count >= cap)
		return false;

	char norm[GD_MAX_PATH];
	gd_strlcpy(norm, path, sizeof(norm));
	path_to_watch_dir(norm);
	if (!norm[0])
		return false;
	if (dir_list_contains(dirs, *count, norm))
		return false;

	gd_strlcpy(dirs[*count], norm, GD_MAX_PATH);
	(*count)++;
	return true;
}

uint64_t gd_wall_ms(void) {
	return (uint64_t)time(NULL) * 1000ULL;
}

bool gd_install_index_cache_path(char *out, size_t cap) {
	return path_config_file(out, cap, "install_index.json");
}

uint32_t gd_tracks_sanitize_mask(uint32_t mask, uint32_t allowed) {
	if (!allowed)
		return 1;
	mask &= allowed;
	if (mask)
		return mask;
	for (int i = 0; i < GD_MAX_AUDIO_TRACKS; i++) {
		if (allowed & (1u << i))
			return (1u << i);
	}
	return 1;
}

void gd_recording_tracks(GD_RecTracks *out) {
	memset(out, 0, sizeof(*out));

	config_t *cfg = obs_frontend_get_profile_config();
	int       rec = 1;

	if (cfg) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		if (mode && strcmp(mode, "Advanced") == 0)
			rec = (int)config_get_uint(cfg, "AdvOut", "RecTracks");
		else
			rec = (int)config_get_int(cfg, "SimpleOutput", "RecTracks");
	}

	for (int i = 0; i < GD_MAX_AUDIO_TRACKS; i++) {
		if (!(rec & (1 << i)))
			continue;
		out->track_nums[out->track_count] = i + 1;
		out->mask |= (1u << i);
		out->track_count++;
	}

	if (out->track_count == 0) {
		out->track_nums[0] = 1;
		out->track_count   = 1;
		out->mask          = 1;
	}
}

static bool json_unescape_quoted(const char **pp, char *out, size_t cap) {
	const char *p = *pp;
	if (*p != '"')
		return false;
	p++;

	size_t i = 0;
	while (*p && *p != '"' && i < cap - 1) {
		if (*p != '\\') {
			out[i++] = *p++;
			continue;
		}
		p++;
		switch (*p) {
		case '\\':
			out[i++] = '\\';
			p++;
			break;
		case '"':
			out[i++] = '"';
			p++;
			break;
		case 'n':
			out[i++] = '\n';
			p++;
			break;
		case 'r':
			out[i++] = '\r';
			p++;
			break;
		case 't':
			out[i++] = '\t';
			p++;
			break;
		case '\0':
			break;
		default:
			out[i++] = *p++;
			break;
		}
	}
	out[i] = '\0';
	if (*p == '"')
		p++;
	*pp = p;
	return i > 0;
}

static bool json_get_str(const char *json, const char *key, char *out,
                         size_t cap) {
	char search[128];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char *p = strstr(json, search);
	if (!p)
		return false;
	p += strlen(search);
	while (*p == ' ' || *p == ':' || *p == '\t')
		p++;
	return json_unescape_quoted(&p, out, cap);
}

// install index
uint64_t gd_install_index_normalize_built_ms(uint64_t raw) {
	uint64_t now = gd_wall_ms();
	if (raw < 1577836800000ULL)
		return now;
	if (raw > now)
		return now;
	return raw;
}

static bool dir_exists(const char *path) {
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void add_entry(GD_InstallIndex *idx, const char *dir, const char *name) {
	if (idx->entry_count >= GD_MAP_CAP || !dir || !dir[0] || !name || !name[0])
		return;

	char lower[GD_MAX_PATH];
	gd_strlcpy(lower, dir, sizeof(lower));
	path_to_install_dir(lower);

	for (int i = 0; i < idx->entry_count; i++)
		if (strcmp(idx->entries[i].install_dir, lower) == 0)
			return;

	GD_InstallEntry *e = &idx->entries[idx->entry_count++];
	e->id = game_id_hash(lower);
	gd_strlcpy(e->install_dir, lower, GD_MAX_PATH);
	gd_strlcpy(e->display_name, name, sizeof(e->display_name));
}

static void add_lookup(GD_InstallIndex *idx, const char *path) {
	if (idx->lookup_dir_count >= GD_MAX_LOOKUP_DIRS || !path || !path[0])
		return;

	char lower[GD_MAX_PATH];
	gd_strlcpy(lower, path, sizeof(lower));
	path_to_install_dir(lower);

	for (int i = 0; i < idx->lookup_dir_count; i++)
		if (_stricmp(idx->lookup_dirs[i], lower) == 0)
			return;

	gd_strlcpy(idx->lookup_dirs[idx->lookup_dir_count], lower, GD_MAX_PATH);
	idx->lookup_dir_count++;
}

#define GD_MANIFEST_MAX_SZ (8 * 1024 * 1024)

typedef bool (*gd_manifest_parse_fn)(const char *buf, char *dir, size_t dcap,
                                     char *name, size_t ncap);

static void scan_manifest_dir(GD_InstallIndex *idx, const char *dir,
                              const char *file_glob, gd_manifest_parse_fn parse) {
	char glob_path[GD_MAX_PATH];
	snprintf(glob_path, sizeof(glob_path), "%s\\%s", dir, file_glob);

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(glob_path, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	do {
		char file_path[GD_MAX_PATH];
		snprintf(file_path, sizeof(file_path), "%s\\%s", dir, fd.cFileName);
		char *buf = gd_file_read_alloc(file_path, GD_MANIFEST_MAX_SZ);
		if (!buf)
			continue;

		char out_dir[GD_MAX_PATH];
		char name[256] = {0};
		gd_strlcpy(out_dir, dir, sizeof(out_dir));
		if (parse(buf, out_dir, sizeof(out_dir), name, sizeof(name)))
			add_entry(idx, out_dir, name);
		bfree(buf);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

static bool parse_steam_manifest(const char *buf, char *dir, size_t dcap,
                                 char *name, size_t ncap) {
	char idir[GD_MAX_PATH];
	if (!json_get_str(buf, "name", name, ncap) ||
	    !json_get_str(buf, "installdir", idir, sizeof(idir)))
		return false;
	snprintf(dir, dcap, "%s\\common\\%s", dir, idir);
	return true;
}

static bool parse_epic_manifest(const char *buf, char *dir, size_t dcap,
                                char *name, size_t ncap) {
	return json_get_str(buf, "DisplayName", name, ncap) &&
	       json_get_str(buf, "InstallLocation", dir, dcap);
}

static void fprint_json_string(FILE *f, const char *s) {
	fputc('"', f);
	if (!s) {
		fputc('"', f);
		return;
	}
	for (; *s; s++) {
		switch (*s) {
		case '\\':
			fputs("\\\\", f);
			break;
		case '"':
			fputs("\\\"", f);
			break;
		case '\n':
			fputs("\\n", f);
			break;
		case '\r':
			fputs("\\r", f);
			break;
		case '\t':
			fputs("\\t", f);
			break;
		default:
			fputc(*s, f);
		}
	}
	fputc('"', f);
}

static bool entry_path_valid(const char *path) {
	if (!path || !path[0])
		return false;
	return strchr(path, '\\') != NULL;
}

static bool index_cache_valid(const GD_InstallIndex *idx) {
	if (idx->entry_count == 0)
		return false;
	for (int i = 0; i < idx->entry_count; i++) {
		if (!entry_path_valid(idx->entries[i].install_dir))
			return false;
	}
	return true;
}

static void scan_steam_library(GD_InstallIndex *idx, const char *library_path) {
	scan_manifest_dir(idx, library_path, "appmanifest_*.acf",
	                  parse_steam_manifest);

	char common[GD_MAX_PATH];
	snprintf(common, sizeof(common), "%s\\common", library_path);
	add_lookup(idx, common);
}

static void scan_steam(GD_InstallIndex *idx) {
	HKEY hk;
	char steam_path[GD_MAX_PATH] = {0};
	if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", 0, KEY_READ,
	                  &hk) == ERROR_SUCCESS) {
		DWORD sz = sizeof(steam_path);
		RegQueryValueExA(hk, "SteamPath", NULL, NULL, (LPBYTE)steam_path, &sz);
		RegCloseKey(hk);
		path_normalize(steam_path);
	}

	if (!steam_path[0])
		return;

	char lib0[GD_MAX_PATH];
	snprintf(lib0, sizeof(lib0), "%s\\steamapps", steam_path);
	scan_steam_library(idx, lib0);

	char vdf[GD_MAX_PATH];
	snprintf(vdf, sizeof(vdf), "%s\\steamapps\\libraryfolders.vdf",
	         steam_path);
	char *buf = gd_file_read_alloc(vdf, GD_MANIFEST_MAX_SZ);
	if (buf) {
		const char *p = buf;
		while ((p = strstr(p, "\"path\"")) != NULL) {
			char extra[GD_MAX_PATH];
			if (json_get_str(p, "path", extra, sizeof(extra))) {
				path_normalize(extra);
				char lib[GD_MAX_PATH];
				snprintf(lib, sizeof(lib), "%s\\steamapps", extra);
				scan_steam_library(idx, lib);
			}
			p++;
		}
		bfree(buf);
	}
}

static void scan_epic(GD_InstallIndex *idx) {
	const char *epic_dir =
		"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
	scan_manifest_dir(idx, epic_dir, "*.item", parse_epic_manifest);

	const char *pf   = getenv("ProgramFiles");
	const char *pf86 = getenv("ProgramFiles(x86)");
	char epic[GD_MAX_PATH];
	if (pf) {
		snprintf(epic, sizeof(epic), "%s\\Epic Games", pf);
		if (dir_exists(epic))
			add_lookup(idx, epic);
	}
	if (pf86) {
		snprintf(epic, sizeof(epic), "%s\\Epic Games", pf86);
		if (dir_exists(epic))
			add_lookup(idx, epic);
	}
}

static void scan_gog(GD_InstallIndex *idx) {
	HKEY hgog;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\GOG.com\\Games",
	                  0, KEY_READ, &hgog) != ERROR_SUCCESS)
		return;

	char sub[256];
	DWORD gidx = 0, sublen = sizeof(sub);
	while (RegEnumKeyExA(hgog, gidx++, sub, &sublen, NULL, NULL, NULL,
	                     NULL) == ERROR_SUCCESS) {
		sublen = sizeof(sub);
		HKEY hg;
		if (RegOpenKeyExA(hgog, sub, 0, KEY_READ, &hg) == ERROR_SUCCESS) {
			char path[GD_MAX_PATH] = {0}, gname[256] = {0};
			DWORD psz = sizeof(path), nsz = sizeof(gname);
			RegQueryValueExA(hg, "path", NULL, NULL, (LPBYTE)path, &psz);
			RegQueryValueExA(hg, "GAMENAME", NULL, NULL, (LPBYTE)gname, &nsz);
			if (path[0] && gname[0])
				add_entry(idx, path, gname);
			RegCloseKey(hg);
		}
	}
	RegCloseKey(hgog);
}

static void scan_ubisoft(GD_InstallIndex *idx) {
	const char *pf86 = getenv("ProgramFiles(x86)");
	const char *pf   = getenv("ProgramFiles");
	char buf[GD_MAX_PATH];
	if (pf86) {
		snprintf(buf, sizeof(buf),
		         "%s\\Ubisoft\\Ubisoft Game Launcher\\games", pf86);
		if (dir_exists(buf))
			add_lookup(idx, buf);
	}
	if (pf) {
		snprintf(buf, sizeof(buf),
		         "%s\\Ubisoft\\Ubisoft Game Launcher\\games", pf);
		if (dir_exists(buf))
			add_lookup(idx, buf);
	}

	HKEY hub;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
	                  "SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs", 0,
	                  KEY_READ, &hub) != ERROR_SUCCESS)
		return;

	char sub[256];
	DWORD uidx = 0, sublen = sizeof(sub);
	while (RegEnumKeyExA(hub, uidx++, sub, &sublen, NULL, NULL, NULL,
	                     NULL) == ERROR_SUCCESS) {
		sublen = sizeof(sub);
		HKEY hg;
		if (RegOpenKeyExA(hub, sub, 0, KEY_READ, &hg) == ERROR_SUCCESS) {
			char idir[GD_MAX_PATH] = {0};
			DWORD dsz = sizeof(idir);
			RegQueryValueExA(hg, "InstallDir", NULL, NULL, (LPBYTE)idir, &dsz);
			RegCloseKey(hg);
			if (idir[0]) {
				path_normalize(idir);
				const char *nm = strrchr(idir, '\\');
				nm = nm ? nm + 1 : idir;
				if (*nm)
					add_entry(idx, idir, nm);
			}
		}
	}
	RegCloseKey(hub);
}

static void scan_ea_origin(GD_InstallIndex *idx) {
	const char *pf   = getenv("ProgramFiles");
	const char *pf86 = getenv("ProgramFiles(x86)");
	const char *names[] = {"EA Games", "Origin Games", NULL};
	char buf[GD_MAX_PATH];
	for (int i = 0; names[i]; i++) {
		if (pf) {
			snprintf(buf, sizeof(buf), "%s\\%s", pf, names[i]);
			if (dir_exists(buf))
				add_lookup(idx, buf);
		}
		if (pf86) {
			snprintf(buf, sizeof(buf), "%s\\%s", pf86, names[i]);
			if (dir_exists(buf))
				add_lookup(idx, buf);
		}
	}
}

void gd_install_index_build(GD_InstallIndex *idx) {
	memset(idx, 0, sizeof(*idx));

	scan_steam(idx);
	scan_epic(idx);
	scan_gog(idx);
	scan_ubisoft(idx);
	scan_ea_origin(idx);

	if (idx->lookup_dir_count == 0)
		blog(LOG_ERROR,
		     "[obs-game-detector] install index build produced no lookup dirs");

	gd_lookup_sanitize_index_dirs(idx);
	idx->built_ms = gd_wall_ms();
}

void gd_install_index_save(const GD_InstallIndex *idx, const char *cache_path) {
	if (!idx || !cache_path)
		return;

	char dir[1024];
	gd_strlcpy(dir, cache_path, sizeof(dir));
	char *sep = strrchr(dir, '/');
	if (!sep)
		sep = strrchr(dir, '\\');
	if (sep) {
		*sep = '\0';
		os_mkdirs(dir);
	}

	char tmp[1100], bak[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
	snprintf(bak, sizeof(bak), "%s.bak", cache_path);

	FILE *f;
	if (fopen_s(&f, tmp, "wb") != 0 || !f)
		return;

	fprintf(f, "{\n  \"built_ms\": %llu,\n  \"entries\": [\n",
	        (unsigned long long)idx->built_ms);
	for (int i = 0; i < idx->entry_count; i++) {
		char idhex[32];
		gd_game_id_to_hex(idx->entries[i].id, idhex, sizeof(idhex));
		fprintf(f, "    {\"game_id\":\"%s\",\"install_dir\":", idhex);
		fprint_json_string(f, idx->entries[i].install_dir);
		fprintf(f, ",\"display_name\":");
		fprint_json_string(f, idx->entries[i].display_name);
		fprintf(f, "}%s\n", (i + 1 < idx->entry_count) ? "," : "");
	}
	fprintf(f, "  ],\n  \"lookup_dirs\": [\n");
	for (int i = 0; i < idx->lookup_dir_count; i++) {
		fprintf(f, "    ");
		fprint_json_string(f, idx->lookup_dirs[i]);
		fprintf(f, "%s\n", (i + 1 < idx->lookup_dir_count) ? "," : "");
	}
	fprintf(f, "  ]\n}\n");
	fclose(f);

	DeleteFileA(bak);
	MoveFileExA(cache_path, bak, MOVEFILE_REPLACE_EXISTING);
	MoveFileExA(tmp, cache_path, MOVEFILE_REPLACE_EXISTING);
}

static bool load_from_json(GD_InstallIndex *idx, const char *path) {
	char *json = gd_file_read_alloc(path, GD_MANIFEST_MAX_SZ);
	if (!json)
		return false;

	memset(idx, 0, sizeof(*idx));

	const char *p = strstr(json, "\"built_ms\"");
	if (p) {
		unsigned long long ms = 0;
		const char *colon = strchr(p, ':');
		if (colon)
			sscanf(colon + 1, "%llu", &ms);
		idx->built_ms = ms;
	}

	p = strstr(json, "\"entries\"");
	if (p) {
		p = strchr(p, '[');
		while (p && idx->entry_count < GD_MAP_CAP) {
			p = strstr(p, "\"install_dir\"");
			if (!p)
				break;
			char idir[GD_MAX_PATH] = {0};
			char dname[256]       = {0};
			json_get_str(p, "install_dir", idir, sizeof(idir));
			const char *dn = strstr(p, "\"display_name\"");
			if (dn)
				json_get_str(dn, "display_name", dname, sizeof(dname));
			if (idir[0] && dname[0])
				add_entry(idx, idir, dname);
			p = strchr(p + 1, '}');
			if (!p)
				break;
			p++;
		}
	}

	p = strstr(json, "\"lookup_dirs\"");
	if (p) {
		p = strchr(p, '[');
		if (p) {
			p++;
			while (idx->lookup_dir_count < GD_MAX_LOOKUP_DIRS) {
				while (*p && *p != '"' && *p != ']')
					p++;
				if (*p == ']')
					break;
				if (*p != '"')
					break;
				char dir[GD_MAX_PATH];
				if (!json_unescape_quoted(&p, dir, sizeof(dir)))
					break;
				add_lookup(idx, dir);
				while (*p && *p != '"' && *p != ']')
					p++;
			}
		}
	}

	bfree(json);
	return idx->entry_count > 0 || idx->lookup_dir_count > 0;
}

bool gd_install_index_load_or_build(GD_InstallIndex *idx, const char *cache_path) {
	if (cache_path && load_from_json(idx, cache_path) &&
	    index_cache_valid(idx)) {
		if (gd_lookup_sanitize_index_dirs(idx)) {
			gd_install_index_save(idx, cache_path);
			blog(LOG_INFO,
			     "[obs-game-detector] install index lookup dirs migrated to library roots: %d dirs",
			     idx->lookup_dir_count);
		}
		blog(LOG_INFO,
		     "[obs-game-detector] install index loaded from cache: %d entries, %d lookup dirs",
		     idx->entry_count, idx->lookup_dir_count);
		return true;
	}

	blog(LOG_INFO,
	     "[obs-game-detector] install index cache invalid: rebuilding");
	gd_install_index_build(idx);
	if (cache_path)
		gd_install_index_save(idx, cache_path);
	return true;
}

static HANDLE s_index_worker = NULL;

static DWORD WINAPI index_worker_thread(LPVOID param) {
	GD_State *state = (GD_State *)param;
	if (!state)
		return 1;

	gd_install_index_build(&state->index_scratch);

	GD_Phase phase = state->phase;
	if (phase == GD_PHASE_IDLE || phase == GD_PHASE_STOPPING)
		return 0;

	char cache[1024];
	if (gd_install_index_cache_path(cache, sizeof(cache)))
		gd_install_index_save(&state->index_scratch, cache);

	GD_Event evt = {};
	evt.kind           = GD_EVT_INDEX_READY;
	evt.index_built_ms = state->index_scratch.built_ms;
	if (state->post_event)
		state->post_event(&evt);
	return 0;
}

void gd_index_apply_ready(GD_State *state, uint64_t built_ms) {
	if (!state)
		return;

	memcpy(&state->index, &state->index_scratch, sizeof(state->index));
	state->index_built_ms = built_ms;
	gd_lookup_build(&state->lookup, &state->index, &state->config);

	char cache[1024];
	if (gd_install_index_cache_path(cache, sizeof(cache)))
		gd_install_index_save(&state->index, cache);

	state->index_rebuild_pending = false;
	if (s_index_worker) {
		CloseHandle(s_index_worker);
		s_index_worker = NULL;
	}

	blog(LOG_INFO,
	     "[obs-game-detector] install index refreshed: %d entries, %d lookup dirs",
	     state->index.entry_count, state->index.lookup_dir_count);
}

void gd_install_index_worker_join(void) {
	if (!s_index_worker)
		return;
	WaitForSingleObject(s_index_worker, INFINITE);
	CloseHandle(s_index_worker);
	s_index_worker = NULL;
}

void gd_install_index_rebuild_async(GD_State *state) {
	if (!state || state->index_rebuild_pending)
		return;

	gd_install_index_worker_join();

	state->index_rebuild_pending = true;
	s_index_worker = CreateThread(NULL, 0, index_worker_thread, state, 0, NULL);
	if (!s_index_worker)
		state->index_rebuild_pending = false;
}

// lookup
static int cmp_dir_len_desc(const void *a, const void *b) {
	const char *sa = *(const char *const *)a;
	const char *sb = *(const char *const *)b;
	size_t      la = strlen(sa);
	size_t      lb = strlen(sb);
	if (la > lb)
		return -1;
	if (la < lb)
		return 1;
	return 0;
}

bool gd_lookup_matches_full_path(const GD_LookupTable *lt,
                                 const char *full_path) {
	if (!lt || !full_path || !full_path[0])
		return false;

	char path_lower[GD_MAX_PATH];
	gd_strlcpy(path_lower, full_path, sizeof(path_lower));
	path_to_install_dir(path_lower);
	return gd_lookup_path_matches(lt, path_lower);
}

void gd_lookup_build(GD_LookupTable *out, const GD_InstallIndex *idx,
                     const GD_ConfigSnap *cfg) {
	char *ptrs[GD_MAX_LOOKUP_DIRS];
	int   n = 0;

	memset(out, 0, sizeof(*out));

	if (idx) {
		for (int i = 0; i < idx->lookup_dir_count && n < GD_MAX_LOOKUP_DIRS;
		     i++) {
			if (gd_dir_add_unique(out->dirs, &out->dir_count,
			                      GD_MAX_LOOKUP_DIRS,
			                      idx->lookup_dirs[i]))
				ptrs[n++] = out->dirs[out->dir_count - 1];
		}
	}

	if (cfg) {
		for (int i = 0; i < cfg->custom_dir_count && n < GD_MAX_LOOKUP_DIRS;
		     i++) {
			if (gd_dir_add_unique(out->dirs, &out->dir_count,
			                      GD_MAX_LOOKUP_DIRS,
			                      cfg->custom_dirs[i]))
				ptrs[n++] = out->dirs[out->dir_count - 1];
		}
	}

	if (n > 1)
		qsort(ptrs, (size_t)n, sizeof(char *), cmp_dir_len_desc);
}

bool gd_lookup_path_matches(const GD_LookupTable *lt, const char *path_lower) {
	if (!lt || !path_lower || !path_lower[0])
		return false;

	for (int i = 0; i < lt->dir_count; i++) {
		const char *dir = lt->dirs[i];
		size_t      len = strlen(dir);
		if (len == 0)
			continue;

		const char *hit = strstr(path_lower, dir);
		if (!hit)
			continue;

		char next = hit[len];
		if (next == '\0' || next == '\\')
			return true;
	}
	return false;
}

static bool resolve_from_index(const GD_InstallIndex *idx, char *dir_lower,
                               GD_GameId *id_out, char *display_out,
                               size_t display_cap, char *install_dir_out,
                               size_t install_cap) {
	for (int depth = 0; depth < 6; depth++) {
		const char *leaf = strrchr(dir_lower, '\\');
		leaf = leaf ? leaf + 1 : dir_lower;

		for (int i = 0; i < idx->entry_count; i++) {
			const GD_InstallEntry *e = &idx->entries[i];
			bool match = strcmp(e->install_dir, dir_lower) == 0;

			if (!match) {
				const char *el = strrchr(e->install_dir, '\\');
				el = el ? el + 1 : e->install_dir;
				match = _stricmp(el, leaf) == 0;
			}

			if (match) {
				if (id_out)
					*id_out = e->id;
				if (display_out)
					gd_strlcpy(display_out, e->display_name,
					           display_cap);
				if (install_dir_out)
					gd_strlcpy(install_dir_out, e->install_dir,
					           install_cap);
				return true;
			}
		}

		char *bs = strrchr(dir_lower, '\\');
		if (!bs)
			break;
		*bs = '\0';
		path_rstrip_bs(dir_lower);
	}
	return false;
}

bool gd_lookup_resolve(const GD_State *state, const char *full_path,
                       GD_GameId *id_out, char *display_out, size_t display_cap,
                       char *install_dir_out, size_t install_cap) {
	if (!state || !full_path || !full_path[0] || !display_out || display_cap == 0)
		return false;

	char dir_lower[GD_MAX_PATH];
	gd_strlcpy(dir_lower, full_path, sizeof(dir_lower));
	path_to_install_dir(dir_lower);

	char *last_bs = strrchr(dir_lower, '\\');
	if (last_bs)
		*last_bs = '\0';
	path_rstrip_bs(dir_lower);

	return resolve_from_index(&state->index, dir_lower, id_out, display_out,
	                          display_cap, install_dir_out, install_cap);
}

bool gd_lookup_dir_covered_by_index(const GD_InstallIndex *idx, const char *dir) {
	if (!idx || !dir || !dir[0])
		return false;

	char norm[GD_MAX_PATH];
	gd_strlcpy(norm, dir, sizeof(norm));
	path_to_watch_dir(norm);

	for (int i = 0; i < idx->lookup_dir_count; i++) {
		char root[GD_MAX_PATH];
		gd_strlcpy(root, idx->lookup_dirs[i], sizeof(root));
		path_to_watch_dir(root);
		if (_stricmp(norm, root) == 0)
			return true;
	}
	return false;
}

GD_GameId gd_index_id_for_display(const GD_InstallIndex *idx,
                                  const char *display_name) {
	if (!idx || !display_name || !display_name[0])
		return 0;

	for (int i = 0; i < idx->entry_count; i++) {
		if (_stricmp(idx->entries[i].display_name, display_name) == 0)
			return idx->entries[i].id;
	}
	return 0;
}

const char *gd_index_display_name(const GD_InstallIndex *idx, GD_GameId id) {
	if (!idx || id == 0)
		return NULL;

	for (int i = 0; i < idx->entry_count; i++) {
		if (idx->entries[i].id == id)
			return idx->entries[i].display_name;
	}
	return NULL;
}

bool gd_lookup_sanitize_index_dirs(GD_InstallIndex *idx) {
	if (!idx)
		return false;

	char dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int  n     = 0;
	bool dirty = false;

	for (int i = 0; i < idx->lookup_dir_count; i++) {
		char root[GD_MAX_PATH];
		char orig[GD_MAX_PATH];

		gd_strlcpy(orig, idx->lookup_dirs[i], sizeof(orig));
		gd_strlcpy(root, orig, sizeof(root));
		path_to_watch_dir(root);

		if (_stricmp(orig, root) != 0)
			dirty = true;

		if (gd_dir_add_unique(dirs, &n, GD_MAX_LOOKUP_DIRS, root)) {
		} else {
			dirty = true;
		}
	}

	if (n != idx->lookup_dir_count)
		dirty = true;

	idx->lookup_dir_count = n;
	memcpy(idx->lookup_dirs, dirs, sizeof(dirs[0]) * (size_t)n);
	return dirty;
}

void gd_lookup_default_dirs(const GD_InstallIndex *idx, char dirs[][GD_MAX_PATH],
                            int *count, int cap) {
	*count = 0;
	if (!idx)
		return;

	for (int i = 0; i < idx->lookup_dir_count && *count < cap; i++)
		gd_dir_add_unique(dirs, count, cap, idx->lookup_dirs[i]);
}

// config
static void today_str(char *out, size_t cap) {
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	strftime(out, cap, "%Y-%m-%d", t);
}

static void add_dir_unique(GD_ConfigSnap *snap, const char *path) {
	if (!snap)
		return;
	gd_dir_add_unique(snap->custom_dirs, &snap->custom_dir_count,
	                   GD_MAX_LOOKUP_DIRS, path);
}

static void snap_sanitize_tracks(GD_ConfigSnap *snap, bool *dirty) {
	GD_RecTracks rec;
	gd_recording_tracks(&rec);

	if (snap->default_tracks == 0)
		snap->default_tracks = 0x03;

	uint32_t def = gd_tracks_sanitize_mask(snap->default_tracks, rec.mask);
	if (def != snap->default_tracks) {
		snap->default_tracks = def;
		*dirty               = true;
	}

	for (int i = 0; i < snap->game_count; i++) {
		if (!snap->games[i].tracks_override)
			continue;

		uint32_t t =
			gd_tracks_sanitize_mask(snap->games[i].tracks, rec.mask);
		if (t != snap->games[i].tracks) {
			snap->games[i].tracks = t;
			*dirty                = true;
		}
		if (t == snap->default_tracks) {
			snap->games[i].tracks_override = false;
			*dirty                         = true;
		}
	}
}

static bool snap_sanitize(GD_ConfigSnap *snap, const GD_InstallIndex *idx) {
	if (!snap)
		return false;

	bool dirty = false;

	snap_sanitize_tracks(snap, &dirty);

	char dirs[GD_MAX_LOOKUP_DIRS][GD_MAX_PATH];
	int  dir_count = 0;
	for (int i = 0; i < snap->custom_dir_count; i++) {
		if (idx && gd_lookup_dir_covered_by_index(idx, snap->custom_dirs[i])) {
			dirty = true;
			continue;
		}
		if (!gd_dir_add_unique(dirs, &dir_count, GD_MAX_LOOKUP_DIRS,
		                       snap->custom_dirs[i]))
			dirty = true;
	}
	if (dir_count != snap->custom_dir_count)
		dirty = true;
	snap->custom_dir_count = dir_count;
	memcpy(snap->custom_dirs, dirs, sizeof(dirs[0]) * (size_t)dir_count);

	GD_ConfigRecord games[GD_MAX_GAMES];
	int game_count = 0;
	for (int i = 0; i < snap->game_count; i++) {
		if (snap->games[i].id == 0)
			continue;

		bool found = false;
		for (int j = 0; j < game_count; j++) {
			if (games[j].id != snap->games[i].id)
				continue;

			if (strcmp(snap->games[i].last_seen, games[j].last_seen) > 0) {
				gd_strlcpy(games[j].last_seen, snap->games[i].last_seen,
				           sizeof(games[j].last_seen));
				gd_strlcpy(games[j].display_name,
				           snap->games[i].display_name,
				           sizeof(games[j].display_name));
			}
			if (!snap->games[i].enabled)
				games[j].enabled = false;
			found = true;
			break;
		}
		if (!found && game_count < GD_MAX_GAMES)
			games[game_count++] = snap->games[i];
	}
	if (game_count != snap->game_count)
		dirty = true;
	snap->game_count = game_count;
	memcpy(snap->games, games, sizeof(games[0]) * (size_t)game_count);

	return dirty;
}

static void save_json(const GD_ConfigSnap *snap) {
	char path[1024];
	if (!path_config_file(path, sizeof(path), "config.json") || !snap)
		return;

	char dir[1024];
	gd_strlcpy(dir, path, sizeof(dir));
	char *sep = strrchr(dir, '/');
	if (!sep)
		sep = strrchr(dir, '\\');
	if (sep) {
		*sep = '\0';
		os_mkdirs(dir);
	}

	obs_data_t *root = obs_data_create();

	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < snap->game_count; i++) {
		const GD_ConfigRecord *r = &snap->games[i];
		char idhex[32];
		gd_game_id_to_hex(r->id, idhex, sizeof(idhex));
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "game_id", idhex);
		obs_data_set_string(item, "display_name", r->display_name);
		obs_data_set_string(item, "last_seen", r->last_seen);
		obs_data_set_bool(item, "enabled", r->enabled);
		if (r->tracks_override) {
			obs_data_set_int(item, "tracks", (int)r->tracks);
			obs_data_set_bool(item, "tracks_override", true);
		}
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "games", arr);
	obs_data_array_release(arr);

	if (snap->default_tracks)
		obs_data_set_int(root, "default_tracks", (int)snap->default_tracks);

	obs_data_array_t *darr = obs_data_array_create();
	for (int i = 0; i < snap->custom_dir_count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", snap->custom_dirs[i]);
		obs_data_array_push_back(darr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "dirs", darr);
	obs_data_array_release(darr);

	obs_data_array_t *sarr = obs_data_array_create();
	for (int i = 0; i < snap->scene_count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", snap->scenes[i]);
		obs_data_array_push_back(sarr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "scenes", sarr);
	obs_data_array_release(sarr);

	obs_data_save_json_safe(root, path, ".tmp", ".bak");
	obs_data_release(root);
}

static void load_json(GD_ConfigSnap *snap, const char *path,
                      const GD_InstallIndex *seed_index) {
	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		return;

	obs_data_array_t *arr = obs_data_get_array(data, "games");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n && snap->game_count < GD_MAX_GAMES; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *idhex = obs_data_get_string(item, "game_id");
			const char *nm    = obs_data_get_string(item, "display_name");
			if (!nm || !*nm)
				nm = obs_data_get_string(item, "name");

			GD_GameId gid = 0;
			if (idhex && *idhex)
				gd_game_id_from_hex(idhex, &gid);
			if (gid == 0 && nm && *nm && seed_index)
				gid = gd_index_id_for_display(seed_index, nm);
			if (gid == 0) {
				obs_data_release(item);
				continue;
			}

			GD_ConfigRecord *r = &snap->games[snap->game_count++];
			r->id = gid;
			const char *idx_nm =
				seed_index ? gd_index_display_name(seed_index, gid) : NULL;
			if (idx_nm && idx_nm[0])
				gd_strlcpy(r->display_name, idx_nm,
				           sizeof(r->display_name));
			else if (nm && *nm)
				gd_strlcpy(r->display_name, nm,
				           sizeof(r->display_name));
			const char *ls = obs_data_get_string(item, "last_seen");
			if (ls)
				gd_strlcpy(r->last_seen, ls, sizeof(r->last_seen));
			r->enabled = obs_data_has_user_value(item, "enabled")
			                 ? obs_data_get_bool(item, "enabled")
			                 : true;
			if (obs_data_has_user_value(item, "tracks_override") &&
			    obs_data_get_bool(item, "tracks_override")) {
				r->tracks_override = true;
				r->tracks =
					(uint32_t)obs_data_get_int(item, "tracks");
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	obs_data_array_t *darr = obs_data_get_array(data, "dirs");
	if (!darr)
		darr = obs_data_get_array(data, "custom_dirs");
	if (darr) {
		size_t n = obs_data_array_count(darr);
		for (size_t i = 0; i < n && snap->custom_dir_count < GD_MAX_LOOKUP_DIRS;
		     i++) {
			obs_data_t *item = obs_data_array_item(darr, i);
			const char *v    = obs_data_get_string(item, "value");
			if (v && *v)
				add_dir_unique(snap, v);
			obs_data_release(item);
		}
		obs_data_array_release(darr);
	}

	obs_data_array_t *sarr = obs_data_get_array(data, "scenes");
	if (sarr) {
		size_t n = obs_data_array_count(sarr);
		for (size_t i = 0; i < n && snap->scene_count < GD_MAX_SCENES; i++) {
			obs_data_t *item = obs_data_array_item(sarr, i);
			const char *v    = obs_data_get_string(item, "value");
			if (v && *v) {
				gd_strlcpy(snap->scenes[snap->scene_count], v,
				           GD_MAX_SCENE_LEN);
				snap->scene_count++;
			}
			obs_data_release(item);
		}
		obs_data_array_release(sarr);
	}

	if (obs_data_has_user_value(data, "default_tracks"))
		snap->default_tracks =
			(uint32_t)obs_data_get_int(data, "default_tracks");

	obs_data_release(data);
}

uint32_t gd_config_mixer_mask(const GD_ConfigSnap *cfg, GD_GameId id) {
	if (!cfg)
		return 0x03;

	GD_RecTracks rec;
	gd_recording_tracks(&rec);

	uint32_t mask = gd_tracks_sanitize_mask(
		cfg->default_tracks ? cfg->default_tracks : 0x03, rec.mask);

	const GD_ConfigRecord *r = gd_config_find(cfg, id);
	if (r && r->tracks_override)
		mask = gd_tracks_sanitize_mask(r->tracks, rec.mask);

	return mask;
}

void gd_config_load(GD_State *state) {
	if (!state)
		return;

	GD_ConfigSnap scratch = {};
	char          path[1024];
	if (path_config_file(path, sizeof(path), "config.json"))
		load_json(&scratch, path, &state->index);

	if (snap_sanitize(&scratch, &state->index))
		save_json(&scratch);

	state->config = scratch;
}

bool gd_config_apply(GD_State *state, const GD_ConfigSnap *scratch) {
	if (!state || !scratch)
		return false;

	GD_ConfigSnap committed = *scratch;
	snap_sanitize(&committed, &state->index);
	state->config = committed;
	gd_lookup_build(&state->lookup, &state->index, &state->config);
	save_json(&state->config);
	return true;
}

const GD_ConfigRecord *gd_config_find(const GD_ConfigSnap *cfg, GD_GameId id) {
	if (!cfg)
		return NULL;
	for (int i = 0; i < cfg->game_count; i++)
		if (cfg->games[i].id == id)
			return &cfg->games[i];
	return NULL;
}

bool gd_config_is_enabled(const GD_ConfigSnap *cfg, GD_GameId id) {
	const GD_ConfigRecord *r = gd_config_find(cfg, id);
	return r && r->enabled;
}

void gd_config_record_game(GD_State *state, GD_GameId id,
                           const char *display_name) {
	if (!state || !display_name || !*display_name || id == 0)
		return;

	const char *label = display_name;
	const char *idx_nm = gd_index_display_name(&state->index, id);
	if (idx_nm && idx_nm[0])
		label = idx_nm;

	char today[16];
	today_str(today, sizeof(today));

	for (int i = 0; i < state->config.game_count; i++) {
		if (state->config.games[i].id == id) {
			gd_strlcpy(state->config.games[i].last_seen, today,
			           sizeof(state->config.games[i].last_seen));
			gd_strlcpy(state->config.games[i].display_name, label,
			           sizeof(state->config.games[i].display_name));
			save_json(&state->config);
			return;
		}
	}

	if (state->config.game_count >= GD_MAX_GAMES)
		return;

	GD_ConfigRecord *r =
		&state->config.games[state->config.game_count++];
	r->id      = id;
	r->enabled = true;
	gd_strlcpy(r->display_name, label, sizeof(r->display_name));
	gd_strlcpy(r->last_seen, today, sizeof(r->last_seen));
	save_json(&state->config);
}
