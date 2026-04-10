/* onemark — main entry point
 *
 * Spatial canvas rendering, box navigation, input loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/om.h"

/* --- coordinate scaling -------------------------------------------------- */
#define CELL_W 8   /* pixels per character column */
#define CELL_H 16  /* pixels per character row */

/* Convert pixel coords to terminal cells */
static int px_to_col(int px) { return px / CELL_W; }
static int px_to_row(int py) { return py / CELL_H; }

/* --- viewport ------------------------------------------------------------ */
static int vp_row, vp_col; /* viewport scroll offset in cells */

/* --- app state ----------------------------------------------------------- */
static struct NotebookFile file;
static struct VimState vim;
static int focused_box = -1; /* index into file.boxes, -1 = none */
static int editing = 0;      /* 1 = editing box body (vim active) */
static int running = 1;

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
	int title_attr = ATTR_BOLD;

	if (inner_w <= 0 || inner_h <= 0) return;

	/* title line */
	if (r + 1 >= 0 && r + 1 < rows - 1) {
		int tlen = strlen(b->title);
		if (tlen > inner_w) tlen = inner_w;
		plat_move(r + 1, c + 1);
		plat_addstr(b->title, tlen, title_attr);
		/* pad */
		for (int x = tlen; x < inner_w; x++)
			plat_addch(' ', 0);
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
	if (cursor_row >= 0) {
		plat_move(cursor_row, cursor_col);
		/* show hardware cursor */
		printf("\033[?25h");
	}
}

static void draw_status(void)
{
	int cols = plat_cols();
	int rows = plat_rows();
	char status[256];
	const char *mode_str;

	switch (vim.mode) {
	case MODE_INSERT:  mode_str = "INSERT"; break;
	case MODE_COMMAND: mode_str = "COMMAND"; break;
	default:           mode_str = "NORMAL"; break;
	}

	if (vim.mode == MODE_COMMAND) {
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
	printf("\033[?25l"); /* hide cursor during draw */

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

	{
		const char *path = argc >= 2 ? argv[1] : "notes.md";
		if (file_parse(&file, path) != 0) {
			/* file doesn't exist or is empty — create new */
			file_init_empty(&file, path);
		}
	}

	vim_init(&vim);
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
						printf("\033[?25l"); /* hide cursor */
					}
				} else {
					focused_box = -1;
					editing = 0;
					printf("\033[?25l");
				}
				redraw();
			}
			continue;
		}

		/* INPUT_KEY */

		/* --- command mode (works with or without focused box) --- */
		if (vim.mode == MODE_COMMAND) {
			vim_keypress(&vim, focused_box >= 0 ? &file.boxes[focused_box].body : NULL, key);

			switch (vim.result) {
			case VIM_RESULT_SAVE:
				file_save(&file);
				file.dirty = 0;
				break;
			case VIM_RESULT_QUIT:
				editing = 0;
				printf("\033[?25l");
				break;
			case VIM_RESULT_SAVEQUIT:
				file_save(&file);
				file.dirty = 0;
				editing = 0;
				printf("\033[?25l");
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
					box_init_new(nb, nx, ny);
					file.box_count++;
					focused_box = file.box_count - 1;
					editing = 1;
					vim.mode = MODE_INSERT;
					file.dirty = 1;
				}
				break;
			}

			if (vim.mode == MODE_NORMAL)
				editing = (focused_box >= 0) ? editing : 0;

			redraw();
			continue;
		}

		/* --- box editing (vim normal/insert inside a box) --- */
		if (editing && focused_box >= 0) {
			struct Box *b = &file.boxes[focused_box];
			vim_keypress(&vim, &b->body, key);

			if (vim.result == VIM_RESULT_SAVE) {
				file_save(&file);
				file.dirty = 0;
			} else if (vim.result == VIM_RESULT_QUIT) {
				editing = 0;
				printf("\033[?25l");
			} else if (vim.result == VIM_RESULT_SAVEQUIT) {
				file_save(&file);
				file.dirty = 0;
				editing = 0;
				printf("\033[?25l");
			}

			if (vim.mode == MODE_NORMAL && key == KEY_ESC) {
				editing = 0;
				printf("\033[?25l");
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
			focused_box = reading_order_next(focused_box);
			break;
		case 'k':
		case KEY_UP:
			focused_box = reading_order_prev(focused_box);
			break;

		case KEY_CTRL('h'):
			focused_box = spatial_nav(focused_box, 'h');
			break;
		case KEY_CTRL('n'):
			focused_box = spatial_nav(focused_box, 'j');
			break;
		case KEY_CTRL('k'):
			focused_box = spatial_nav(focused_box, 'k');
			break;
		case KEY_CTRL('l'):
			focused_box = spatial_nav(focused_box, 'l');
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
