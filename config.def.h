/* onemark compile-time configuration
 *
 * Copy this file to config.h and edit to taste. Recompile after changes.
 * Runtime config (~/.config/onemark/config) overrides these defaults.
 */

/* vim leader key (single character) */
static const char *default_leader = " ";

/* tab stop width */
static const int default_tab_stop = 4;

/* pixels per character cell for coordinate scaling from Electron format */
static const int default_cell_w = 8;
static const int default_cell_h = 16;

/* ANSI color indices for tags (0-7, add 8 for bold) */
static const struct { const char *name; int color; } default_tags[] = {
	{ "none",      7 },  /* white */
	{ "idea",      4 },  /* blue */
	{ "todo",      3 },  /* yellow */
	{ "reference", 5 },  /* magenta */
};

/* maximum undo history depth */
static const int default_history_depth = 50;

/* auto-save debounce in milliseconds */
static const int default_save_debounce_ms = 500;
