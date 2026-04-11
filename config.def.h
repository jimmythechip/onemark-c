/* onemark compile-time configuration
 *
 * Copy this file to config.h and edit to taste. Recompile after changes.
 * Runtime config file (~/.config/onemark/config) overrides these at startup.
 *
 * Include this from exactly ONE .c file with #define CONFIG_IMPL before
 * including, to place the variable definitions. All other files get externs.
 */

#ifdef CONFIG_IMPL
#define CFG_DEF
#else
#define CFG_DEF extern
#endif

/* vim leader key (single character) */
CFG_DEF char cfg_leader
#ifdef CONFIG_IMPL
	= ' '
#endif
;

/* pixels per character cell for coordinate scaling from Electron format */
CFG_DEF int cfg_cell_w
#ifdef CONFIG_IMPL
	= 8
#endif
;
CFG_DEF int cfg_cell_h
#ifdef CONFIG_IMPL
	= 16
#endif
;

/* default box size in pixels (for new boxes) */
CFG_DEF int cfg_box_w
#ifdef CONFIG_IMPL
	= 320
#endif
;
CFG_DEF int cfg_box_h
#ifdef CONFIG_IMPL
	= 120
#endif
;

/* maximum undo history entries per box */
CFG_DEF int cfg_undo_max
#ifdef CONFIG_IMPL
	= 100
#endif
;

/* auto-save debounce in milliseconds (0 = disabled) */
CFG_DEF int cfg_save_debounce_ms
#ifdef CONFIG_IMPL
	= 500
#endif
;

/* maximum box width in terminal columns (auto-grow stops here) */
CFG_DEF int cfg_max_box_cols
#ifdef CONFIG_IMPL
	= 80
#endif
;

/* tag definitions: name + ANSI color (0-7) */
CFG_DEF const struct { const char *name; int color; } cfg_tags[]
#ifdef CONFIG_IMPL
	= {
		{ "none",      7 },  /* white */
		{ "idea",      4 },  /* blue */
		{ "todo",      3 },  /* yellow */
		{ "reference", 5 },  /* magenta */
	}
#endif
;

/*
 * Keybindings for leader-key dispatch (Space + <key>).
 */
CFG_DEF const struct { int key; int action; } cfg_leader_bindings[]
#ifdef CONFIG_IMPL
	= {
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
	}
#endif
;
#define CFG_LEADER_BIND_COUNT 10

#undef CFG_DEF
