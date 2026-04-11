/* onemark — application controller
 *
 * All app state and input processing. No platform calls.
 */
#define _POSIX_C_SOURCE 200809L
#define CONFIG_IMPL  /* config variable definitions live here */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

/* --- helpers ------------------------------------------------------------- */

static void jump_push(struct App *app, int box, int pos)
{
	struct JumpHistory *jh = &app->jumphist;
	if (jh->count > 0) {
		int prev = jh->cursor;
		if (prev >= 0 && prev < jh->count && jh->ring[prev].box == box)
			return;
	}
	jh->count = jh->cursor + 1;
	if (jh->count >= JUMP_MAX) {
		memmove(&jh->ring[0], &jh->ring[1],
			(JUMP_MAX - 1) * sizeof(jh->ring[0]));
		jh->count = JUMP_MAX - 1;
	}
	jh->ring[jh->count].box = box;
	jh->ring[jh->count].pos = pos;
	jh->cursor = jh->count;
	jh->count++;
}

static void cycle_tag(struct Box *b)
{
	switch (b->tag) {
	case TAG_NONE:      b->tag = TAG_IDEA; break;
	case TAG_IDEA:      b->tag = TAG_TODO; break;
	case TAG_TODO:      b->tag = TAG_REFERENCE; break;
	case TAG_REFERENCE: b->tag = TAG_NONE; break;
	}
}

void app_box_free(struct Box *b)
{
	gap_free(&b->body);
	undo_free(&b->undo);
	free(b->title);
	for (int i = 0; i < b->custom_count; i++) {
		free(b->custom[i].key);
		free(b->custom[i].value);
	}
}

static void do_delete_box(struct App *app)
{
	if (app->focused_box < 0 || app->focused_box >= app->file.box_count)
		return;
	int del = app->focused_box;
	app_box_free(&app->file.boxes[del]);
	if (del < app->file.box_count - 1)
		memmove(&app->file.boxes[del], &app->file.boxes[del + 1],
			(app->file.box_count - del - 1) * sizeof(struct Box));
	app->file.box_count--;
	app->editing = 0;
	app->focused_box = del < app->file.box_count ? del : app->file.box_count - 1;
	for (int i = 0; i < 26; i++) {
		if (app->marks[i].box == del) app->marks[i].box = -1;
		else if (app->marks[i].box > del) app->marks[i].box--;
	}
	app->file.dirty = 1;
}

static void do_dup_box(struct App *app)
{
	if (app->focused_box < 0 || app->focused_box >= app->file.box_count)
		return;
	if (app->file.box_count >= MAX_BOXES) return;
	struct Box *src = &app->file.boxes[app->focused_box];
	struct Box *nb = &app->file.boxes[app->file.box_count];
	box_init_new(nb, src->x + 20, src->y + 20, cfg_box_w, cfg_box_h);
	char *content = gap_contents(&src->body);
	gap_free(&nb->body);
	gap_init(&nb->body, content, (int)strlen(content));
	free(content);
	free(nb->title);
	nb->title = strdup(src->title);
	nb->tag = src->tag;
	app->file.box_count++;
	app->focused_box = app->file.box_count - 1;
	app->editing = 1;
	if (app->vim.mode == MODE_INSERT)
		app->vim.mode = MODE_NORMAL;
	app->file.dirty = 1;
}

/* --- focus ---------------------------------------------------------------- */

static void focus_box(struct App *app, int idx)
{
	if (idx == app->focused_box) return;
	if (app->focused_box >= 0)
		jump_push(app, app->focused_box,
			  app->file.boxes[app->focused_box].body.gap_start);
	app->focused_box = idx;
	if (idx >= 0) {
		app->editing = 1;
		if (app->vim.mode == MODE_INSERT)
			app->vim.mode = MODE_NORMAL;
		jump_push(app, idx, app->file.boxes[idx].body.gap_start);
		app_scroll_to_box(app, idx);
	} else {
		app->editing = 0;
	}
}

/* --- navigation ---------------------------------------------------------- */

int app_next_box(struct App *app)
{
	if (app->file.box_count == 0) return -1;
	if (app->focused_box < 0) return 0;
	return (app->focused_box + 1) % app->file.box_count;
}

int app_prev_box(struct App *app)
{
	if (app->file.box_count == 0) return -1;
	if (app->focused_box < 0) return app->file.box_count - 1;
	return (app->focused_box - 1 + app->file.box_count) % app->file.box_count;
}

