/* onemark — vim modal editing engine
 *
 * Operator-motion composition: [count] operator [count] motion
 * Modes: NORMAL, INSERT, VISUAL, VLINE, OP_PENDING, COMMAND
 * Features: motions, operators, text objects, f/t/F/T, %, r, s/S,
 *           C/D/Y, search (/n/N), marks (m/'), repeat (.), undo (u/C-r)
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
	/* skip if buffer content hasn't changed since last snapshot */
	if (u->current >= 0 && u->entries[u->current].text) {
		char *cur = gap_contents(buf);
		if (strcmp(cur, u->entries[u->current].text) == 0) {
			u->entries[u->current].cursor = buf->gap_start;
			free(cur);
			return;
		}
		free(cur);
	}

	/* discard any redo entries beyond current */
	for (int i = u->current + 1; i < u->count; i++) {
		free(u->entries[i].text);
		u->entries[i].text = NULL;
	}
	u->count = u->current + 1;

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

/* --- line helpers -------------------------------------------------------- */

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

static int first_nonblank(struct GapBuf *buf, int pos)
{
	int ls = line_start(buf, pos);
	int len = gap_len(buf);
	while (ls < len && (gap_char_at(buf, ls) == ' ' || gap_char_at(buf, ls) == '\t'))
		ls++;
	return ls;
}

static int is_word_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* --- motion range -------------------------------------------------------- */

struct Range { int from; int to; int linewise; };

static struct Range motion_range(struct GapBuf *buf, int pos, int key, int count)
{
	struct Range r = { pos, pos, 0 };
	int len = gap_len(buf);
	int newpos = pos;

