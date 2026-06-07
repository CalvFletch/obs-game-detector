#include "gd_skip_rules.h"

#include "gd_api.h"
#include "gd_io.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <stdio.h>
#include <string.h>

#define GD_MAX_SKIP_EXACT  48
#define GD_MAX_SKIP_SUBSTR 32
#define GD_MAX_SKIP_LEN    128

static char s_exact[GD_MAX_SKIP_EXACT][GD_MAX_SKIP_LEN];
static int  s_exact_count;
static char s_substr[GD_MAX_SKIP_SUBSTR][GD_MAX_SKIP_LEN];
static int  s_substr_count;

static const char *DEFAULT_EXACT[] = {
	"unitycrashhandler64.exe", "unitycrashhandler.exe",
	"unitycrashandler64.exe", "unitycrashandler.exe",
	"crashpad_handler.exe",
	"easyanticheats.exe",     "easyanticheat_launcher.exe",
	"beservice.exe",          "bedaisy.exe",
	"steamservice.exe",       "steam.exe",           "steamwebhelper.exe",
	"gameoverlayui.exe",      "gameoverlayui64.exe",
	"cefsubprocess.exe",      "unrealcefsubprocess.exe",
	"dotnet.exe",             "node.exe",            "java.exe",
	"javaw.exe",              "steamshim.exe",       "battleye.exe",
	"battleyelauncher.exe",   "nprotect.exe",        "gameguard.exe",
	NULL,
};

static const char *DEFAULT_SUBSTR[] = {
	"crash", "handler", "service", "setup", "install", "uninstall", "redist",
	"helper", "updater", "anticheat", "anti_cheat", "overlay", "cefsubprocess",
	"shim", "monitor", "report", "sender", NULL,
};

static void skip_add_to_list(char list[][GD_MAX_SKIP_LEN], int *count, int cap,
                             const char *s) {
	if (!s || !s[0] || *count >= cap)
		return;
	for (int i = 0; i < *count; i++)
		if (strcmp(list[i], s) == 0)
			return;
	gd_strlcpy(list[*count], s, GD_MAX_SKIP_LEN);
	(*count)++;
}

static void skip_write_defaults(const char *path) {
	FILE *f;
	if (fopen_s(&f, path, "wb") != 0 || !f)
		return;

	fprintf(f, "{\n  \"exe_exact\": [\n");
	for (int i = 0; DEFAULT_EXACT[i]; i++)
		fprintf(f, "    \"%s\"%s\n", DEFAULT_EXACT[i],
		        DEFAULT_EXACT[i + 1] ? "," : "");
	fprintf(f, "  ],\n  \"exe_substr\": [\n");
	for (int i = 0; DEFAULT_SUBSTR[i]; i++)
		fprintf(f, "    \"%s\"%s\n", DEFAULT_SUBSTR[i],
		        DEFAULT_SUBSTR[i + 1] ? "," : "");
	fprintf(f, "  ]\n}\n");
	fclose(f);
}

typedef void (*json_str_array_fn)(const char *s, void *ctx);

static void json_load_str_array(const char *json, const char *key,
                                json_str_array_fn fn, void *ctx) {
	if (!json || !key || !fn)
		return;

	char search[64];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char *p = strstr(json, search);
	if (!p)
		return;
	p = strchr(p, '[');
	if (!p)
		return;
	p++;

	while (*p) {
		p = strchr(p, '"');
		if (!p)
			break;
		p++;
		char buf[256];
		size_t i = 0;
		while (*p && *p != '"' && i < sizeof(buf) - 1)
			buf[i++] = *p++;
		buf[i] = '\0';
		if (i > 0)
			fn(buf, ctx);
		p = strchr(p, ']');
		if (p && p > strstr(json, key)) {
			const char *close = strchr(p, ']');
			if (close && strchr(close + 1, ']') == NULL)
				break;
		}
		if (p && *(p + 1) == ']')
			break;
		if (!p)
			break;
		p++;
	}
}

typedef struct {
	char (*list)[GD_MAX_SKIP_LEN];
	int  *count;
	int   cap;
} skip_load_ctx;

static void skip_load_item(const char *s, void *vp) {
	auto *ctx = (skip_load_ctx *)vp;
	skip_add_to_list(ctx->list, ctx->count, ctx->cap, s);
}

static void skip_load_defaults(void) {
	s_exact_count  = 0;
	s_substr_count = 0;
	for (int i = 0; DEFAULT_EXACT[i]; i++)
		skip_add_to_list(s_exact, &s_exact_count, GD_MAX_SKIP_EXACT,
		                 DEFAULT_EXACT[i]);
	for (int i = 0; DEFAULT_SUBSTR[i]; i++)
		skip_add_to_list(s_substr, &s_substr_count, GD_MAX_SKIP_SUBSTR,
		                 DEFAULT_SUBSTR[i]);
}

void gd_skip_rules_load(void) {
	skip_load_defaults();

	char *path = obs_module_config_path("skip_rules.json");
	if (!path)
		return;

	char *buf = gd_file_read_alloc(path, 256 * 1024);
	if (!buf) {
		skip_write_defaults(path);
		buf = gd_file_read_alloc(path, 256 * 1024);
		if (!buf) {
			bfree(path);
			return;
		}
	}

	s_exact_count  = 0;
	s_substr_count = 0;
	skip_load_ctx exact_ctx = {s_exact, &s_exact_count, GD_MAX_SKIP_EXACT};
	skip_load_ctx substr_ctx = {s_substr, &s_substr_count, GD_MAX_SKIP_SUBSTR};
	json_load_str_array(buf, "exe_exact", skip_load_item, &exact_ctx);
	json_load_str_array(buf, "exe_substr", skip_load_item, &substr_ctx);
	if (s_exact_count == 0 && s_substr_count == 0)
		skip_load_defaults();

	bfree(buf);
	bfree(path);
}

static bool skip_substr_heuristic(const char *exe_lower) {
	char base[GD_MAX_PATH];
	gd_strlcpy(base, exe_lower, sizeof(base));
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';

	for (int i = 0; i < s_substr_count; i++)
		if (strstr(base, s_substr[i]))
			return true;

	size_t blen = strlen(base);
	const char *suf  = "launcher";
	size_t      slen = strlen(suf);
	if (blen >= slen && strcmp(base + blen - slen, suf) == 0)
		return true;

	return false;
}

bool gd_skip_exact(const char *exe_lower) {
	if (!exe_lower || !exe_lower[0])
		return true;

	for (int i = 0; i < s_exact_count; i++)
		if (strcmp(exe_lower, s_exact[i]) == 0)
			return true;
	return false;
}

bool gd_skip_heuristic(const char *exe_lower) {
	if (!exe_lower || !exe_lower[0])
		return true;
	return skip_substr_heuristic(exe_lower);
}