int app_spatial_nav(struct App *app, int dir_key)
{
	int cur = app->focused_box;
	if (cur < 0 || app->file.box_count < 2) return cur;

	struct Box *from = &app->file.boxes[cur];
	int fx = from->x + from->w / 2;
	int fy = from->y + from->h / 2;
	int best = -1, best_dist = 999999;

	for (int i = 0; i < app->file.box_count; i++) {
		if (i == cur) continue;
		struct Box *to = &app->file.boxes[i];
		int tx = to->x + to->w / 2;
		int ty = to->y + to->h / 2;
		int dx = tx - fx, dy = ty - fy;
		int ok = 0;
		switch (dir_key) {
		case 'h': ok = (dx < 0); break;
		case 'l': ok = (dx > 0); break;
		case 'k': ok = (dy < 0); break;
		case 'j': ok = (dy > 0); break;
		}
		if (!ok) continue;
		int dist = dx * dx + dy * dy;
		if (dist < best_dist) { best_dist = dist; best = i; }
	}
	return best >= 0 ? best : cur;
}

/* --- hit testing (file-pixel coords) ------------------------------------- */

enum HitZone app_hit_test(struct App *app, int px, int py, int *box_idx)
{
	*box_idx = -1;
	for (int i = app->file.box_count - 1; i >= 0; i--) {
		struct Box *b = &app->file.boxes[i];
		int bw = b->w < 48 ? 48 : b->w;  /* minimum visual size */
		int bh = b->h < 48 ? 48 : b->h;
		if (px >= b->x && px < b->x + bw && py >= b->y && py < b->y + bh) {
			*box_idx = i;
			/* right edge: last 16px */
			if (px >= b->x + bw - 16)
				return HIT_RESIZE;
			/* top border: first 5px */
			if (py < b->y + 5)
				return HIT_BORDER;
			/* title area: 5-25px from top */
			if (py < b->y + 25)
				return HIT_TITLE;
			/* bottom border: last 5px */
			if (py >= b->y + bh - 5)
				return HIT_BORDER;
			/* left border: first 5px */
			if (px < b->x + 5)
				return HIT_BORDER;
			return HIT_BODY;
		}
	}
	return HIT_NONE;
}

/* --- viewport ------------------------------------------------------------ */

void app_scroll_to_box(struct App *app, int idx)
{
	if (idx < 0 || idx >= app->file.box_count) return;
	struct Box *b = &app->file.boxes[idx];
	int bw = b->w < 48 ? 48 : b->w;
	int bh = b->h < 48 ? 48 : b->h;

	if (b->x - app->vp_x < 0)
		app->vp_x = b->x;
	else if (b->x + bw - app->vp_x > app->view_w)
		app->vp_x = b->x + bw - app->view_w;

	if (b->y - app->vp_y < 0)
		app->vp_y = b->y;
	else if (b->y + bh - app->vp_y > app->view_h)
		app->vp_y = b->y + bh - app->view_h;
}

/* --- lifecycle ----------------------------------------------------------- */

void app_init(struct App *app, const char *path)
{
	memset(app, 0, sizeof *app);
	app->focused_box = -1;
	app->hovered_box = -1;
	app->drag_box = -1;
	app->running = 1;

	if (file_parse(&app->file, path) != 0)
		file_init_empty(&app->file, path);

	vim_init(&app->vim);
	for (int i = 0; i < 26; i++)
		app->marks[i].box = -1;

	if (app->file.box_count > 0)
		app->focused_box = 0;
}

void app_destroy(struct App *app)
{
	for (int i = 0; i < app->file.box_count; i++)
		app_box_free(&app->file.boxes[i]);
	free(app->file.path);
	free(app->file.name);
	free(app->vim.yank.text);
}

/* --- handle vim result codes --------------------------------------------- */

