/* onemark compile-time configuration
 *
 * Copy this file to config.h and edit to taste. Recompile after changes.
 * Runtime config file (~/.config/onemark/config) overrides these at startup.
 */

/* vim leader key (single character) */
char cfg_leader = ' ';

/* pixels per character cell for coordinate scaling from Electron format */
int cfg_cell_w = 8;
int cfg_cell_h = 16;

/* default box size in pixels (for new boxes) */
int cfg_box_w = 320;
int cfg_box_h = 120;

/* maximum undo history entries per box */
int cfg_undo_max = 100;

/* auto-save debounce in milliseconds (0 = disabled) */
int cfg_save_debounce_ms = 500;

/* maximum box width in terminal columns (auto-grow stops here) */
int cfg_max_box_cols = 80;

/* tag definitions: name + ANSI color (0-7) */
const struct { const char *name; int color; } cfg_tags[] = {
	{ "none",      7 },  /* white */
	{ "idea",      4 },  /* blue */
	{ "todo",      3 },  /* yellow */
	{ "reference", 5 },  /* magenta */
};

/*
 * Keybindings for leader-key dispatch (Space + <key>).
 * action values are single chars dispatched in the leader handler.
 */
const struct { int key; int action; } cfg_leader_bindings[] = {
	{ 'b', 'b' },  /* bold */
	{ 'i', 'i' },  /* italic */
	{ '1', '1' },  /* checkbox rotate */
	{ 't', 't' },  /* cycle tag */
	{ 'w', 'w' },  /* save */
	{ 'n', 'n' },  /* new box */
	{ 'd', 'd' },  /* duplicate box */
	{ 'D', 'D' },  /* delete box */
	{ 'p', 'p' },  /* peek (reserved) */
	{ 's', 's' },  /* easymotion (reserved) */
};
#define CFG_LEADER_BIND_COUNT \
	(int)(sizeof(cfg_leader_bindings) / sizeof(cfg_leader_bindings[0]))
