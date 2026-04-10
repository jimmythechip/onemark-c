/* onemark — vim modal editing engine
 *
 * Operator-motion composition: [count] operator [count] motion
 * Modes: NORMAL, INSERT, VISUAL, VLINE, OP_PENDING, COMMAND
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "om.h"

/* --- undo ring ----------------------------------------------------------- */

void undo_init(struct UndoRing *u)
{
	memset(u, 0, sizeof *u);
	u->current = -1;
}

void undo_free(struct UndoRing *u)
{
	for (int i = 0; i < u->count; i++)
		free(u->entries[i].text);
	memset(u, 0, sizeof *u);
	u->current = -1;
}

void undo_push(struct UndoRing *u, struct GapBuf *buf)
{
	/* discard any redo entries beyond current */
	for (int i = u->current + 1; i < u->count; i++) {
		free(u->entries[i].text);
		u->entries[i].text = NULL;
	}
	u->count = u->current + 1;

	/* if full, shift everything down */
	if (u->count >= UNDO_MAX) {
		free(u->entries[0].text);
		memmove(&u->entries[0], &u->entries[1],
			(UNDO_MAX - 1) * sizeof(struct UndoEntry));
		u->count = UNDO_MAX - 1;
	}

	u->entries[u->count].text = gap_contents(buf);
	u->entries[u->count].cursor = buf->gap_start;
	u->current = u->count;
	u->count++;
}

int undo_undo(struct UndoRing *u, struct GapBuf *buf)
{
	if (u->current <= 0) return -1;
	u->current--;
	struct UndoEntry *e = &u->entries[u->current];
	int len = strlen(e->text);
	gap_free(buf);
	gap_init(buf, e->text, len);
	gap_move(buf, e->cursor < len ? e->cursor : len);
	return e->cursor;
}

int undo_redo(struct UndoRing *u, struct GapBuf *buf)
{
	if (u->current >= u->count - 1) return -1;
	u->current++;
	struct UndoEntry *e = &u->entries[u->current];
	int len = strlen(e->text);
	gap_free(buf);
	gap_init(buf, e->text, len);
	gap_move(buf, e->cursor < len ? e->cursor : len);
	return e->cursor;
}

/* --- motion helpers ------------------------------------------------------ */

static int line_start(struct GapBuf *buf, int pos)
{
	while (pos > 0 && gap_char_at(buf, pos - 1) != '\n')
		pos--;
	return pos;
}

static int line_end(struct GapBuf *buf, int pos)
{
	int len = gap_len(buf);
	while (pos < len && gap_char_at(buf, pos) != '\n')
		pos++;
	return pos;
}

static int is_word_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* A motion returns {from, to} range. If from == to, it's just a cursor move.
 * For linewise motions, the range covers full lines including trailing \n. */
struct Range { int from; int to; int linewise; };

static struct Range motion_range(struct GapBuf *buf, int pos, int key, int count)
{
	struct Range r = { pos, pos, 0 };
	int len = gap_len(buf);
	int newpos = pos;

	for (int rep = 0; rep < count; rep++) {
		switch (key) {
		case 'h':
		case KEY_LEFT:
			if (newpos > line_start(buf, newpos)) newpos--;
			break;
		case 'l':
		case KEY_RIGHT: {
			int le = line_end(buf, newpos);
			if (newpos < le && (le == len || newpos < le - 1))
				newpos++;
			break;
		}
		case 'j':
		case KEY_DOWN: {
			int ls = line_start(buf, newpos);
			int col = newpos - ls;
			int le = line_end(buf, newpos);
			if (le < len) {
				int nls = le + 1;
				int nle = line_end(buf, nls);
				int nlen = nle - nls;
				newpos = nls + (col < nlen ? col : (nlen > 0 ? nlen - 1 : 0));
			}
			break;
		}
		case 'k':
		case KEY_UP: {
			int ls = line_start(buf, newpos);
			int col = newpos - ls;
			if (ls > 0) {
				int ple = ls - 1;
				int pls = line_start(buf, ple);
				int plen = ple - pls;
				newpos = pls + (col < plen ? col : (plen > 0 ? plen - 1 : 0));
			}
			break;
		}
		case 'w': {
			if (newpos < len && is_word_char(gap_char_at(buf, newpos)))
				while (newpos < len && is_word_char(gap_char_at(buf, newpos))) newpos++;
			else
				while (newpos < len && !is_word_char(gap_char_at(buf, newpos))
				       && gap_char_at(buf, newpos) != '\n') newpos++;
			while (newpos < len && (gap_char_at(buf, newpos) == ' '
			       || gap_char_at(buf, newpos) == '\n')) newpos++;
			break;
		}
		case 'b': {
			if (newpos > 0) newpos--;
			while (newpos > 0 && !is_word_char(gap_char_at(buf, newpos))) newpos--;
			while (newpos > 0 && is_word_char(gap_char_at(buf, newpos - 1))) newpos--;
			break;
		}
		case 'e': {
			if (newpos < len - 1) newpos++;
			while (newpos < len && !is_word_char(gap_char_at(buf, newpos))) newpos++;
			while (newpos < len - 1 && is_word_char(gap_char_at(buf, newpos + 1))) newpos++;
			break;
		}
		case '0':
			newpos = line_start(buf, newpos);
			break;
		case '$':
			newpos = line_end(buf, newpos);
			if (newpos > 0 && gap_char_at(buf, newpos) == '\n') newpos--;
			if (newpos < line_start(buf, newpos)) newpos = line_start(buf, newpos);
			break;
		case '^': {
			int ls = line_start(buf, newpos);
			newpos = ls;
			while (newpos < len && gap_char_at(buf, newpos) == ' ') newpos++;
			break;
		}
		case 'G':
			newpos = len > 0 ? len - 1 : 0;
			break;
		}
	}

