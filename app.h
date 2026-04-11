/* onemark — application controller
 *
 * Owns all application state. Platform backends (terminal, Win32, X11)
 * call app_* functions and read the App struct to render.
 * The App knows nothing about rendering or platform APIs.
 */
#ifndef APP_H
#define APP_H

#include "core/om.h"
#include "config.h"

/* --- hit zones (what's under the mouse) --------------------------------- */
enum HitZone {
	HIT_NONE,       /* empty canvas */
	HIT_BODY,       /* box body (text area) */
	HIT_BORDER,     /* box border (drag handle) */
	HIT_RESIZE,     /* right edge (resize grip) */
	HIT_TITLE,      /* title bar area */
	HIT_TAG         /* tag swatch */
};

/* --- application state --------------------------------------------------- */
struct App {
	struct NotebookFile file;
	struct VimState vim;

	int focused_box;     /* index or -1 */
	int hovered_box;     /* index or -1 (mouse hover, no click) */
	int editing;         /* 1 = editing box body */
	int running;         /* 0 = quit */
	int leader_pending;

	/* mouse drag */
	int dragging;        /* 0=none, 1=move, 2=resize */
	int drag_box;
	int drag_ox, drag_oy; /* pixel offset from box origin at drag start */

	/* title editing */
	int editing_title;
	char title_buf[256];
	int title_len;
	int title_cursor;

	/* cross-box state */
	struct Mark marks[26];
	struct JumpHistory jumphist;

	/* viewport offset in file-pixel coords */
	int vp_x, vp_y;

	/* view dimensions in file-pixel coords (set by view) */
	int view_w, view_h;

	/* bracket nav pending (two-key: ] then b/h/l) */
	int bracket_pending;  /* '[' or ']' or 0 */
};

/* --- lifecycle ----------------------------------------------------------- */
void app_init(struct App *app, const char *path);
void app_destroy(struct App *app);

/* --- input events (all coordinates in file-pixel space) ------------------ */
void app_key(struct App *app, int key);
void app_mouse_down(struct App *app, int px, int py, int button);
void app_mouse_up(struct App *app, int px, int py, int button);
void app_mouse_move(struct App *app, int px, int py);

/* --- queries ------------------------------------------------------------- */

/* Hit test at file-pixel coordinate. Sets *box_idx to box index or -1. */
enum HitZone app_hit_test(struct App *app, int px, int py, int *box_idx);

/* Box navigation */
int app_next_box(struct App *app);
int app_prev_box(struct App *app);
int app_spatial_nav(struct App *app, int dir_key);

/* Auto-scroll viewport to keep box visible.
 * Call after changing view_w/view_h. */
void app_scroll_to_box(struct App *app, int idx);

/* Free a box's contents (body, undo, title, custom fields) */
void app_box_free(struct Box *b);

#endif /* APP_H */