static void handle_vim_result(struct App *app)
{
	struct VimState *v = &app->vim;
	struct Box *b = (app->focused_box >= 0) ?
		&app->file.boxes[app->focused_box] : NULL;

	switch (v->result) {
	case VIM_RESULT_SAVE:
		file_save(&app->file);
		app->file.dirty = 0;
		break;
	case VIM_RESULT_QUIT:
		app->editing = 0;
		break;
	case VIM_RESULT_SAVEQUIT:
		file_save(&app->file);
		app->file.dirty = 0;
		app->editing = 0;
		break;
	case VIM_RESULT_NEWBOX:
		if (app->file.box_count < MAX_BOXES) {
			struct Box *nb = &app->file.boxes[app->file.box_count];
			int nx = 40, ny = 40;
			if (app->focused_box >= 0) {
				nx = b->x + b->w + 20;
				ny = b->y;
			}
			box_init_new(nb, nx, ny, cfg_box_w, cfg_box_h);
			app->file.box_count++;
			app->focused_box = app->file.box_count - 1;
			app->editing = 1;
			app->vim.mode = MODE_INSERT;
			app->file.dirty = 1;
		}
		break;
	case VIM_RESULT_DELBOX:
		do_delete_box(app);
		break;
	case VIM_RESULT_DUPBOX:
		do_dup_box(app);
		break;
	case VIM_RESULT_SET_MARK:
		if (b) {
			int idx = v->mark_letter - 'a';
			app->marks[idx].box = app->focused_box;
			app->marks[idx].pos = b->body.gap_start;
		}
		break;
	case VIM_RESULT_JUMP_MARK: {
		int idx = v->mark_letter - 'a';
		if (app->marks[idx].box >= 0 &&
		    app->marks[idx].box < app->file.box_count) {
			focus_box(app, app->marks[idx].box);
			gap_move(&app->file.boxes[app->marks[idx].box].body,
				 app->marks[idx].pos);
		}
		break;
	}
	case VIM_RESULT_ZZ:
		file_save(&app->file);
		app->file.dirty = 0;
		app->editing = 0;
		break;
	case VIM_RESULT_ZQ:
		app->editing = 0;
		break;
	case VIM_RESULT_SET_FIELD:
		if (b) {
			char *arg = v->cmd_buf + 4;
			char *sp = strchr(arg, ' ');
			if (sp && b->custom_count < MAX_CUSTOM_FIELDS) {
				*sp = '\0';
				b->custom[b->custom_count].key = strdup(arg);
				b->custom[b->custom_count].value = strdup(sp + 1);
				b->custom_count++;
				app->file.dirty = 1;
			}
		}
		break;
	case VIM_RESULT_SET_TAG:
		if (b) {
			char *arg = v->cmd_buf + 4;
			if (strcmp(arg, "idea") == 0) b->tag = TAG_IDEA;
			else if (strcmp(arg, "todo") == 0) b->tag = TAG_TODO;
			else if (strcmp(arg, "reference") == 0) b->tag = TAG_REFERENCE;
			else b->tag = TAG_NONE;
			app->file.dirty = 1;
		}
		break;
	default:
		break;
	}
}

/* --- auto-grow box width ------------------------------------------------- */

static void auto_grow_box(struct Box *b)
{
	int max_line = 0, cur_line = 0;
	int blen = gap_len(&b->body);
	for (int ci = 0; ci < blen; ci++) {
		if (gap_char_at(&b->body, ci) == '\n') {
			if (cur_line > max_line) max_line = cur_line;
			cur_line = 0;
		} else {
			cur_line++;
		}
	}
	if (cur_line > max_line) max_line = cur_line;
	int need_cols = max_line + 3;
	if (need_cols > cfg_max_box_cols) need_cols = cfg_max_box_cols;
	int need_px = need_cols * cfg_cell_w;
	if (need_px > b->w) b->w = need_px;
}

/* --- input: keyboard ----------------------------------------------------- */