	/* for operator-pending: range is from pos to newpos */
	if (newpos < pos) { r.from = newpos; r.to = pos; }
	else { r.from = pos; r.to = newpos; }

	/* j/k are linewise when used with operators */
	if (key == 'j' || key == 'k') r.linewise = 1;
	/* $ includes the char at end */
	if (key == '$' && r.to < len) r.to++;

	return r;
}

/* Expand range to full lines */
static struct Range linewise_range(struct GapBuf *buf, struct Range r)
{
	r.from = line_start(buf, r.from);
	r.to = line_end(buf, r.to);
	if (r.to < gap_len(buf)) r.to++; /* include newline */
	r.linewise = 1;
	return r;
}

/* --- yank/delete helpers ------------------------------------------------- */

static void yank_range(struct VimState *v, struct GapBuf *buf, int from, int to, int linewise)
{
	int n = to - from;
	if (n <= 0) return;
	free(v->yank.text);
	v->yank.text = malloc(n + 1);
	for (int i = 0; i < n; i++)
		v->yank.text[i] = gap_char_at(buf, from + i);
	v->yank.text[n] = '\0';
	v->yank.len = n;
	v->yank.linewise = linewise;
}

static void delete_range(struct GapBuf *buf, int from, int to)
{
	gap_move(buf, to);
	gap_delete(buf, to - from);
}

/* --- apply operator to range --------------------------------------------- */

static void apply_operator(struct VimState *v, struct GapBuf *buf,
			   struct UndoRing *undo, struct Range r)
{
	if (r.linewise)
		r = linewise_range(buf, r);

	int from = r.from, to = r.to;
	if (from > to) { int t = from; from = to; to = t; }
	if (from == to && v->op != 'y') return;

	switch (v->op) {
	case 'd':
		undo_push(undo, buf);
		yank_range(v, buf, from, to, r.linewise);
		delete_range(buf, from, to);
		gap_move(buf, from);
		break;
	case 'y':
		yank_range(v, buf, from, to, r.linewise);
		gap_move(buf, from);
		break;
	case 'c':
		undo_push(undo, buf);
		yank_range(v, buf, from, to, r.linewise);
		delete_range(buf, from, to);
		gap_move(buf, from);
		v->mode = MODE_INSERT;
		break;
	}
}

/* --- init ---------------------------------------------------------------- */

void vim_init(struct VimState *v)
{
	memset(v, 0, sizeof *v);
	v->mode = MODE_NORMAL;
}

/* --- main keypress handler ----------------------------------------------- */

