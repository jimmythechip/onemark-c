#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* onemark — runtime config parser
 *
 * Reads ~/.config/onemark/config (or %APPDATA%\onemark\config on Windows).
 * Overrides compile-time defaults from config.h.
 * Unknown keys are silently ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(p,m) _mkdir(p)
#define access(p,m) _access(p,0)
#define F_OK 0
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include "om.h"

/* config.h is included by the translation unit that calls conf_load,
 * which provides the cfg_* variables. We declare them extern here. */
extern char cfg_leader;
extern int  cfg_cell_w;
extern int  cfg_cell_h;
extern int  cfg_box_w;
extern int  cfg_box_h;
extern int  cfg_undo_max;
extern int  cfg_save_debounce_ms;
extern int  cfg_max_box_cols;

static void trim(char *s)
{
	/* trim trailing whitespace */
	int len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	/* trim leading whitespace (shift in place) */
	char *start = s;
	while (*start && isspace((unsigned char)*start))
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1);
}

static char *config_path(void)
{
#ifdef _WIN32
	const char *home = getenv("APPDATA");
#else
	const char *home = getenv("HOME");
#endif
	if (!home) return NULL;
	int len = (int)strlen(home) + 40;
	char *path = malloc(len);
#ifdef _WIN32
	snprintf(path, len, "%s\\onemark\\config", home);
#else
	snprintf(path, len, "%s/.config/onemark/config", home);
#endif
	return path;
}

void conf_load(void)
{
	char *path = config_path();
	if (!path) return;

	FILE *fp = fopen(path, "r");
	free(path);
	if (!fp) return;

	char line[512];
	while (fgets(line, sizeof line, fp)) {
		/* skip comments and blank lines */
		trim(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;

		/* find = separator */
		char *eq = strchr(line, '=');
		if (!eq) continue;

		*eq = '\0';
		char *key = line;
		char *val = eq + 1;
		trim(key);
		trim(val);

		if (strcmp(key, "leader") == 0) {
			if (val[0]) cfg_leader = val[0];
		} else if (strcmp(key, "cell_w") == 0) {
			int v = atoi(val);
			if (v > 0) cfg_cell_w = v;
		} else if (strcmp(key, "cell_h") == 0) {
			int v = atoi(val);
			if (v > 0) cfg_cell_h = v;
		} else if (strcmp(key, "box_w") == 0) {
			int v = atoi(val);
			if (v > 0) cfg_box_w = v;
		} else if (strcmp(key, "box_h") == 0) {
			int v = atoi(val);
			if (v > 0) cfg_box_h = v;
		} else if (strcmp(key, "undo_max") == 0) {
			int v = atoi(val);
			if (v > 0 && v <= UNDO_MAX) cfg_undo_max = v;
		} else if (strcmp(key, "save_debounce_ms") == 0) {
			int v = atoi(val);
			if (v >= 0) cfg_save_debounce_ms = v;
		} else if (strcmp(key, "max_box_cols") == 0) {
			int v = atoi(val);
			if (v >= 10 && v <= 300) cfg_max_box_cols = v;
		}
		/* unknown keys silently ignored */
	}

	fclose(fp);
}

void conf_ensure_dir(void)
{
#ifdef _WIN32
	const char *home = getenv("APPDATA");
#else
	const char *home = getenv("HOME");
#endif
	if (!home) return;
	char dir[512];
#ifdef _WIN32
	snprintf(dir, sizeof dir, "%s\\onemark", home);
	mkdir(dir, 0);
#else
	snprintf(dir, sizeof dir, "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof dir, "%s/.config/onemark", home);
	mkdir(dir, 0755);
#endif

	/* write default config if it doesn't exist */
	char path[512];
#ifdef _WIN32
	snprintf(path, sizeof path, "%s\\onemark\\config", home);
#else
	snprintf(path, sizeof path, "%s/.config/onemark/config", home);
#endif
	if (access(path, F_OK) != 0) {
		FILE *fp = fopen(path, "w");
		if (fp) {
			fprintf(fp, "# onemark configuration\n");
			fprintf(fp, "# edit and restart (or recompile config.h for compile-time)\n\n");
			fprintf(fp, "# leader = <space>\n");
			fprintf(fp, "# cell_w = 8\n");
			fprintf(fp, "# cell_h = 16\n");
			fprintf(fp, "# box_w = 320\n");
			fprintf(fp, "# box_h = 180\n");
			fprintf(fp, "# max_box_cols = 80\n");
			fprintf(fp, "# undo_max = 100\n");
			fprintf(fp, "# save_debounce_ms = 500\n");
			fclose(fp);
		}
	}
}