void app_key(struct App *app, int key)
{
	/* command mode works globally */
	if (app->vim.mode == MODE_COMMAND) {
		vim_keypress(&app->vim,
			app->focused_box >= 0 ? &app->file.boxes[app->focused_box].body : NULL,
			app->focused_box >= 0 ? &app->file.boxes[app->focused_box].undo : NULL,
			key);
		handle_vim_result(app);
		if (app->vim.mode == MODE_NORMAL)
			app->editing = (app->focused_box >= 0) ? app->editing : 0;
		return;
	}

	/* title editing */
	if (app->editing_title && app->focused_box >= 0) {
		switch (key) {
		case KEY_ESC: case '\r': case '\n':
			app->title_buf[app->title_len] = '\0';
			free(app->file.boxes[app->focused_box].title);
			app->file.boxes[app->focused_box].title = strdup(app->title_buf);
			app->file.boxes[app->focused_box].title_is_default = 0;
			app->editing_title = 0;
			app->file.dirty = 1;
			break;
		case KEY_BACKSPACE:
			if (app->title_cursor > 0) {
				memmove(app->title_buf + app->title_cursor - 1,
					app->title_buf + app->title_cursor,
					app->title_len - app->title_cursor);
				app->title_cursor--;
				app->title_len--;
			}
			break;
		case KEY_LEFT:
			if (app->title_cursor > 0) app->title_cursor--;
			break;
		case KEY_RIGHT:
			if (app->title_cursor < app->title_len) app->title_cursor++;
			break;
		case KEY_HOME: app->title_cursor = 0; break;
		case KEY_END:  app->title_cursor = app->title_len; break;
		default:
			if (key >= 32 && key < 127 &&
			    app->title_len < (int)sizeof(app->title_buf) - 1) {
				memmove(app->title_buf + app->title_cursor + 1,
					app->title_buf + app->title_cursor,
					app->title_len - app->title_cursor);
				app->title_buf[app->title_cursor] = key;
				app->title_cursor++;
				app->title_len++;
			}
			break;
		}
		return;
	}

	/* leader key dispatch */
	if (app->leader_pending && app->editing && app->focused_box >= 0) {
		struct Box *b = &app->file.boxes[app->focused_box];
		app->leader_pending = 0;
		switch (key) {
		case 'b':
			edit_toggle_wrap(&b->body, &b->undo, b->body.gap_start,
					 b->body.gap_start, "**");
			app->file.dirty = 1;
			break;
		case 'i':
			edit_toggle_wrap(&b->body, &b->undo, b->body.gap_start,
					 b->body.gap_start, "*");
			app->file.dirty = 1;
			break;
		case '1':
			edit_checkbox_rotate(&b->body, &b->undo, b->body.gap_start);
			app->file.dirty = 1;
			break;
		case 't':
			cycle_tag(b);
			app->file.dirty = 1;
			break;
		case 'w':
			file_save(&app->file);
			app->file.dirty = 0;
			break;
		case 'n':
			if (app->file.box_count < MAX_BOXES) {
				struct Box *nb = &app->file.boxes[app->file.box_count];
				box_init_new(nb, b->x + b->w + 20, b->y, cfg_box_w, cfg_box_h);
				app->file.box_count++;
				focus_box(app, app->file.box_count - 1);
				app->editing = 1;
				app->vim.mode = MODE_INSERT;
				app->file.dirty = 1;
			}
			break;
		case 'd':
			do_dup_box(app);
			break;
		case 'D':
			do_delete_box(app);
			break;
		}
		return;
	}

	/* box editing (vim active) */
	if (app->editing && app->focused_box >= 0) {
		struct Box *b = &app->file.boxes[app->focused_box];

		/* leader key */
		if (app->vim.mode == MODE_NORMAL && key == cfg_leader) {
			app->leader_pending = 1;
			return;
		}

		/* jump back/forward */
		if (app->vim.mode == MODE_NORMAL && key == KEY_CTRL('o')) {
			if (app->jumphist.cursor > 0) {
				app->jumphist.cursor--;
				int bi = app->jumphist.ring[app->jumphist.cursor].box;
				if (bi >= 0 && bi < app->file.box_count) {
					app->focused_box = bi;
					gap_move(&app->file.boxes[bi].body,
						 app->jumphist.ring[app->jumphist.cursor].pos);
					app_scroll_to_box(app, bi);
				}
			}
			return;
		}
		if (app->vim.mode == MODE_NORMAL && key == KEY_CTRL('i')) {
			if (app->jumphist.cursor < app->jumphist.count - 1) {
				app->jumphist.cursor++;
				int bi = app->jumphist.ring[app->jumphist.cursor].box;
				if (bi >= 0 && bi < app->file.box_count) {
					app->focused_box = bi;
					gap_move(&app->file.boxes[bi].body,
						 app->jumphist.ring[app->jumphist.cursor].pos);
					app_scroll_to_box(app, bi);
				}
			}
			return;
		}

		/* bracket nav: two-key sequences ]/[ then b/h/l */
		if (app->vim.mode == MODE_NORMAL && (key == ']' || key == '[')) {
			app->bracket_pending = key;
			return;
		}
		if (app->bracket_pending) {
			int dir = app->bracket_pending;
			app->bracket_pending = 0;
			if (key == ']' || key == '[') {
				int np = (dir == ']')
					? edit_next_heading(&b->body, b->body.gap_start)
					: edit_prev_heading(&b->body, b->body.gap_start);
				gap_move(&b->body, np);
			} else if (key == 'b') {
				int next = (dir == ']') ? app_next_box(app) : app_prev_box(app);
				if (next >= 0) {
					app->editing = 0;
					focus_box(app, next);
				}
			} else if (key == 'l') {
				int np = (dir == ']')
					? edit_next_list_item(&b->body, b->body.gap_start)
					: edit_prev_list_item(&b->body, b->body.gap_start);
				gap_move(&b->body, np);
			} else if (key == 'h') {
				int np = (dir == ']')
					? edit_next_heading(&b->body, b->body.gap_start)
					: edit_prev_heading(&b->body, b->body.gap_start);
				gap_move(&b->body, np);
			}
			return;
		}

		/* dispatch to vim engine */
		vim_keypress(&app->vim, &b->body, &b->undo, key);
		handle_vim_result(app);

		if (app->vim.result == VIM_RESULT_NONE && app->vim.mode == MODE_INSERT)
			app->file.dirty = 1;

		auto_grow_box(b);

		/* Esc in normal mode exits editing */
		if (app->vim.mode == MODE_NORMAL && key == KEY_ESC) {
			app->editing = 0;
		}
		return;
	}

	/* canvas-level keys (not editing) */
	switch (key) {
	case 'q':
		if (app->file.dirty) file_save(&app->file);
		app->running = 0;
		break;
	case '\r': case '\n': case 'i':
		if (app->focused_box >= 0) {
			app->editing = 1;
			app->vim.mode = (key == 'i') ? MODE_INSERT : MODE_NORMAL;
		}
		break;
	case KEY_ESC:
		app->focused_box = -1;
		break;
	case 'j': case KEY_DOWN:
		focus_box(app, app_next_box(app));
		break;
	case 'k': case KEY_UP:
		focus_box(app, app_prev_box(app));
		break;
	case KEY_CTRL('h'):
		focus_box(app, app_spatial_nav(app, 'h'));
		break;
	case KEY_CTRL('n'):
		focus_box(app, app_spatial_nav(app, 'j'));
		break;
	case KEY_CTRL('k'):
		focus_box(app, app_spatial_nav(app, 'k'));
		break;
	case KEY_CTRL('l'):
		focus_box(app, app_spatial_nav(app, 'l'));
		break;
	case ':':
		app->vim.mode = MODE_COMMAND;
		app->vim.cmd_len = 0;
		app->vim.cmd_buf[0] = '\0';
		break;
	}
}