void vim_keypress(struct VimState *v, struct GapBuf *buf, int key)
{
	int pos, len;

	v->result = VIM_RESULT_NONE;
	pos = buf ? buf->gap_start : 0;
	len = buf ? gap_len(buf) : 0;

	/* need an undo ring pointer — passed via the buf's owning Box.
	 * We use a trick: the UndoRing is right after GapBuf in struct Box.
	 * This is fragile but avoids changing the API. */
	struct UndoRing *undo = buf ? (struct UndoRing *)(buf + 1) : NULL;

	switch (v->mode) {

	/* ================================================================== */
	case MODE_INSERT:
		if (!buf) { v->mode = MODE_NORMAL; break; }
		switch (key) {
		case KEY_ESC:
			v->mode = MODE_NORMAL;
			if (buf->gap_start > 0 &&
			    gap_char_at(buf, buf->gap_start - 1) != '\n')
				gap_move(buf, buf->gap_start - 1);
			/* push undo after exiting insert */
			if (undo) undo_push(undo, buf);
			break;
		case KEY_BACKSPACE:
			if (buf->gap_start > 0)
				gap_delete(buf, 1);
			break;
		case KEY_DEL:
			gap_delete_fwd(buf, 1);
			break;
		case '\r': case '\n':
			gap_insert(buf, '\n');
			break;
		case '\t':
			gap_insert(buf, '\t');
			break;
		case KEY_LEFT:
			if (buf->gap_start > 0) gap_move(buf, buf->gap_start - 1);
			break;
		case KEY_RIGHT:
			if (buf->gap_start < len) gap_move(buf, buf->gap_start + 1);
			break;
		case KEY_UP: {
			struct Range r = motion_range(buf, buf->gap_start, 'k', 1);
			gap_move(buf, r.from == pos ? r.to : r.from);
			break;
		}
		case KEY_DOWN: {
			struct Range r = motion_range(buf, buf->gap_start, 'j', 1);
			gap_move(buf, r.to > pos ? r.to : r.from);
			break;
		}
		default:
			if (key >= 32 && key < 127)
				gap_insert(buf, key);
			break;
		}
		break;

	/* ================================================================== */
	case MODE_NORMAL:
		if (!buf) break;

		/* numeric count prefix */
		if (key >= '1' && key <= '9') {
			v->count = v->count * 10 + (key - '0');
			break;
		}
		if (key == '0' && v->count > 0) {
			v->count = v->count * 10;
			break;
		}

		{
		int cnt = v->count > 0 ? v->count : 1;

		/* two-key: g prefix */
		if (v->pending_g) {
			v->pending_g = 0;
			v->count = 0;
			if (key == 'g') gap_move(buf, 0); /* gg */
			break;
		}

		switch (key) {
		/* --- enter insert --- */
		case 'i':
			if (undo) undo_push(undo, buf);
			v->mode = MODE_INSERT;
			break;
		case 'a':
			if (undo) undo_push(undo, buf);
			if (pos < len && gap_char_at(buf, pos) != '\n')
				gap_move(buf, pos + 1);
			v->mode = MODE_INSERT;
			break;
		case 'I':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_start(buf, pos));
			v->mode = MODE_INSERT;
			break;
		case 'A':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_end(buf, pos));
			v->mode = MODE_INSERT;
			break;
		case 'o':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_end(buf, pos));
			gap_insert(buf, '\n');
			v->mode = MODE_INSERT;
			break;
		case 'O':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_start(buf, pos));
			gap_insert(buf, '\n');
			gap_move(buf, buf->gap_start - 1);
			v->mode = MODE_INSERT;
			break;

		/* --- motions (no operator pending) --- */
		case 'h': case 'l': case 'j': case 'k':
		case 'w': case 'b': case 'e':
		case '0': case '$': case '^': case 'G':
		case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN: {
			struct Range r = motion_range(buf, pos, key, cnt);
			int target = (key == 'h' || key == 'k' || key == 'b' || key == '0'
				|| key == '^' || key == KEY_LEFT || key == KEY_UP)
				? r.from : r.to;
			/* for $ keep the to position */
			if (key == '$') target = r.to > 0 ? r.to - 1 : 0;
			/* for G, e — use the returned to */
			if (key == 'G' || key == 'e') target = r.to;
			gap_move(buf, target);
			break;
		}

		/* --- g prefix --- */
		case 'g':
			v->pending_g = 1;
			break;

		/* --- operators --- */
		case 'd': case 'y': case 'c':
			v->op = key;
			v->mode = MODE_OP_PENDING;
			break;

		/* --- x / X --- */
		case 'x':
			if (pos < len) {
				if (undo) undo_push(undo, buf);
				yank_range(v, buf, pos, pos + 1, 0);
				gap_delete_fwd(buf, 1);
			}
			break;
		case 'X':
			if (pos > 0) {
				if (undo) undo_push(undo, buf);
				yank_range(v, buf, pos - 1, pos, 0);
				gap_delete(buf, 1);
			}
			break;

		/* --- paste --- */
		case 'p':
			if (v->yank.text) {
				if (undo) undo_push(undo, buf);
				if (v->yank.linewise) {
					gap_move(buf, line_end(buf, pos));
					if (pos < len) gap_move(buf, buf->gap_start + 1);
					else gap_insert(buf, '\n');
				} else {
					if (pos < len) gap_move(buf, pos + 1);
				}
				gap_insert_str(buf, v->yank.text, v->yank.len);
				gap_move(buf, buf->gap_start - 1);
			}
			break;
		case 'P':
			if (v->yank.text) {
				if (undo) undo_push(undo, buf);
				if (v->yank.linewise)
					gap_move(buf, line_start(buf, pos));
				gap_insert_str(buf, v->yank.text, v->yank.len);
				gap_move(buf, buf->gap_start - v->yank.len);
			}
			break;

		/* --- J (join) --- */
		case 'J': {
			int le = line_end(buf, pos);
			if (le < len) {
				if (undo) undo_push(undo, buf);
				gap_move(buf, le);
				gap_delete_fwd(buf, 1);
				while (buf->gap_end < buf->cap && buf->buf[buf->gap_end] == ' ')
					gap_delete_fwd(buf, 1);
				gap_insert(buf, ' ');
			}
			break;
		}

		/* --- undo/redo --- */
		case 'u':
			if (undo) undo_undo(undo, buf);
			break;
		case KEY_CTRL('r'):
			if (undo) undo_redo(undo, buf);
			break;

		/* --- : command mode --- */
		case ':':
			v->mode = MODE_COMMAND;
			v->cmd_len = 0;
			v->cmd_buf[0] = '\0';
			break;

		default:
			break;
		}
		v->count = 0;
		}
		break;

	/* ================================================================== */
	case MODE_OP_PENDING:
		if (!buf) { v->mode = MODE_NORMAL; v->op = 0; break; }

		/* ESC cancels */
		if (key == KEY_ESC) {
			v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* dd / yy / cc — linewise on current line */
		if (key == v->op) {
			int cnt = v->count > 0 ? v->count : 1;
			struct Range r;
			r.from = line_start(buf, pos);
			r.to = pos;
			for (int i = 0; i < cnt; i++) {
				r.to = line_end(buf, r.to);
				if (r.to < len) r.to++;
			}
			r.linewise = 1;
			apply_operator(v, buf, undo, r);
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* numeric count between op and motion */
		if (key >= '1' && key <= '9') {
			v->count = v->count * 10 + (key - '0');
			break;
		}
		if (key == '0' && v->count > 0) {
			v->count = v->count * 10;
			break;
		}

		/* motion key */
		{
			int cnt = v->count > 0 ? v->count : 1;
			int is_motion = 0;

			switch (key) {
			case 'h': case 'l': case 'j': case 'k':
			case 'w': case 'b': case 'e':
			case '0': case '$': case '^': case 'G':
			case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
				is_motion = 1;
				break;
			}

			if (is_motion) {
				struct Range r = motion_range(buf, pos, key, cnt);
				/* w/e/$/G: range extends to include the endpoint char */
				if (key == 'w' || key == 'e' || key == '$') {
					if (r.to < len) r.to++;
				}
				apply_operator(v, buf, undo, r);
				if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			} else {
				v->mode = MODE_NORMAL;
			}
			v->op = 0;
			v->count = 0;
		}
		break;

	/* ================================================================== */
	case MODE_COMMAND:
		switch (key) {
		case KEY_ESC:
			v->mode = MODE_NORMAL;
			break;
		case KEY_BACKSPACE:
			if (v->cmd_len > 0)
				v->cmd_buf[--v->cmd_len] = '\0';
			else
				v->mode = MODE_NORMAL;
			break;
		case '\r': case '\n':
			v->cmd_buf[v->cmd_len] = '\0';
			if (strcmp(v->cmd_buf, "w") == 0)
				v->result = VIM_RESULT_SAVE;
			else if (strcmp(v->cmd_buf, "q") == 0)
				v->result = VIM_RESULT_QUIT;
			else if (strcmp(v->cmd_buf, "wq") == 0 || strcmp(v->cmd_buf, "x") == 0)
				v->result = VIM_RESULT_SAVEQUIT;
			else if (strcmp(v->cmd_buf, "q!") == 0)
				v->result = VIM_RESULT_QUIT;
			else if (strcmp(v->cmd_buf, "newbox") == 0)
				v->result = VIM_RESULT_NEWBOX;
			else if (strcmp(v->cmd_buf, "del") == 0)
				v->result = VIM_RESULT_DELBOX;
			else if (strcmp(v->cmd_buf, "dup") == 0)
				v->result = VIM_RESULT_DUPBOX;
			v->mode = MODE_NORMAL;
			break;
		default:
			if (key >= 32 && key < 127 && v->cmd_len < (int)sizeof(v->cmd_buf) - 1)
				v->cmd_buf[v->cmd_len++] = key;
			break;
		}
		break;

	default:
		break;
	}
}
