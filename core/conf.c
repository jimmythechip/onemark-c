#define _POSIX_C_SOURCE 200809L
/* onemark — runtime config parser
 *
 * Reads ~/.config/onemark/config (simple key = value format).
 * Overrides compile-time defaults from config.h.
 * Unknown keys are silently ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
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
	const char *home = getenv("HOME");
	if (!home) return NULL;
	/* ~/.config/onemark/config */
	int len = strlen(home) + 32;
	char *path = malloc(len);
	snprintf(path, len, "%s/.config/onemark/config", home);
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
		}
		/* unknown keys silently ignored */
	}

	fclose(fp);
}

void conf_ensure_dir(void)
{
	const char *home = getenv("HOME");
	if (!home) return;
	char dir[512];
	snprintf(dir, sizeof dir, "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof dir, "%s/.config/onemark", home);
	mkdir(dir, 0755);
}