	for (int rep = 0; rep < count; rep++) {
		switch (key) {
		case 'h': case KEY_LEFT:
			if (newpos > line_start(buf, newpos)) newpos--;
			break;
		case 'l': case KEY_RIGHT: {
			int le = line_end(buf, newpos);
			if (newpos < le && (le == len || newpos < le - 1))
				newpos++;
			break;
		}
		case 'j': case KEY_DOWN: {
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
		case 'k': case KEY_UP: {
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
		case 'W': {
			/* WORD: non-blank sequences */
			while (newpos < len && gap_char_at(buf, newpos) != ' '
			       && gap_char_at(buf, newpos) != '\n'
			       && gap_char_at(buf, newpos) != '\t') newpos++;
			while (newpos < len && (gap_char_at(buf, newpos) == ' '
			       || gap_char_at(buf, newpos) == '\n'
			       || gap_char_at(buf, newpos) == '\t')) newpos++;
			break;
		}
		case 'b': {
			if (newpos > 0) newpos--;
			while (newpos > 0 && !is_word_char(gap_char_at(buf, newpos))) newpos--;
			while (newpos > 0 && is_word_char(gap_char_at(buf, newpos - 1))) newpos--;
			break;
		}
		case 'B': {
			if (newpos > 0) newpos--;
			while (newpos > 0 && (gap_char_at(buf, newpos) == ' '
			       || gap_char_at(buf, newpos) == '\n'
			       || gap_char_at(buf, newpos) == '\t')) newpos--;
			while (newpos > 0 && gap_char_at(buf, newpos - 1) != ' '
			       && gap_char_at(buf, newpos - 1) != '\n'
			       && gap_char_at(buf, newpos - 1) != '\t') newpos--;
			break;
		}
		case 'e': {
			if (newpos < len - 1) newpos++;
			while (newpos < len && !is_word_char(gap_char_at(buf, newpos))) newpos++;
			while (newpos < len - 1 && is_word_char(gap_char_at(buf, newpos + 1))) newpos++;
			break;
		}
		case 'E': {
			if (newpos < len - 1) newpos++;
			while (newpos < len && (gap_char_at(buf, newpos) == ' '
			       || gap_char_at(buf, newpos) == '\n'
			       || gap_char_at(buf, newpos) == '\t')) newpos++;
			while (newpos < len - 1 && gap_char_at(buf, newpos + 1) != ' '
			       && gap_char_at(buf, newpos + 1) != '\n'
			       && gap_char_at(buf, newpos + 1) != '\t') newpos++;
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
		case '^':
			newpos = first_nonblank(buf, newpos);
			break;
		case 'G':
			newpos = len > 0 ? len - 1 : 0;
			break;
		case '{': {
			if (newpos > 0) newpos--;
			while (newpos > 0) {
				int ls = line_start(buf, newpos);
				if (ls == newpos || (ls == newpos && gap_char_at(buf, ls) == '\n')) break;
				newpos = ls > 0 ? ls - 1 : 0;
			}
			newpos = line_start(buf, newpos);
			break;
		}
		case '}': {
			int le = line_end(buf, newpos);
			if (le < len) newpos = le + 1;
			while (newpos < len) {
				if (gap_char_at(buf, newpos) == '\n') break;
				int le2 = line_end(buf, newpos);
				if (le2 >= len) { newpos = len; break; }
				newpos = le2 + 1;
			}
			break;
		}
		}
	}

	if (newpos < pos) { r.from = newpos; r.to = pos; }
	else { r.from = pos; r.to = newpos; }

	if (key == 'j' || key == 'k') r.linewise = 1;

	return r;
}

static struct Range linewise_range(struct GapBuf *buf, struct Range r)
{
	r.from = line_start(buf, r.from);
	r.to = line_end(buf, r.to);
	if (r.to < gap_len(buf)) r.to++;
	r.linewise = 1;
	return r;
}

/* --- findchar (f/t/F/T) ------------------------------------------------- */

/* Find character ch on current line starting from pos.
 * dir: +1 forward, -1 backward.
 * Returns position or -1 if not found. */
static int findchar(struct GapBuf *buf, int pos, int ch, int dir)
{
	int len = gap_len(buf);
	int p = pos + dir;
	while (p >= 0 && p < len) {
		char c = gap_char_at(buf, p);
		if (c == '\n') break;
		if ((unsigned char)c == (unsigned char)ch)
			return p;
		p += dir;
	}
	return -1;
}

/* Execute f/t/F/T and return new position or original pos on failure */
static int do_findchar(struct GapBuf *buf, int pos, int cmd, int ch, int count)
{
	int dir = (cmd == 'f' || cmd == 't') ? +1 : -1;
	int p = pos;
	for (int i = 0; i < count; i++) {
		int found = findchar(buf, p, ch, dir);
		if (found < 0) return pos; /* not found, stay put */
		p = found;
	}
	/* t/T: one position back from the found char */
	if (cmd == 't' && p > pos) p--;
	if (cmd == 'T' && p < pos) p++;
	return p;
}

/* --- bracket matching (%) ------------------------------------------------ */

static int match_bracket(struct GapBuf *buf, int pos)
{
	int len = gap_len(buf);
	char *pairs = "()[]{}";
	char ch;
	char *pp;
	int depth = 1;
	int dir;
	int match_open, match_close;

	/* first, find a bracket char at or after pos on the same line */
	int p = pos;
	while (p < len && gap_char_at(buf, p) != '\n') {
		ch = gap_char_at(buf, p);
		if (strchr(pairs, (unsigned char)ch))
			goto found;
		p++;
	}
	return pos; /* no bracket found */

found:
	pp = strchr(pairs, (unsigned char)ch);
	if (!pp) return pos;
	{
		int idx = pp - pairs;
		if (idx & 1) {
			/* closing bracket: search backward */
			dir = -1;
			match_close = ch;
			match_open = pairs[idx - 1];
		} else {
			/* opening bracket: search forward */
			dir = +1;
			match_open = ch;
			match_close = pairs[idx + 1];
		}
	}

	p += dir;
	while (p >= 0 && p < len) {
		char c = gap_char_at(buf, p);
		if (c == match_open) depth++;
		if (c == match_close) depth--;
		if (depth == 0) return p;
		p += dir;
	}
	return pos; /* no match */
}

/* --- text objects -------------------------------------------------------- */

/* Inner/around word text object. Returns range. */
static struct Range textobj_word(struct GapBuf *buf, int pos, int around)
{
	int len = gap_len(buf);
	struct Range r = { pos, pos, 0 };
	if (pos >= len) return r;

	char c = gap_char_at(buf, pos);
	int is_w = is_word_char(c);
	int is_sp = (c == ' ' || c == '\t');

	/* expand backward while same class */
	r.from = pos;
	if (is_w) {
		while (r.from > 0 && is_word_char(gap_char_at(buf, r.from - 1))) r.from--;
	} else if (is_sp) {
		while (r.from > 0 && (gap_char_at(buf, r.from - 1) == ' ' || gap_char_at(buf, r.from - 1) == '\t')) r.from--;
	} else {
		while (r.from > 0 && !is_word_char(gap_char_at(buf, r.from - 1))
		       && gap_char_at(buf, r.from - 1) != ' '
		       && gap_char_at(buf, r.from - 1) != '\t'
		       && gap_char_at(buf, r.from - 1) != '\n') r.from--;
	}

	/* expand forward */
	r.to = pos;
	if (is_w) {
		while (r.to < len && is_word_char(gap_char_at(buf, r.to))) r.to++;
	} else if (is_sp) {
		while (r.to < len && (gap_char_at(buf, r.to) == ' ' || gap_char_at(buf, r.to) == '\t')) r.to++;
	} else {
		while (r.to < len && !is_word_char(gap_char_at(buf, r.to))
		       && gap_char_at(buf, r.to) != ' '
		       && gap_char_at(buf, r.to) != '\t'
		       && gap_char_at(buf, r.to) != '\n') r.to++;
	}

	/* "around": include trailing whitespace (or leading if at end) */
	if (around) {
		int old_to = r.to;
		while (r.to < len && (gap_char_at(buf, r.to) == ' ' || gap_char_at(buf, r.to) == '\t')) r.to++;
		if (r.to == old_to) {
			/* no trailing space — include leading */
			while (r.from > 0 && (gap_char_at(buf, r.from - 1) == ' ' || gap_char_at(buf, r.from - 1) == '\t')) r.from--;
		}
	}
	return r;
}

/* Inner/around WORD (non-blank) text object */
static struct Range textobj_WORD(struct GapBuf *buf, int pos, int around)
{
	int len = gap_len(buf);
	struct Range r = { pos, pos, 0 };
	if (pos >= len) return r;

	char c = gap_char_at(buf, pos);
	int is_sp = (c == ' ' || c == '\t' || c == '\n');

	r.from = pos;
	r.to = pos;

	if (!is_sp) {
		while (r.from > 0 && gap_char_at(buf, r.from - 1) != ' '
		       && gap_char_at(buf, r.from - 1) != '\t'
		       && gap_char_at(buf, r.from - 1) != '\n') r.from--;
		while (r.to < len && gap_char_at(buf, r.to) != ' '
		       && gap_char_at(buf, r.to) != '\t'
		       && gap_char_at(buf, r.to) != '\n') r.to++;
	} else {
		while (r.from > 0 && (gap_char_at(buf, r.from - 1) == ' ' || gap_char_at(buf, r.from - 1) == '\t')) r.from--;
		while (r.to < len && (gap_char_at(buf, r.to) == ' ' || gap_char_at(buf, r.to) == '\t')) r.to++;
	}

	if (around && !is_sp) {
		int old_to = r.to;
		while (r.to < len && (gap_char_at(buf, r.to) == ' ' || gap_char_at(buf, r.to) == '\t')) r.to++;
		if (r.to == old_to)
			while (r.from > 0 && (gap_char_at(buf, r.from - 1) == ' ' || gap_char_at(buf, r.from - 1) == '\t')) r.from--;
	}
	return r;
}

/* Inner/around delimited pair (quotes, brackets).
 * For quotes: search outward for matching pair.
 * For brackets: find matching pair accounting for nesting. */
static struct Range textobj_delim(struct GapBuf *buf, int pos, int ch, int around)
{
	int len = gap_len(buf);
	struct Range r = { pos, pos, 0 };
	int open_ch, close_ch;

	switch (ch) {
	case '(': case ')': open_ch = '('; close_ch = ')'; break;
	case '{': case '}': open_ch = '{'; close_ch = '}'; break;
	case '[': case ']': open_ch = '['; close_ch = ']'; break;
	case '<': case '>': open_ch = '<'; close_ch = '>'; break;
	case '"':  open_ch = '"';  close_ch = '"';  break;
	case '\'': open_ch = '\''; close_ch = '\''; break;
	case '`':  open_ch = '`';  close_ch = '`';  break;
	default: return r;
	}

	if (open_ch == close_ch) {
		/* quotes: find surrounding pair on same line */
		int start = -1, end = -1;
		int ls = line_start(buf, pos);
		int le = line_end(buf, pos);

		/* search backward for opening quote */
		for (int p = pos; p >= ls; p--) {
			if (gap_char_at(buf, p) == open_ch) {
				/* check if this is an opening quote: count quotes from line start */
				int cnt = 0;
				for (int q = ls; q <= p; q++)
					if (gap_char_at(buf, q) == open_ch) cnt++;
				if (cnt & 1) { start = p; break; }
			}
		}
		if (start < 0) return r;

		/* search forward for closing quote */
		for (int p = start + 1; p < le; p++) {
			if (gap_char_at(buf, p) == close_ch) {
				end = p;
				break;
			}
		}
		if (end < 0) return r;

		if (around) {
			r.from = start;
			r.to = end + 1;
		} else {
			r.from = start + 1;
			r.to = end;
		}
	} else {
		/* brackets: find matching pair with nesting */
		int start = -1, depth;

		/* search backward for opening bracket */
		depth = 0;
		for (int p = pos; p >= 0; p--) {
			char c = gap_char_at(buf, p);
			if (c == close_ch) depth++;
			if (c == open_ch) {
				if (depth == 0) { start = p; break; }
				depth--;
			}
		}
		if (start < 0) return r;

		/* search forward for closing bracket */
		int end = -1;
		depth = 0;
		for (int p = start + 1; p < len; p++) {
			char c = gap_char_at(buf, p);
			if (c == open_ch) depth++;
			if (c == close_ch) {
				if (depth == 0) { end = p; break; }
				depth--;
			}
		}
		if (end < 0) return r;

		if (around) {
			r.from = start;
			r.to = end + 1;
		} else {
			r.from = start + 1;
			r.to = end;
		}
	}
	return r;
}

/* Dispatch text object by type character */
static struct Range textobj_dispatch(struct GapBuf *buf, int pos, int inner_or_around, int obj_ch)
{
	struct Range r = { pos, pos, 0 };
	int around = (inner_or_around == 'a');

	switch (obj_ch) {
	case 'w': return textobj_word(buf, pos, around);
	case 'W': return textobj_WORD(buf, pos, around);
	case '"': case '\'': case '`':
	case '(': case ')': case 'b':
	case '{': case '}': case 'B':
	case '[': case ']':
	case '<': case '>':
		if (obj_ch == 'b') obj_ch = '(';
		if (obj_ch == 'B') obj_ch = '{';
		return textobj_delim(buf, pos, obj_ch, around);
	default: return r;
	}
}

/* --- yank/delete helpers ------------------------------------------------- */

static void yank_range(struct VimState *v, struct GapBuf *buf, int from, int to, int linewise)
{
	int n = to - from;
	if (n <= 0) return;
	free(v->yank.text);
	v->yank.text = malloc(n + 1);
	if (!v->yank.text) { v->yank.len = 0; return; }
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

static void apply_operator(struct VimState *v, struct GapBuf *buf,
			   struct UndoRing *undo, struct Range r)
{
	if (r.linewise) r = linewise_range(buf, r);
	int from = r.from, to = r.to;
	if (from > to) { int t = from; from = to; to = t; }
	if (from == to && v->op != 'y') return;

	switch (v->op) {
	case 'd':
		undo_push(undo, buf);
		yank_range(v, buf, from, to, r.linewise);
		delete_range(buf, from, to);
		gap_move(buf, from < gap_len(buf) ? from : (gap_len(buf) > 0 ? gap_len(buf) - 1 : 0));
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
	case '>':
		undo_push(undo, buf);
		{
			int p = line_start(buf, from);
			while (p <= to && p < gap_len(buf)) {
				gap_move(buf, p);
				gap_insert(buf, '\t');
				to++;
				int le = line_end(buf, p);
				if (le >= gap_len(buf)) break;
				p = le + 1;
			}
			gap_move(buf, from);
		}
		break;
	case '<':
		undo_push(undo, buf);
		{
			int p = line_start(buf, from);
			while (p <= to && p < gap_len(buf)) {
				if (gap_char_at(buf, p) == '\t') {
					gap_move(buf, p + 1);
					gap_delete(buf, 1);
					to--;
				}
				int le = line_end(buf, p);
				if (le >= gap_len(buf)) break;
				p = le + 1;
			}
			gap_move(buf, from);
		}
		break;
	}
}

/* --- search -------------------------------------------------------------- */

static int search_forward(struct GapBuf *buf, const char *needle, int pos)
{
	int nlen = strlen(needle);
	int blen = gap_len(buf);
	if (nlen == 0) return -1;
	for (int i = pos + 1; i <= blen - nlen; i++) {
		int match = 1;
		for (int j = 0; j < nlen; j++) {
			char bc = gap_char_at(buf, i + j);
			if (tolower((unsigned char)bc) != tolower((unsigned char)needle[j])) {
				match = 0; break;
			}
		}
		if (match) return i;
	}
	/* wrap */
	for (int i = 0; i < pos && i <= blen - nlen; i++) {
		int match = 1;
		for (int j = 0; j < nlen; j++) {
			char bc = gap_char_at(buf, i + j);
			if (tolower((unsigned char)bc) != tolower((unsigned char)needle[j])) {
				match = 0; break;
			}
		}
		if (match) return i;
	}
	return -1;
}

static int search_backward(struct GapBuf *buf, const char *needle, int pos)
{
	int nlen = strlen(needle);
	int blen = gap_len(buf);
	if (nlen == 0) return -1;
	for (int i = pos - 1; i >= 0; i--) {
		if (i + nlen > blen) continue;
		int match = 1;
		for (int j = 0; j < nlen; j++) {
			char bc = gap_char_at(buf, i + j);
			if (tolower((unsigned char)bc) != tolower((unsigned char)needle[j])) {
				match = 0; break;
			}
		}
		if (match) return i;
	}
	return -1;
}

/* --- repeat (.) recording ------------------------------------------------ */

static void rec_start(struct VimState *v, int key)
{
	if (v->replaying) return;
	v->recording = 1;
	v->rec_len = 0;
	v->rec_buf[v->rec_len++] = key;
}

static void rec_key(struct VimState *v, int key)
{
	if (!v->recording || v->replaying) return;
	if (v->rec_len < REP_MAX)
		v->rec_buf[v->rec_len++] = key;
}

static void rec_stop(struct VimState *v)
{
	if (!v->recording || v->replaying) return;
	v->recording = 0;
	memcpy(v->rep_buf, v->rec_buf, v->rec_len * sizeof(int));
	v->rep_len = v->rec_len;
}

/* --- init ---------------------------------------------------------------- */

void vim_init(struct VimState *v)
{
	memset(v, 0, sizeof *v);
	v->mode = MODE_NORMAL;
}

/* --- is this key a motion? ----------------------------------------------- */

static int is_motion_key(int key)
{
	switch (key) {
	case 'h': case 'l': case 'j': case 'k':
	case 'w': case 'W': case 'b': case 'B': case 'e': case 'E':
	case '0': case '$': case '^': case 'G':
	case '{': case '}':
	case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
		return 1;
	}
	return 0;
}

static int motion_target(struct GapBuf *buf, int pos, int key, int count)
{
	struct Range r = motion_range(buf, pos, key, count);
	if (key == 'h' || key == 'k' || key == 'b' || key == 'B'
	    || key == '0' || key == '^' || key == '{' || key == KEY_LEFT || key == KEY_UP)
		return r.from;
	if (key == '$') return r.to > 0 ? r.to - 1 : 0;
	return r.to;
}

/* --- main keypress handler ----------------------------------------------- */

void vim_keypress(struct VimState *v, struct GapBuf *buf,
		  struct UndoRing *undo, int key)
{
	int pos, len;

	v->result = VIM_RESULT_NONE;
	pos = buf ? buf->gap_start : 0;
	len = buf ? gap_len(buf) : 0;

	switch (v->mode) {

	/* ================================================================== */
	case MODE_INSERT:
		if (!buf) { v->mode = MODE_NORMAL; break; }

		/* record keys for dot repeat */
		rec_key(v, key);

		switch (key) {
		case KEY_ESC:
			v->mode = MODE_NORMAL;
			if (buf->gap_start > 0 &&
			    gap_char_at(buf, buf->gap_start - 1) != '\n')
				gap_move(buf, buf->gap_start - 1);
			if (undo) undo_push(undo, buf);
			rec_stop(v);
			break;
		case KEY_BACKSPACE:
			if (buf->gap_start > 0) gap_delete(buf, 1);
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
		case KEY_UP:
			gap_move(buf, motion_target(buf, buf->gap_start, 'k', 1));
			break;
		case KEY_DOWN:
			gap_move(buf, motion_target(buf, buf->gap_start, 'j', 1));
			break;
		case KEY_CTRL('w'):
			/* delete word backward */
			{
				int p = buf->gap_start;
				while (p > 0 && gap_char_at(buf, p - 1) == ' ') p--;
				while (p > 0 && is_word_char(gap_char_at(buf, p - 1))) p--;
				if (p < buf->gap_start)
					gap_delete(buf, buf->gap_start - p);
			}
			break;
		case KEY_CTRL('u'):
			/* delete to line start */
			{
				int ls = line_start(buf, buf->gap_start);
				if (ls < buf->gap_start)
					gap_delete(buf, buf->gap_start - ls);
			}
			break;
		default:
			if (key >= 32 && key < 127)
				gap_insert(buf, key);
			break;
		}
		break;

	/* ================================================================== */
	case MODE_NORMAL:
		if (!buf) break;

		/* numeric count */
		if (key >= '1' && key <= '9') { v->count = v->count * 10 + (key - '0'); break; }
		if (key == '0' && v->count > 0) { v->count = v->count * 10; break; }

		{ int cnt = v->count > 0 ? v->count : 1;

		/* --- pending states --- */

		/* g prefix */
		if (v->pending_g) {
			v->pending_g = 0; v->count = 0;
			if (key == 'g') gap_move(buf, 0);
			else if (key == 'e') {
				/* ge: backward word end */
				int p = pos;
				for (int i = 0; i < cnt; i++) {
					if (p > 0) p--;
					while (p > 0 && !is_word_char(gap_char_at(buf, p))) p--;
					while (p > 0 && is_word_char(gap_char_at(buf, p - 1))) p--;
				}
				gap_move(buf, p);
			}
			break;
		}

		/* Z prefix */
		if (v->pending_Z) {
			v->pending_Z = 0; v->count = 0;
			if (key == 'Z') v->result = VIM_RESULT_ZZ;
			else if (key == 'Q') v->result = VIM_RESULT_ZQ;
			break;
		}

		/* mark pending */
		if (v->mark_pending) {
			v->mark_pending = 0;
			if (key >= 'a' && key <= 'z') {
				v->mark_letter = key;
				v->result = (v->mark_action == 'm')
					? VIM_RESULT_SET_MARK : VIM_RESULT_JUMP_MARK;
			}
			v->count = 0;
			break;
		}

		/* f/t/F/T waiting for character */
		if (v->pending_find) {
			int cmd = v->pending_find_cmd;
			v->pending_find = 0;
			if (key >= 32 && key < 127) {
				v->findchar_cmd = cmd;
				v->findchar_ch = key;
				int np = do_findchar(buf, pos, cmd, key, cnt);
				gap_move(buf, np);
			}
			v->count = 0;
			break;
		}

		/* r waiting for replacement character */
		if (v->pending_r) {
			v->pending_r = 0;
			if (key >= 32 && key < 127 && pos < len && gap_char_at(buf, pos) != '\n') {
				/* check enough chars on line for count */
				int avail = 0;
				for (int i = 0; i < cnt && pos + i < len
				     && gap_char_at(buf, pos + i) != '\n'; i++)
					avail++;
				if (avail >= cnt) {
					if (undo) undo_push(undo, buf);
					rec_start(v, 'r');
					rec_key(v, key);
					for (int i = 0; i < cnt; i++) {
						gap_move(buf, pos + i + 1);
						gap_delete(buf, 1);
						gap_insert(buf, key);
					}
					gap_move(buf, pos + cnt - 1);
					rec_stop(v);
				}
			}
			v->count = 0;
			break;
		}

		switch (key) {
		/* --- insert --- */
		case 'i':
			if (undo) undo_push(undo, buf);
			v->mode = MODE_INSERT;
			rec_start(v, 'i');
			break;
		case 'a':
			if (undo) undo_push(undo, buf);
			if (pos < len && gap_char_at(buf, pos) != '\n') gap_move(buf, pos + 1);
			v->mode = MODE_INSERT;
			rec_start(v, 'a');
			break;
		case 'I':
			if (undo) undo_push(undo, buf);
			gap_move(buf, first_nonblank(buf, pos));
			v->mode = MODE_INSERT;
			rec_start(v, 'I');
			break;
		case 'A':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_end(buf, pos));
			v->mode = MODE_INSERT;
			rec_start(v, 'A');
			break;
		case 'o':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_end(buf, pos));
			gap_insert(buf, '\n');
			v->mode = MODE_INSERT;
			rec_start(v, 'o');
			break;
		case 'O':
			if (undo) undo_push(undo, buf);
			gap_move(buf, line_start(buf, pos));
			gap_insert(buf, '\n');
			gap_move(buf, buf->gap_start - 1);
			v->mode = MODE_INSERT;
			rec_start(v, 'O');
			break;

		/* --- s/S (substitute) --- */
		case 's':
			if (pos < len && gap_char_at(buf, pos) != '\n') {
				if (undo) undo_push(undo, buf);
				for (int i = 0; i < cnt && pos < gap_len(buf) && gap_char_at(buf, pos) != '\n'; i++)
					gap_delete_fwd(buf, 1);
				v->mode = MODE_INSERT;
				rec_start(v, 's');
			}
			break;
		case 'S':
			if (undo) undo_push(undo, buf);
			{
				int ls = line_start(buf, pos);
				int le = line_end(buf, pos);
				gap_move(buf, le);
				gap_delete(buf, le - ls);
				gap_move(buf, ls);
			}
			v->mode = MODE_INSERT;
			rec_start(v, 'S');
			break;

		/* --- C/D/Y shortcuts --- */
		case 'C':
			if (undo) undo_push(undo, buf);
			{
				int le = line_end(buf, pos);
				if (le > pos) {
					yank_range(v, buf, pos, le, 0);
					delete_range(buf, pos, le);
				}
			}
			v->mode = MODE_INSERT;
			rec_start(v, 'C');
			break;
		case 'D':
			{
				int le = line_end(buf, pos);
				if (le > pos) {
					if (undo) undo_push(undo, buf);
					yank_range(v, buf, pos, le, 0);
					delete_range(buf, pos, le);
					/* back up if past eol */
					if (pos > 0 && pos >= gap_len(buf))
						gap_move(buf, pos - 1);
				}
			}
			v->count = 0;
			break;
		case 'Y':
			/* yank whole line */
			{
				int ls = line_start(buf, pos);
				int le = line_end(buf, pos);
				if (le < len) le++;
				yank_range(v, buf, ls, le, 1);
			}
			v->count = 0;
			break;

		/* --- motions --- */
		case 'h': case 'l': case 'j': case 'k':
		case 'w': case 'W': case 'b': case 'B': case 'e': case 'E':
		case '0': case '$': case '^': case 'G':
		case '{': case '}':
		case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
			gap_move(buf, motion_target(buf, pos, key, cnt));
			break;

		/* --- f/t/F/T findchar --- */
		case 'f': case 't': case 'F': case 'T':
			v->pending_find = 1;
			v->pending_find_cmd = key;
			break;
		case ';':
			if (v->findchar_cmd) {
				int np = do_findchar(buf, pos, v->findchar_cmd, v->findchar_ch, cnt);
				gap_move(buf, np);
			}
			break;
		case ',':
			if (v->findchar_cmd) {
				/* reverse direction */
				int rev;
				switch (v->findchar_cmd) {
				case 'f': rev = 'F'; break;
				case 'F': rev = 'f'; break;
				case 't': rev = 'T'; break;
				case 'T': rev = 't'; break;
				default: rev = v->findchar_cmd;
				}
				int np = do_findchar(buf, pos, rev, v->findchar_ch, cnt);
				gap_move(buf, np);
			}
			break;

		/* --- % bracket matching --- */
		case '%':
			gap_move(buf, match_bracket(buf, pos));
			break;

		/* --- g prefix --- */
		case 'g': v->pending_g = 1; break;

		/* --- operators --- */
		case 'd': case 'y': case 'c': case '>': case '<':
			v->op = key;
			v->mode = MODE_OP_PENDING;
			v->rep_count = v->count;
			break;

		/* --- x / X --- */
		case 'x':
			if (pos < len) {
				if (undo) undo_push(undo, buf);
				rec_start(v, 'x');
				for (int i = 0; i < cnt && pos < gap_len(buf); i++) {
					yank_range(v, buf, pos, pos + 1, 0);
					gap_delete_fwd(buf, 1);
				}
				/* back up if past end */
				if (buf->gap_start > 0 && buf->gap_start >= gap_len(buf))
					gap_move(buf, buf->gap_start - 1);
				rec_stop(v);
			}
			break;
		case 'X':
			if (pos > 0) {
				if (undo) undo_push(undo, buf);
				rec_start(v, 'X');
				for (int i = 0; i < cnt && buf->gap_start > line_start(buf, buf->gap_start); i++) {
					yank_range(v, buf, buf->gap_start - 1, buf->gap_start, 0);
					gap_delete(buf, 1);
				}
				rec_stop(v);
			}
			break;

		/* --- r (replace character) --- */
		case 'r':
			v->pending_r = 1;
			break;

		/* --- paste --- */
		case 'p':
			if (v->yank.text) {
				if (undo) undo_push(undo, buf);
				rec_start(v, 'p');
				if (v->yank.linewise) {
					int le = line_end(buf, pos);
					gap_move(buf, le < len ? le + 1 : le);
					if (le >= len) gap_insert(buf, '\n');
				} else {
					if (pos < len) gap_move(buf, pos + 1);
				}
				for (int i = 0; i < cnt; i++)
					gap_insert_str(buf, v->yank.text, v->yank.len);
				gap_move(buf, buf->gap_start - 1);
				rec_stop(v);
			}
			break;
		case 'P':
			if (v->yank.text) {
				if (undo) undo_push(undo, buf);
				rec_start(v, 'P');
				if (v->yank.linewise) gap_move(buf, line_start(buf, pos));
				for (int i = 0; i < cnt; i++)
					gap_insert_str(buf, v->yank.text, v->yank.len);
				gap_move(buf, buf->gap_start - v->yank.len);
				rec_stop(v);
			}
			break;

		/* --- J (join) --- */
		case 'J': {
			int le = line_end(buf, pos);
			if (le < len) {
				if (undo) undo_push(undo, buf);
				rec_start(v, 'J');
				gap_move(buf, le);
				gap_delete_fwd(buf, 1);
				while (buf->gap_end < buf->cap && buf->buf[buf->gap_end] == ' ')
					gap_delete_fwd(buf, 1);
				gap_insert(buf, ' ');
				rec_stop(v);
			}
			break;
		}

		/* --- ~ (toggle case) --- */
		case '~':
			if (pos < len && gap_char_at(buf, pos) != '\n') {
				if (undo) undo_push(undo, buf);
				rec_start(v, '~');
				for (int i = 0; i < cnt && pos + i < gap_len(buf)
				     && gap_char_at(buf, pos + i) != '\n'; i++) {
					char c = gap_char_at(buf, pos + i);
					if (isalpha((unsigned char)c)) {
						gap_move(buf, pos + i + 1);
						gap_delete(buf, 1);
						gap_insert(buf, isupper((unsigned char)c) ?
							tolower((unsigned char)c) : toupper((unsigned char)c));
					}
				}
				/* advance cursor past toggled chars */
				{
					int end = pos + cnt;
					if (end > gap_len(buf)) end = gap_len(buf);
					gap_move(buf, end < gap_len(buf) ? end : (gap_len(buf) > 0 ? gap_len(buf) - 1 : 0));
				}
				rec_stop(v);
			}
			break;

		/* --- undo/redo --- */
		case 'u':
			if (undo) undo_undo(undo, buf);
			break;
		case KEY_CTRL('r'):
			if (undo) undo_redo(undo, buf);
			break;

		/* --- visual mode --- */
		case 'v':
			v->mode = MODE_VISUAL;
			v->visual_start = pos;
			break;
		case 'V':
			v->mode = MODE_VLINE;
			v->visual_start = pos;
			break;

		/* --- search --- */
		case '/':
			v->search_active = 1;
			v->search_len = 0;
			v->search_buf[0] = '\0';
			break;
		case 'n':
			if (v->search_buf[0]) {
				int found = search_forward(buf, v->search_buf, pos);
				if (found >= 0) gap_move(buf, found);
			}
			break;
		case 'N':
			if (v->search_buf[0]) {
				int found = search_backward(buf, v->search_buf, pos);
				if (found >= 0) gap_move(buf, found);
			}
			break;

		/* --- . (dot repeat) --- */
		case '.':
			if (v->rep_len > 0) {
				v->replaying = 1;
				int rep_cnt = cnt;
				for (int r = 0; r < rep_cnt; r++) {
					for (int i = 0; i < v->rep_len; i++)
						vim_keypress(v, buf, undo, v->rep_buf[i]);
				}
				v->replaying = 0;
			}
			break;

		/* --- marks --- */
		case 'm':
			v->mark_pending = 1;
			v->mark_action = 'm';
			break;
		case '\'':
			v->mark_pending = 1;
			v->mark_action = '\'';
			break;

		/* --- : command --- */
		case ':':
			v->mode = MODE_COMMAND;
			v->cmd_len = 0;
			v->cmd_buf[0] = '\0';
			break;

		/* --- ZZ / ZQ --- */
		case 'Z':
			v->pending_Z = 1;
			break;

		default:
			break;
		}

		v->count = 0;
		} /* end cnt scope */
		break;

	/* ================================================================== */
	case MODE_VISUAL:
	case MODE_VLINE:
		if (!buf) { v->mode = MODE_NORMAL; break; }

		if (key == KEY_ESC) {
			v->mode = MODE_NORMAL;
			break;
		}

		/* numeric count */
		if (key >= '1' && key <= '9') { v->count = v->count * 10 + (key - '0'); break; }
		if (key == '0' && v->count > 0) { v->count = v->count * 10; break; }

		/* motions extend selection */
		if (is_motion_key(key)) {
			int cnt = v->count > 0 ? v->count : 1;
			gap_move(buf, motion_target(buf, pos, key, cnt));
			v->count = 0;
			break;
		}

		/* f/t/F/T in visual mode */
		if (key == 'f' || key == 't' || key == 'F' || key == 'T') {
			v->pending_find = 1;
			v->pending_find_cmd = key;
			break;
		}
		if (v->pending_find) {
			int cmd = v->pending_find_cmd;
			v->pending_find = 0;
			if (key >= 32 && key < 127) {
				v->findchar_cmd = cmd;
				v->findchar_ch = key;
				int cnt = v->count > 0 ? v->count : 1;
				int np = do_findchar(buf, pos, cmd, key, cnt);
				gap_move(buf, np);
			}
			v->count = 0;
			break;
		}

		/* ; and , in visual */
		if (key == ';' && v->findchar_cmd) {
			int cnt = v->count > 0 ? v->count : 1;
			gap_move(buf, do_findchar(buf, pos, v->findchar_cmd, v->findchar_ch, cnt));
			v->count = 0;
			break;
		}
		if (key == ',' && v->findchar_cmd) {
			int rev;
			switch (v->findchar_cmd) {
			case 'f': rev = 'F'; break;
			case 'F': rev = 'f'; break;
			case 't': rev = 'T'; break;
			case 'T': rev = 't'; break;
			default: rev = v->findchar_cmd;
			}
			int cnt = v->count > 0 ? v->count : 1;
			gap_move(buf, do_findchar(buf, pos, rev, v->findchar_ch, cnt));
			v->count = 0;
			break;
		}

		/* % in visual */
		if (key == '%') {
			gap_move(buf, match_bracket(buf, pos));
			break;
		}

		/* switch visual sub-modes */
		if (key == 'v' && v->mode == MODE_VISUAL) { v->mode = MODE_NORMAL; break; }
		if (key == 'V' && v->mode == MODE_VLINE)  { v->mode = MODE_NORMAL; break; }
		if (key == 'v' && v->mode == MODE_VLINE)  { v->mode = MODE_VISUAL; break; }
		if (key == 'V' && v->mode == MODE_VISUAL)  { v->mode = MODE_VLINE; break; }

		/* o — swap selection end */
		if (key == 'o') {
			int tmp = v->visual_start;
			v->visual_start = buf->gap_start;
			gap_move(buf, tmp);
			break;
		}

		/* operators on visual range */
		if (key == 'd' || key == 'y' || key == 'c' || key == '>' || key == '<') {
			int from = v->visual_start;
			int to = buf->gap_start;
			if (from > to) { int t = from; from = to; to = t; }
			if (to < len) to++; /* visual is inclusive */

			struct Range r = { from, to, v->mode == MODE_VLINE };
			v->op = key;
			apply_operator(v, buf, undo, r);
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* ~ in visual: toggle case */
		if (key == '~') {
			int from = v->visual_start;
			int to = buf->gap_start;
			if (from > to) { int t = from; from = to; to = t; }
			if (to < len) to++;
			if (undo) undo_push(undo, buf);
			for (int i = from; i < to; i++) {
				char c = gap_char_at(buf, i);
				if (isalpha((unsigned char)c)) {
					gap_move(buf, i + 1);
					gap_delete(buf, 1);
					gap_insert(buf, isupper((unsigned char)c) ?
						tolower((unsigned char)c) : toupper((unsigned char)c));
				}
			}
			v->mode = MODE_NORMAL;
			gap_move(buf, from);
			break;
		}

		/* U/u in visual: case change */
		if (key == 'U' || key == 'u') {
			int from = v->visual_start;
			int to = buf->gap_start;
			if (from > to) { int t = from; from = to; to = t; }
			if (to < len) to++;
			if (undo) undo_push(undo, buf);
			for (int i = from; i < to; i++) {
				char c = gap_char_at(buf, i);
				if (isalpha((unsigned char)c)) {
					char nc = (key == 'U') ? toupper((unsigned char)c) : tolower((unsigned char)c);
					gap_move(buf, i + 1);
					gap_delete(buf, 1);
					gap_insert(buf, nc);
				}
			}
			v->mode = MODE_NORMAL;
			gap_move(buf, from);
			break;
		}

		v->count = 0;
		break;

	/* ================================================================== */
	case MODE_OP_PENDING:
		if (!buf) { v->mode = MODE_NORMAL; v->op = 0; break; }

		if (key == KEY_ESC) {
			v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			v->pending_obj = 0;
			break;
		}

		/* text objects: i/a prefix */
		if (v->pending_obj) {
			int obj_type = v->pending_obj;
			v->pending_obj = 0;
			struct Range r = textobj_dispatch(buf, pos, obj_type, key);
			if (r.from != r.to) {
				rec_start(v, v->op);
				rec_key(v, obj_type);
				rec_key(v, key);
				apply_operator(v, buf, undo, r);
				/* if operator entered INSERT (c), don't stop recording —
				 * INSERT mode Esc will call rec_stop with typed text */
				if (v->mode != MODE_INSERT)
					rec_stop(v);
			}
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		if (key == 'i' || key == 'a') {
			v->pending_obj = key;
			break;
		}

		/* dd / yy / cc / >> / << */
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
			rec_start(v, v->op);
			rec_key(v, key);
			apply_operator(v, buf, undo, r);
			if (v->mode != MODE_INSERT)
				rec_stop(v);
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* count between op and motion */
		if (key >= '1' && key <= '9') { v->count = v->count * 10 + (key - '0'); break; }
		if (key == '0' && v->count > 0) { v->count = v->count * 10; break; }

		/* f/t/F/T as motion in op pending */
		if (key == 'f' || key == 't' || key == 'F' || key == 'T') {
			v->pending_find = 1;
			v->pending_find_cmd = key;
			break;
		}
		if (v->pending_find) {
			int cmd = v->pending_find_cmd;
			v->pending_find = 0;
			if (key >= 32 && key < 127) {
				int cnt = v->count > 0 ? v->count : 1;
				v->findchar_cmd = cmd;
				v->findchar_ch = key;
				int np = do_findchar(buf, pos, cmd, key, cnt);
				struct Range r = { pos, np, 0 };
				if (r.from > r.to) { int t = r.from; r.from = r.to; r.to = t; }
				if (r.to < len) r.to++; /* inclusive */
				rec_start(v, v->op);
				rec_key(v, cmd);
				rec_key(v, key);
				apply_operator(v, buf, undo, r);
				if (v->mode != MODE_INSERT)
					rec_stop(v);
			}
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* % as motion in op pending */
		if (key == '%') {
			int np = match_bracket(buf, pos);
			if (np != pos) {
				struct Range r = { pos, np, 0 };
				if (r.from > r.to) { int t = r.from; r.from = r.to; r.to = t; }
				if (r.to < len) r.to++;
				rec_start(v, v->op);
				rec_key(v, '%');
				apply_operator(v, buf, undo, r);
				if (v->mode != MODE_INSERT)
					rec_stop(v);
			}
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
			v->op = 0;
			v->count = 0;
			break;
		}

		/* regular motion */
		if (is_motion_key(key)) {
			int cnt = v->count > 0 ? v->count : 1;
			struct Range r = motion_range(buf, pos, key, cnt);
			if (key == 'w' || key == 'W' || key == 'e' || key == 'E' || key == '$') {
				if (r.to < len) r.to++;
			}
			rec_start(v, v->op);
			rec_key(v, key);
			apply_operator(v, buf, undo, r);
			if (v->mode != MODE_INSERT)
				rec_stop(v);
			if (v->mode != MODE_INSERT) v->mode = MODE_NORMAL;
		} else {
			v->mode = MODE_NORMAL;
		}
		v->op = 0;
		v->count = 0;
		break;

	/* ================================================================== */
	case MODE_COMMAND:
		switch (key) {
		case KEY_ESC:
			v->mode = MODE_NORMAL;
			v->search_active = 0;
			break;
		case KEY_BACKSPACE:
			if (v->search_active) {
				if (v->search_len > 0) v->search_buf[--v->search_len] = '\0';
				else { v->search_active = 0; v->mode = MODE_NORMAL; }
			} else {
				if (v->cmd_len > 0) v->cmd_buf[--v->cmd_len] = '\0';
				else v->mode = MODE_NORMAL;
			}
			break;
		case '\r': case '\n':
			if (v->search_active) {
				v->search_active = 0;
				v->mode = MODE_NORMAL;
				if (buf && v->search_buf[0]) {
					int found = search_forward(buf, v->search_buf, pos);
					if (found >= 0) gap_move(buf, found);
				}
			} else {
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
				else if (strncmp(v->cmd_buf, "set ", 4) == 0)
					v->result = VIM_RESULT_SET_FIELD;
				else if (strncmp(v->cmd_buf, "tag ", 4) == 0)
					v->result = VIM_RESULT_SET_TAG;
				v->mode = MODE_NORMAL;
			}
			break;
		default:
			if (key >= 32 && key < 127) {
				if (v->search_active) {
					if (v->search_len < SEARCH_MAX - 1)
						v->search_buf[v->search_len++] = key;
					v->search_buf[v->search_len] = '\0';
				} else {
					if (v->cmd_len < (int)sizeof(v->cmd_buf) - 1)
						v->cmd_buf[v->cmd_len++] = key;
				}
			}
			break;
		}
		break;

	default:
		break;
	}

	/* search mode is entered from normal '/' — switch to COMMAND for input */
	if (v->search_active && v->mode == MODE_NORMAL) {
		v->mode = MODE_COMMAND;
	}
}
