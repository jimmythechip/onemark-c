#define _POSIX_C_SOURCE 200809L
/* onemark — main entry point
 *
 * Spatial canvas rendering, box navigation, input loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/om.h"
#include "config.h"

/* Convert pixel coords to terminal cells using config values */
static int px_to_col(int px) { return px / cfg_cell_w; }
static int px_to_row(int py) { return py / cfg_cell_h; }

/* --- viewport ------------------------------------------------------------ */
static int vp_row, vp_col; /* viewport scroll offset in cells */

/* --- app state ----------------------------------------------------------- */
static struct NotebookFile file;
static struct VimState vim;
static int focused_box = -1; /* index into file.boxes, -1 = none */
static int editing = 0;      /* 1 = editing box body (vim active) */
static int running = 1;
static int leader_pending = 0; /* waiting for key after leader */

/* cross-box state */
static struct Mark marks[26]; /* a-z */
static struct JumpHistory jumphist;

/* --- drawing ------------------------------------------------------------- */

static void draw_box_border(int r, int c, int w, int h, int focused, enum Tag tag)
{
	int attr = 0;
	int rows = plat_rows();
	int cols = plat_cols();

	if (focused) attr = ATTR_BOLD;
	switch (tag) {
	case TAG_IDEA:      attr |= ATTR_COLOR(COL_BLUE); break;
	case TAG_TODO:       attr |= ATTR_COLOR(COL_YELLOW); break;
	case TAG_REFERENCE:  attr |= ATTR_COLOR(COL_MAGENTA); break;
	default: break;
	}

	/* top border */
	if (r >= 0 && r < rows - 1) {
		if (c >= 0 && c < cols) { plat_move(r, c); plat_addch('+', attr); }
		for (int x = 1; x < w - 1; x++)
			if (c + x >= 0 && c + x < cols) { plat_move(r, c + x); plat_addch('-', attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(r, c + w - 1); plat_addch('+', attr); }
	}

	/* sides + content area */
	for (int y = 1; y < h - 1; y++) {
		int row = r + y;
		if (row < 0 || row >= rows - 1) continue;
		if (c >= 0 && c < cols) { plat_move(row, c); plat_addch('|', attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(row, c + w - 1); plat_addch('|', attr); }
	}

	/* bottom border */
	if (r + h - 1 >= 0 && r + h - 1 < rows - 1) {
		int row = r + h - 1;
		if (c >= 0 && c < cols) { plat_move(row, c); plat_addch('+', attr); }
		for (int x = 1; x < w - 1; x++)
			if (c + x >= 0 && c + x < cols) { plat_move(row, c + x); plat_addch('-', attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(row, c + w - 1); plat_addch('+', attr); }
	}
}

static void draw_box_content(struct Box *b, int r, int c, int w, int h, int is_focused)
{
	int inner_w = w - 2;
	int inner_h = h - 2;
	int rows = plat_rows();
	/* focused (not editing): reverse title to show it's selected */
	int title_attr = is_focused ? (ATTR_BOLD | ATTR_REVERSE) : ATTR_BOLD;

	if (inner_w <= 0 || inner_h <= 0) return;

	/* title line */
	if (r + 1 >= 0 && r + 1 < rows - 1) {
		int tlen = strlen(b->title);
		if (tlen > inner_w) tlen = inner_w;
		plat_move(r + 1, c + 1);
		plat_addstr(b->title, tlen, title_attr);
		for (int x = tlen; x < inner_w; x++)
			plat_addch(' ', title_attr);
	}

	/* body lines */
	char *content = gap_contents(&b->body);
	char *line = content;
	int line_idx = 0;

	for (int row = 0; row < inner_h - 1; row++) {
		int scr_row = r + 2 + row;
		if (scr_row < 0 || scr_row >= rows - 1) {
			/* advance to next line anyway */
			if (line && *line) {
				char *nl = strchr(line, '\n');
				line = nl ? nl + 1 : NULL;
			}
			continue;
		}

		plat_move(scr_row, c + 1);
		if (line && *line) {
			char *nl = strchr(line, '\n');
			int ll = nl ? (int)(nl - line) : (int)strlen(line);
			if (ll > inner_w) ll = inner_w;
			plat_addstr(line, ll, 0);
			/* pad rest */
			for (int x = ll; x < inner_w; x++)
				plat_addch(' ', 0);
			line = nl ? nl + 1 : NULL;
		} else {
			for (int x = 0; x < inner_w; x++)
				plat_addch(' ', 0);
		}
		line_idx++;
	}
	free(content);
	(void)is_focused;
}

static void draw_editing_box(struct Box *b, int r, int c, int w, int h)
{
	int inner_w = w - 2;
	int inner_h = h - 2;
	int rows = plat_rows();

	if (inner_w <= 0 || inner_h <= 0) return;

	/* title */
	if (r + 1 >= 0 && r + 1 < rows - 1) {
		int tlen = strlen(b->title);
		if (tlen > inner_w) tlen = inner_w;
		plat_move(r + 1, c + 1);
		plat_addstr(b->title, tlen, ATTR_BOLD | ATTR_REVERSE);
		for (int x = tlen; x < inner_w; x++)
			plat_addch(' ', ATTR_REVERSE);
	}

	/* body with cursor */
	int cursor_pos = b->body.gap_start;
	int char_idx = 0;
	int cursor_row = -1, cursor_col = -1;
	int len = gap_len(&b->body);

	for (int row = 0; row < inner_h - 1; row++) {
		int scr_row = r + 2 + row;
		if (scr_row < 0 || scr_row >= rows - 1) {
			/* skip line */
			while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n')
				char_idx++;
			if (char_idx < len) char_idx++; /* skip newline */
			continue;
		}
		plat_move(scr_row, c + 1);
		int col_in_line = 0;
		while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n' && col_in_line < inner_w) {
			if (char_idx == cursor_pos) {
				cursor_row = scr_row;
				cursor_col = c + 1 + col_in_line;
			}
			plat_addch(gap_char_at(&b->body, char_idx), 0);
			char_idx++;
			col_in_line++;
		}
		if (char_idx == cursor_pos) {
			cursor_row = scr_row;
			cursor_col = c + 1 + col_in_line;
		}
		/* pad */
		for (int x = col_in_line; x < inner_w; x++)
			plat_addch(' ', 0);
		/* skip rest of long line + newline */
		while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n')
			char_idx++;
		if (char_idx < len) char_idx++;
	}

	/* show cursor */
	if (cursor_row >= 0)
		plat_show_cursor(cursor_row, cursor_col);
}

static void draw_status(void)
{
	int cols = plat_cols();
	int rows = plat_rows();
	char status[256];
	const char *mode_str;

	switch (vim.mode) {
	case MODE_INSERT:     mode_str = "INSERT"; break;
	case MODE_VISUAL:     mode_str = "VISUAL"; break;
	case MODE_VLINE:      mode_str = "V-LINE"; break;
	case MODE_OP_PENDING: mode_str = "OP-PENDING"; break;
	case MODE_COMMAND:    mode_str = "COMMAND"; break;
	default:              mode_str = leader_pending ? "LEADER" : "NORMAL"; break;
	}

	if (vim.search_active) {
		snprintf(status, sizeof status, "/%.250s", vim.search_buf);
	} else if (vim.mode == MODE_COMMAND) {
		snprintf(status, sizeof status, ":%.250s", vim.cmd_buf);
	} else {
		const char *fname = file.name ? file.name : "(no file)";
		int box_num = focused_box >= 0 ? focused_box + 1 : 0;
		snprintf(status, sizeof status, " %s | %s | box %d/%d %s",
			mode_str, fname, box_num, file.box_count,
			file.dirty ? "[+]" : "");
	}

	plat_move(rows - 1, 0);
	int slen = strlen(status);
	plat_addstr(status, slen, ATTR_REVERSE);
	for (int x = slen; x < cols; x++)
		plat_addch(' ', ATTR_REVERSE);
}

static void redraw(void)
{
	plat_clear();
	plat_hide_cursor(); /* hide cursor during draw */

	for (int i = 0; i < file.box_count; i++) {
		struct Box *b = &file.boxes[i];
		int r = px_to_row(b->y) - vp_row;
		int c = px_to_col(b->x) - vp_col;
		int w = px_to_col(b->w);
		int h = px_to_row(b->h);
		if (w < 6) w = 6;
		if (h < 4) h = 4;

		int is_focused = (i == focused_box);
		draw_box_border(r, c, w, h, is_focused, b->tag);

		if (is_focused && editing)
			draw_editing_box(b, r, c, w, h);
		else
			draw_box_content(b, r, c, w, h, is_focused);
	}

	if (file.box_count == 0) {
		plat_move(1, 2);
		plat_addstr("No boxes yet.  Type  :newbox  then Enter.", -1, ATTR_DIM);
		plat_move(2, 2);
		plat_addstr("Press q to quit.", -1, ATTR_DIM);
	}

	draw_status();
	plat_refresh();
}

/* --- box hit testing ----------------------------------------------------- */

static int box_at_cell(int row, int col)
{
	/* check in reverse order (later boxes draw on top) */
	for (int i = file.box_count - 1; i >= 0; i--) {
		struct Box *b = &file.boxes[i];
		int r = px_to_row(b->y) - vp_row;
		int c = px_to_col(b->x) - vp_col;
		int w = px_to_col(b->w);
		int h = px_to_row(b->h);
		if (w < 6) w = 6;
		if (h < 4) h = 4;

		if (row >= r && row < r + h && col >= c && col < c + w)
			return i;
	}
	return -1;
}

/* --- jump history -------------------------------------------------------- */

static void jump_push(int box, int pos)
{
	if (jumphist.count > 0) {
		int prev = jumphist.cursor;
		if (prev >= 0 && prev < jumphist.count &&
		    jumphist.ring[prev].box == box)
			return; /* don't push duplicate */
	}
	/* truncate forward */
	jumphist.count = jumphist.cursor + 1;
	if (jumphist.count >= JUMP_MAX) {
		memmove(&jumphist.ring[0], &jumphist.ring[1],
			(JUMP_MAX - 1) * sizeof(jumphist.ring[0]));
		jumphist.count = JUMP_MAX - 1;
	}
	jumphist.ring[jumphist.count].box = box;
	jumphist.ring[jumphist.count].pos = pos;
	jumphist.cursor = jumphist.count;
	jumphist.count++;
}

/* --- tag cycling --------------------------------------------------------- */

static void cycle_tag(struct Box *b)
{
	switch (b->tag) {
	case TAG_NONE:      b->tag = TAG_IDEA; break;
	case TAG_IDEA:      b->tag = TAG_TODO; break;
	case TAG_TODO:       b->tag = TAG_REFERENCE; break;
	case TAG_REFERENCE:  b->tag = TAG_NONE; break;
	}
}

/* --- viewport auto-scroll ------------------------------------------------ */

static void auto_scroll_to_box(int idx)
{
	if (idx < 0 || idx >= file.box_count) return;
	struct Box *b = &file.boxes[idx];
	int r = px_to_row(b->y);
	int c = px_to_col(b->x);
	int bh = px_to_row(b->h);
	int bw = px_to_col(b->w);
	if (bh < 4) bh = 4;
	if (bw < 6) bw = 6;
	int rows = plat_rows() - 1;
	int cols = plat_cols();

	if (r - vp_row < 0) vp_row = r;
	else if (r + bh - vp_row > rows) vp_row = r + bh - rows;
	if (c - vp_col < 0) vp_col = c;
	else if (c + bw - vp_col > cols) vp_col = c + bw - cols;
}

/* --- focus helper -------------------------------------------------------- */

static void focus_box(int idx)
{
	if (idx == focused_box) return;
	if (focused_box >= 0)
		jump_push(focused_box, file.boxes[focused_box].body.gap_start);
	focused_box = idx;
	if (idx >= 0) {
		jump_push(idx, file.boxes[idx].body.gap_start);
		auto_scroll_to_box(idx);
	}
}

/* --- navigation ---------------------------------------------------------- */

/* Simple reading order: sort by y then x */
static int reading_order_next(int cur)
{
	if (file.box_count == 0) return -1;
	if (cur < 0) return 0;
	return (cur + 1) % file.box_count;
}

static int reading_order_prev(int cur)
{
	if (file.box_count == 0) return -1;
	if (cur < 0) return file.box_count - 1;
	return (cur - 1 + file.box_count) % file.box_count;
}

/* Spatial navigation: find nearest box in direction */
static int spatial_nav(int cur, int dir_key)
{
	if (cur < 0 || file.box_count < 2) return cur;

	struct Box *from = &file.boxes[cur];
	int fx = from->x + from->w / 2;
	int fy = from->y + from->h / 2;
	int best = -1;
	int best_dist = 999999;

	for (int i = 0; i < file.box_count; i++) {
		if (i == cur) continue;
		struct Box *to = &file.boxes[i];
		int tx = to->x + to->w / 2;
		int ty = to->y + to->h / 2;
		int dx = tx - fx;
		int dy = ty - fy;

		/* check direction */
		int ok = 0;
		switch (dir_key) {
		case 'h': ok = (dx < 0); break;
		case 'l': ok = (dx > 0); break;
		case 'k': ok = (dy < 0); break;
		case 'j': ok = (dy > 0); break;
		}
		if (!ok) continue;

		int dist = dx * dx + dy * dy;
		if (dist < best_dist) {
			best_dist = dist;
			best = i;
		}
	}
	return best >= 0 ? best : cur;
}

/* --- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int key;
	struct MouseEvent mouse;

	/* load runtime config (overrides config.h defaults) */
	conf_ensure_dir();
	conf_load();

	{
		const char *path = argc >= 2 ? argv[1] : "notes.md";
		if (file_parse(&file, path) != 0)
			file_init_empty(&file, path);
	}

	vim_init(&vim);
	for (int i = 0; i < 26; i++) marks[i].box = -1;
	memset(&jumphist, 0, sizeof jumphist);
	plat_init();

	if (file.box_count > 0)
		focused_box = 0;

	redraw();

	while (running) {
		int result = plat_getinput(&key, &mouse, 50);

		if (result == INPUT_RESIZE) {
			redraw();
			continue;
		}
		if (result == INPUT_NONE)
			continue;

		if (result == INPUT_MOUSE) {
			if (mouse.pressed && mouse.button == 0) {
				int hit = box_at_cell(mouse.row, mouse.col);
				if (hit >= 0) {
					if (hit == focused_box && editing) {
						/* click inside editing box — TODO: place cursor */
					} else {
						focused_box = hit;
						editing = 0;
						plat_hide_cursor();
					}
				} else {
					/* click on empty canvas — create a new box here */
					if (file.box_count < MAX_BOXES) {
						struct Box *nb = &file.boxes[file.box_count];
						int px = (mouse.col + vp_col) * cfg_cell_w;
						int py = (mouse.row + vp_row) * cfg_cell_h;
						box_init_new(nb, px, py, cfg_box_w, cfg_box_h);
						file.box_count++;
						focused_box = file.box_count - 1;
						editing = 1;
						vim.mode = MODE_INSERT;
						file.dirty = 1;
					}
				}
			}
			/* always redraw on any mouse event (fixes drag traces) */
			redraw();
			continue;
		}

		/* INPUT_KEY */

		/* --- command mode (works with or without focused box) --- */
		if (vim.mode == MODE_COMMAND) {
			vim_keypress(&vim,
				focused_box >= 0 ? &file.boxes[focused_box].body : NULL,
				focused_box >= 0 ? &file.boxes[focused_box].undo : NULL,
				key);

			switch (vim.result) {
			case VIM_RESULT_SAVE:
				file_save(&file);
				file.dirty = 0;
				break;
			case VIM_RESULT_QUIT:
				editing = 0;
				plat_hide_cursor();
				break;
			case VIM_RESULT_SAVEQUIT:
				file_save(&file);
				file.dirty = 0;
				editing = 0;
				plat_hide_cursor();
				break;
			case VIM_RESULT_NEWBOX:
				if (file.box_count < MAX_BOXES) {
					struct Box *nb = &file.boxes[file.box_count];
					int nx = 40, ny = 40;
					if (focused_box >= 0) {
						nx = file.boxes[focused_box].x + file.boxes[focused_box].w + 20;
						ny = file.boxes[focused_box].y;
					} else if (file.box_count > 0) {
						nx = file.boxes[file.box_count - 1].x;
						ny = file.boxes[file.box_count - 1].y + file.boxes[file.box_count - 1].h + 20;
					}
					box_init_new(nb, nx, ny, cfg_box_w, cfg_box_h);
					file.box_count++;
					focused_box = file.box_count - 1;
					editing = 1;
					vim.mode = MODE_INSERT;
					file.dirty = 1;
				}
				break;
			default:
				break;
			}

			if (vim.mode == MODE_NORMAL)
				editing = (focused_box >= 0) ? editing : 0;

			redraw();
			continue;
		}

		/* --- leader key dispatch (in editing normal mode) --- */
		if (leader_pending && editing && focused_box >= 0) {
			struct Box *b = &file.boxes[focused_box];
			leader_pending = 0;
			switch (key) {
			case 'b': /* bold */
				edit_toggle_wrap(&b->body, &b->undo, b->body.gap_start, b->body.gap_start, "**");
				file.dirty = 1;
				break;
			case 'i': /* italic */
				edit_toggle_wrap(&b->body, &b->undo, b->body.gap_start, b->body.gap_start, "*");
				file.dirty = 1;
				break;
			case '1': /* checkbox */
				edit_checkbox_rotate(&b->body, &b->undo, b->body.gap_start);
				file.dirty = 1;
				break;
			case 't': /* cycle tag */
				cycle_tag(b);
				file.dirty = 1;
				break;
			case 'w': /* save */
				file_save(&file);
				file.dirty = 0;
				break;
			case 'n': /* new box */
				if (file.box_count < MAX_BOXES) {
					struct Box *nb = &file.boxes[file.box_count];
					box_init_new(nb, b->x + b->w + 20, b->y, cfg_box_w, cfg_box_h);
					file.box_count++;
					focus_box(file.box_count - 1);
					editing = 1;
					vim.mode = MODE_INSERT;
					file.dirty = 1;
				}
				break;
			case 'd': /* duplicate box */
				if (file.box_count < MAX_BOXES) {
					struct Box *nb = &file.boxes[file.box_count];
					box_init_new(nb, b->x + 20, b->y + 20, cfg_box_w, cfg_box_h);
					/* copy body */
					char *content = gap_contents(&b->body);
					gap_free(&nb->body);
					gap_init(&nb->body, content, strlen(content));
					free(content);
					/* copy title */
					free(nb->title);
					nb->title = strdup(b->title);
					nb->tag = b->tag;
					file.box_count++;
					focus_box(file.box_count - 1);
					file.dirty = 1;
				}
				break;
			case 'D': /* delete box */
				if (file.box_count > 0) {
					int del = focused_box;
					gap_free(&b->body);
					undo_free(&b->undo);
					free(b->title);
					memmove(&file.boxes[del], &file.boxes[del + 1],
						(file.box_count - del - 1) * sizeof(struct Box));
					file.box_count--;
					editing = 0;
					focused_box = del < file.box_count ? del : file.box_count - 1;
					file.dirty = 1;
				}
				break;
			}
			redraw();
			continue;
		}

		/* --- box editing (vim normal/insert inside a box) --- */
		if (editing && focused_box >= 0) {
			struct Box *b = &file.boxes[focused_box];

			/* leader key (configurable, default space) */
			if (vim.mode == MODE_NORMAL && key == cfg_leader) {
				leader_pending = 1;
				redraw();
				continue;
			}

			/* heading/list nav in normal mode */
			if (vim.mode == MODE_NORMAL) {
				/* ]] and [[ need two-key handling... simplified: use ]h/[h */
				if (key == KEY_CTRL('o')) {
					/* jump back */
					if (jumphist.cursor > 0) {
						jumphist.cursor--;
						int bi = jumphist.ring[jumphist.cursor].box;
						if (bi >= 0 && bi < file.box_count) {
							focused_box = bi;
							gap_move(&file.boxes[bi].body,
								jumphist.ring[jumphist.cursor].pos);
							auto_scroll_to_box(bi);
						}
					}
					redraw();
					continue;
				}
				if (key == KEY_CTRL('i')) {
					/* jump forward */
					if (jumphist.cursor < jumphist.count - 1) {
						jumphist.cursor++;
						int bi = jumphist.ring[jumphist.cursor].box;
						if (bi >= 0 && bi < file.box_count) {
							focused_box = bi;
							gap_move(&file.boxes[bi].body,
								jumphist.ring[jumphist.cursor].pos);
							auto_scroll_to_box(bi);
						}
					}
					redraw();
					continue;
				}
			}

			/* ]]/[[, ]b/[b, ]l/[l — bracket nav (two-key, handled here) */
			if (vim.mode == MODE_NORMAL && (key == ']' || key == '[')) {
				int dir = key;
				int key2;
				struct MouseEvent dummy;
				if (plat_getinput(&key2, &dummy, 500) == INPUT_KEY) {
					if (key2 == ']' || key2 == '[') {
						/* ]] or [[ — heading nav */
						int np = (dir == ']')
							? edit_next_heading(&b->body, b->body.gap_start)
							: edit_prev_heading(&b->body, b->body.gap_start);
						gap_move(&b->body, np);
					} else if (key2 == 'b') {
						/* ]b/[b — box nav */
						int next = (dir == ']')
							? reading_order_next(focused_box)
							: reading_order_prev(focused_box);
						if (next >= 0) {
							editing = 0;
							plat_hide_cursor();
							focus_box(next);
						}
					} else if (key2 == 'l') {
						/* ]l/[l — list nav */
						int np = (dir == ']')
							? edit_next_list_item(&b->body, b->body.gap_start)
							: edit_prev_list_item(&b->body, b->body.gap_start);
						gap_move(&b->body, np);
					} else if (key2 == 'h') {
						/* ]h/[h — heading nav (alias) */
						int np = (dir == ']')
							? edit_next_heading(&b->body, b->body.gap_start)
							: edit_prev_heading(&b->body, b->body.gap_start);
						gap_move(&b->body, np);
					}
				}
				redraw();
				continue;
			}

			vim_keypress(&vim, &b->body, &b->undo, key);

			/* handle results */
			if (vim.result == VIM_RESULT_SET_MARK) {
				int idx = vim.mark_letter - 'a';
				marks[idx].box = focused_box;
				marks[idx].pos = b->body.gap_start;
			} else if (vim.result == VIM_RESULT_JUMP_MARK) {
				int idx = vim.mark_letter - 'a';
				if (marks[idx].box >= 0 && marks[idx].box < file.box_count) {
					focus_box(marks[idx].box);
					gap_move(&file.boxes[marks[idx].box].body, marks[idx].pos);
				}
			} else if (vim.result == VIM_RESULT_ZZ) {
				file_save(&file);
				file.dirty = 0;
				editing = 0;
				plat_hide_cursor();
			} else if (vim.result == VIM_RESULT_ZQ) {
				editing = 0;
				plat_hide_cursor();
			} else if (vim.result == VIM_RESULT_SET_FIELD) {
				/* :set key value — parse from cmd_buf */
				char *arg = vim.cmd_buf + 4;
				char *sp = strchr(arg, ' ');
				if (sp && b->custom_count < MAX_CUSTOM_FIELDS) {
					*sp = '\0';
					b->custom[b->custom_count].key = strdup(arg);
					b->custom[b->custom_count].value = strdup(sp + 1);
					b->custom_count++;
					file.dirty = 1;
				}
			} else if (vim.result == VIM_RESULT_SET_TAG) {
				char *arg = vim.cmd_buf + 4;
				if (strcmp(arg, "idea") == 0) b->tag = TAG_IDEA;
				else if (strcmp(arg, "todo") == 0) b->tag = TAG_TODO;
				else if (strcmp(arg, "reference") == 0) b->tag = TAG_REFERENCE;
				else b->tag = TAG_NONE;
				file.dirty = 1;
			}

			if (vim.result == VIM_RESULT_SAVE) {
				file_save(&file);
				file.dirty = 0;
			} else if (vim.result == VIM_RESULT_QUIT) {
				editing = 0;
				plat_hide_cursor();
			} else if (vim.result == VIM_RESULT_SAVEQUIT) {
				file_save(&file);
				file.dirty = 0;
				editing = 0;
				plat_hide_cursor();
			} else if (vim.result == VIM_RESULT_DELBOX) {
				if (file.box_count > 0) {
					int del = focused_box;
					gap_free(&b->body);
					undo_free(&b->undo);
					free(b->title);
					memmove(&file.boxes[del], &file.boxes[del + 1],
						(file.box_count - del - 1) * sizeof(struct Box));
					file.box_count--;
					editing = 0;
					focused_box = del < file.box_count ? del : file.box_count - 1;
					file.dirty = 1;
				}
			} else if (vim.result == VIM_RESULT_DUPBOX) {
				if (file.box_count < MAX_BOXES) {
					struct Box *nb = &file.boxes[file.box_count];
					box_init_new(nb, b->x + 20, b->y + 20, cfg_box_w, cfg_box_h);
					char *content = gap_contents(&b->body);
					gap_free(&nb->body);
					gap_init(&nb->body, content, strlen(content));
					free(content);
					free(nb->title);
					nb->title = strdup(b->title);
					nb->tag = b->tag;
					file.box_count++;
					focus_box(file.box_count - 1);
					file.dirty = 1;
				}
			}

			if (vim.mode == MODE_NORMAL && key == KEY_ESC) {
				editing = 0;
				plat_hide_cursor();
			}

			if (vim.result == VIM_RESULT_NONE && vim.mode == MODE_INSERT)
				file.dirty = 1;

			redraw();
			continue;
		}

		/* --- canvas-level keys (not editing a box) --- */
		switch (key) {
		case 'q':
			if (file.dirty)
				file_save(&file);
			running = 0;
			break;

		case '\r':
		case '\n':
		case 'i':
			if (focused_box >= 0) {
				editing = 1;
				vim.mode = (key == 'i') ? MODE_INSERT : MODE_NORMAL;
			}
			break;

		case KEY_ESC:
			focused_box = -1;
			break;

		case 'j':
		case KEY_DOWN:
			focus_box(reading_order_next(focused_box));
			break;
		case 'k':
		case KEY_UP:
			focus_box(reading_order_prev(focused_box));
			break;

		case KEY_CTRL('h'):
			focus_box(spatial_nav(focused_box, 'h'));
			break;
		case KEY_CTRL('n'):
			focus_box(spatial_nav(focused_box, 'j'));
			break;
		case KEY_CTRL('k'):
			focus_box(spatial_nav(focused_box, 'k'));
			break;
		case KEY_CTRL('l'):
			focus_box(spatial_nav(focused_box, 'l'));
			break;

		case ':':
			vim.mode = MODE_COMMAND;
			vim.cmd_len = 0;
			vim.cmd_buf[0] = '\0';
			break;
		}

		redraw();
	}

	plat_deinit();
	return 0;
}
