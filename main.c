/* onemark — terminal frontend
 *
 * Renders the app state via termbox2 (plat_* API).
 * Converts between terminal cells and file-pixel coordinates.
 * All logic is in app.c; this file only draws and gathers input.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

/* Convert file-pixel coords ↔ terminal cells */
static int px_to_col(int px) { return px / cfg_cell_w; }
static int px_to_row(int py) { return py / cfg_cell_h; }
static int col_to_px(int c)  { return c * cfg_cell_w; }
static int row_to_px(int r)  { return r * cfg_cell_h; }

/* Unicode box-drawing characters */
#define BOX_H   0x2500
#define BOX_V   0x2502
#define BOX_TL  0x250C
#define BOX_TR  0x2510
#define BOX_BL  0x2514
#define BOX_BR  0x2518

static struct App app;

/* --- drawing ------------------------------------------------------------- */

static void draw_box_border(int r, int c, int w, int h, int focused, int hovered, enum Tag tag)
{
	int attr = 0;
	int rows = plat_rows(), cols = plat_cols();

	if (focused) attr = ATTR_BOLD;
	else if (hovered) attr = ATTR_DIM; /* subtle hover hint */
	switch (tag) {
	case TAG_IDEA:      attr |= ATTR_COLOR(COL_BLUE); break;
	case TAG_TODO:      attr |= ATTR_COLOR(COL_YELLOW); break;
	case TAG_REFERENCE: attr |= ATTR_COLOR(COL_MAGENTA); break;
	default: break;
	}

	if (r >= 0 && r < rows - 1) {
		if (c >= 0 && c < cols) { plat_move(r, c); plat_adduc(BOX_TL, attr); }
		for (int x = 1; x < w - 1; x++)
			if (c + x >= 0 && c + x < cols) { plat_move(r, c + x); plat_adduc(BOX_H, attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(r, c + w - 1); plat_adduc(BOX_TR, attr); }
	}
	for (int y = 1; y < h - 1; y++) {
		int row = r + y;
		if (row < 0 || row >= rows - 1) continue;
		if (c >= 0 && c < cols) { plat_move(row, c); plat_adduc(BOX_V, attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(row, c + w - 1); plat_adduc(BOX_V, attr); }
	}
	if (r + h - 1 >= 0 && r + h - 1 < rows - 1) {
		int row = r + h - 1;
		if (c >= 0 && c < cols) { plat_move(row, c); plat_adduc(BOX_BL, attr); }
		for (int x = 1; x < w - 1; x++)
			if (c + x >= 0 && c + x < cols) { plat_move(row, c + x); plat_adduc(BOX_H, attr); }
		if (c + w - 1 >= 0 && c + w - 1 < cols) { plat_move(row, c + w - 1); plat_adduc(BOX_BR, attr); }
	}
}

static void draw_box_content(struct Box *b, int r, int c, int w, int h, int focused)
{
	int inner_w = w - 2, inner_h = h - 2;
	int rows = plat_rows();
	int title_attr = focused ? (ATTR_BOLD | ATTR_REVERSE) : ATTR_BOLD;

	if (inner_w <= 0 || inner_h <= 0) return;

	if (r + 1 >= 0 && r + 1 < rows - 1) {
		int tlen = (int)strlen(b->title);
		if (tlen > inner_w) tlen = inner_w;
		plat_move(r + 1, c + 1);
		plat_addstr(b->title, tlen, title_attr);
		for (int x = tlen; x < inner_w; x++)
			plat_addch(' ', title_attr);
	}

	char *content = gap_contents(&b->body);
	char *line = content;
	for (int row = 0; row < inner_h - 1; row++) {
		int scr_row = r + 2 + row;
		if (scr_row < 0 || scr_row >= rows - 1) {
			if (line && *line) { char *nl = strchr(line, '\n'); line = nl ? nl + 1 : NULL; }
			continue;
		}
		plat_move(scr_row, c + 1);
		if (line && *line) {
			char *nl = strchr(line, '\n');
			int ll = nl ? (int)(nl - line) : (int)strlen(line);
			if (ll > inner_w) ll = inner_w;
			plat_addstr(line, ll, 0);
			for (int x = ll; x < inner_w; x++) plat_addch(' ', 0);
			line = nl ? nl + 1 : NULL;
		} else {
			for (int x = 0; x < inner_w; x++) plat_addch(' ', 0);
		}
	}
	free(content);
}

static void draw_editing_box(struct Box *b, int r, int c, int w, int h)
{
	int inner_w = w - 2, inner_h = h - 2;
	int rows = plat_rows();

	if (inner_w <= 0 || inner_h <= 0) return;

	/* title */
	if (r + 1 >= 0 && r + 1 < rows - 1) {
		plat_move(r + 1, c + 1);
		if (app.editing_title) {
			int tlen = app.title_len;
			if (tlen > inner_w) tlen = inner_w;
			plat_addstr(app.title_buf, tlen, ATTR_BOLD | ATTR_REVERSE);
			for (int x = tlen; x < inner_w; x++) plat_addch(' ', ATTR_REVERSE);
			int tc = app.title_cursor < inner_w ? app.title_cursor : inner_w - 1;
			plat_show_cursor(r + 1, c + 1 + tc);
			plat_cursor_style(5);
			return;
		}
		int tlen = (int)strlen(b->title);
		if (tlen > inner_w) tlen = inner_w;
		plat_addstr(b->title, tlen, ATTR_BOLD | ATTR_REVERSE);
		for (int x = tlen; x < inner_w; x++) plat_addch(' ', ATTR_REVERSE);
	}

	/* cursor + scroll + body rendering */
	int cursor_pos = b->body.gap_start;
	int len = gap_len(&b->body);
	int cursor_line = 0;
	{
		int ln = 0;
		for (int ci = 0; ci < len; ci++) {
			if (ci == cursor_pos) cursor_line = ln;
			if (gap_char_at(&b->body, ci) == '\n') ln++;
		}
		if (cursor_pos == len) cursor_line = ln;
	}

	int body_rows = inner_h - 1;
	int scroll = cursor_line >= body_rows ? cursor_line - body_rows + 1 : 0;

	int char_idx = 0, cur_line = 0;
	int cursor_row = -1, cursor_col = -1;

	while (cur_line < scroll && char_idx < len) {
		if (gap_char_at(&b->body, char_idx) == '\n') cur_line++;
		char_idx++;
	}

	/* visual highlight range */
	int vis_from = -1, vis_to = -1;
	if (app.vim.mode == MODE_VISUAL || app.vim.mode == MODE_VLINE) {
		vis_from = app.vim.visual_start;
		vis_to = cursor_pos;
		if (vis_from > vis_to) { int t = vis_from; vis_from = vis_to; vis_to = t; }
		vis_to++;
		if (app.vim.mode == MODE_VLINE) {
			int vf = vis_from, vt = vis_to > 0 ? vis_to - 1 : 0;
			while (vf > 0 && gap_char_at(&b->body, vf - 1) != '\n') vf--;
			while (vt < len && gap_char_at(&b->body, vt) != '\n') vt++;
			vis_from = vf; vis_to = vt;
		}
	}

	for (int row = 0; row < body_rows; row++) {
		int scr_row = r + 2 + row;
		if (scr_row < 0 || scr_row >= rows - 1) {
			while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n') char_idx++;
			if (char_idx < len) char_idx++;
			continue;
		}
		plat_move(scr_row, c + 1);
		int col = 0;
		while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n' && col < inner_w) {
			if (char_idx == cursor_pos) { cursor_row = scr_row; cursor_col = c + 1 + col; }
			int attr = (char_idx >= vis_from && char_idx < vis_to) ? ATTR_REVERSE : 0;
			plat_addch(gap_char_at(&b->body, char_idx), attr);
			char_idx++; col++;
		}
		if (char_idx == cursor_pos) { cursor_row = scr_row; cursor_col = c + 1 + col; }
		for (int x = col; x < inner_w; x++) plat_addch(' ', 0);
		while (char_idx < len && gap_char_at(&b->body, char_idx) != '\n') char_idx++;
		if (char_idx < len) char_idx++;
	}

	if (cursor_row < 0) { cursor_row = r + 2; cursor_col = c + 1; }
	plat_show_cursor(cursor_row, cursor_col);
	plat_cursor_style(app.vim.mode == MODE_INSERT ? 5 : 1);
}

static void draw_status(void)
{
	int cols = plat_cols(), rows = plat_rows();
	char status[256];
	const char *mode_str;

	switch (app.vim.mode) {
	case MODE_INSERT:     mode_str = "INSERT"; break;
	case MODE_VISUAL:     mode_str = "VISUAL"; break;
	case MODE_VLINE:      mode_str = "V-LINE"; break;
	case MODE_OP_PENDING: mode_str = "OP-PENDING"; break;
	case MODE_COMMAND:    mode_str = "COMMAND"; break;
	default:              mode_str = app.leader_pending ? "LEADER" : "NORMAL"; break;
	}

	if (app.vim.search_active) {
		snprintf(status, sizeof status, "/%.250s", app.vim.search_buf);
	} else if (app.vim.mode == MODE_COMMAND) {
		snprintf(status, sizeof status, ":%.250s", app.vim.cmd_buf);
	} else {
		const char *fname = app.file.name ? app.file.name : "(no file)";
		int box_num = app.focused_box >= 0 ? app.focused_box + 1 : 0;
		int cur_line = 0, cur_col = 0;
		const char *box_title = "";
		const char *tag_str = "";

		if (app.focused_box >= 0 && app.focused_box < app.file.box_count) {
			struct Box *fb = &app.file.boxes[app.focused_box];
			box_title = fb->title ? fb->title : "";
			int pos = fb->body.gap_start;
			int blen = gap_len(&fb->body);
			cur_line = 1; cur_col = 1;
			for (int ci = 0; ci < pos && ci < blen; ci++) {
				if (gap_char_at(&fb->body, ci) == '\n') { cur_line++; cur_col = 1; }
				else cur_col++;
			}
			switch (fb->tag) {
			case TAG_IDEA:      tag_str = " [idea]"; break;
			case TAG_TODO:      tag_str = " [todo]"; break;
			case TAG_REFERENCE: tag_str = " [ref]"; break;
			default: break;
			}
		}

		if (app.editing && app.focused_box >= 0) {
			snprintf(status, sizeof status,
				" %s | %.20s%s | %d:%d | %s %d/%d %s",
				mode_str, box_title, tag_str, cur_line, cur_col,
				fname, box_num, app.file.box_count,
				app.file.dirty ? "[+]" : "");
		} else {
			snprintf(status, sizeof status,
				" %s | %s | box %d/%d %s",
				mode_str, fname, box_num, app.file.box_count,
				app.file.dirty ? "[+]" : "");
		}
	}

	plat_move(rows - 1, 0);
	int slen = (int)strlen(status);
	plat_addstr(status, slen, ATTR_REVERSE);
	for (int x = slen; x < cols; x++) plat_addch(' ', ATTR_REVERSE);
}

static void redraw(void)
{
	int vp_col = px_to_col(app.vp_x);
	int vp_row = px_to_row(app.vp_y);

	plat_clear();
	plat_hide_cursor();

	for (int i = 0; i < app.file.box_count; i++) {
		struct Box *b = &app.file.boxes[i];
		int r = px_to_row(b->y) - vp_row;
		int c = px_to_col(b->x) - vp_col;
		int w = px_to_col(b->w);
		int h = px_to_row(b->h);
		if (w < 6) w = 6;
		if (h < 4) h = 4;

		int is_focused = (i == app.focused_box);
		int is_hovered = (i == app.hovered_box);
		draw_box_border(r, c, w, h, is_focused, is_hovered, b->tag);

		if (is_focused && app.editing)
			draw_editing_box(b, r, c, w, h);
		else
			draw_box_content(b, r, c, w, h, is_focused);
	}

	if (app.file.box_count == 0) {
		plat_move(1, 2);
		plat_addstr("No boxes yet.  Type  :newbox  then Enter.", -1, ATTR_DIM);
		plat_move(2, 2);
		plat_addstr("Press q to quit.", -1, ATTR_DIM);
	}

	draw_status();
	plat_refresh();
}

/* --- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int key;
	struct MouseEvent mouse;

	conf_ensure_dir();
	conf_load();

	app_init(&app, argc >= 2 ? argv[1] : "notes.md");

	plat_init();

	/* tell app our viewport size in file-pixel coords */
	app.view_w = plat_cols() * cfg_cell_w;
	app.view_h = (plat_rows() - 1) * cfg_cell_h; /* -1 for status bar */

	redraw();

	while (app.running) {
		int result = plat_getinput(&key, &mouse, 50);

		if (result == OM_INPUT_RESIZE) {
			app.view_w = plat_cols() * cfg_cell_w;
			app.view_h = (plat_rows() - 1) * cfg_cell_h;
			redraw();
			continue;
		}
		if (result == OM_INPUT_NONE)
			continue;

		if (result == OM_INPUT_MOUSE) {
			/* convert cell coords to file-pixel coords */
			int vp_col = px_to_col(app.vp_x);
			int vp_row = px_to_row(app.vp_y);
			int px = col_to_px(mouse.col + vp_col);
			int py = row_to_px(mouse.row + vp_row);

			if (mouse.pressed && mouse.button == 0) {
				if (app.dragging)
					app_mouse_move(&app, px, py);
				else
					app_mouse_down(&app, px, py, mouse.button);
			} else if (!mouse.pressed && app.dragging) {
				app_mouse_up(&app, px, py, mouse.button);
			} else if (mouse.pressed && app.dragging) {
				app_mouse_move(&app, px, py);
			}
			redraw();
			continue;
		}

		/* OM_INPUT_KEY */
		app_key(&app, key);
		redraw();
	}

	plat_deinit();
	app_destroy(&app);
	return 0;
}