/* --- input: mouse -------------------------------------------------------- */

void app_mouse_down(struct App *app, int px, int py, int button)
{
	if (button != 0) return; /* left button only */

	int box_idx;
	enum HitZone zone = app_hit_test(app, px, py, &box_idx);

	if (box_idx >= 0) {
		if (zone == HIT_RESIZE) {
			app->dragging = 2;
			app->drag_box = box_idx;
			focus_box(app, box_idx);
		} else if (zone == HIT_TITLE && box_idx == app->focused_box &&
			   !app->dragging) {
			/* title of focused box → edit title */
			app->editing_title = 1;
			struct Box *tb = &app->file.boxes[box_idx];
			app->title_len = snprintf(app->title_buf, sizeof app->title_buf,
						  "%s", tb->title);
			app->title_cursor = app->title_len;
		} else if (zone == HIT_BORDER || zone == HIT_TITLE) {
			/* border/title of unfocused → drag */
			app->dragging = 1;
			app->drag_box = box_idx;
			focus_box(app, box_idx);
			struct Box *b = &app->file.boxes[box_idx];
			app->drag_ox = px - b->x;
			app->drag_oy = py - b->y;
		} else {
			focus_box(app, box_idx);
			app->editing_title = 0;
		}
	} else {
		/* click on empty canvas → create box */
		if (app->file.box_count < MAX_BOXES) {
			struct Box *nb = &app->file.boxes[app->file.box_count];
			box_init_new(nb, px, py, cfg_box_w, cfg_box_h);
			app->file.box_count++;
			app->focused_box = app->file.box_count - 1;
			app->editing = 1;
			app->vim.mode = MODE_INSERT;
			app->file.dirty = 1;
		}
	}
}

void app_mouse_up(struct App *app, int px, int py, int button)
{
	(void)px; (void)py; (void)button;
	if (app->dragging) {
		app->dragging = 0;
		app->drag_box = -1;
		app->file.dirty = 1;
	}
}

void app_mouse_move(struct App *app, int px, int py)
{
	if (app->dragging && app->drag_box >= 0 &&
	    app->drag_box < app->file.box_count) {
		struct Box *b = &app->file.boxes[app->drag_box];
		if (app->dragging == 1) {
			int nx = px - app->drag_ox;
			int ny = py - app->drag_oy;
			if (nx < 0) nx = 0;
			if (ny < 0) ny = 0;
			b->x = nx;
			b->y = ny;
		} else if (app->dragging == 2) {
			int new_w = px - b->x;
			if (new_w < 48) new_w = 48;
			b->w = new_w;
		}
	} else {
		/* hover detection */
		int box_idx;
		app_hit_test(app, px, py, &box_idx);
		app->hovered_box = box_idx;
	}
}
