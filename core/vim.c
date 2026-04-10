/* onemark — vim modal editing engine
 *
 * Minimal but functional: NORMAL, INSERT, COMMAND modes.
 * Operator-pending (d/y/c + motion) deferred to phase 2.
 */
#include <string.h>
#include <ctype.h>
#include "om.h"

void vim_init(struct VimState *v)
{
	v->mode = MODE_NORMAL;
	v->op = 0;
	v->count = 0;
	v->count2 = 0;
	v->cmd_len = 0;
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

/* --- normal mode --------------------------------------------------------- */

static int normal_motion(struct GapBuf *buf, int pos, int key)
{
	int len = gap_len(buf);

	switch (key) {
	case 'h':
	case KEY_LEFT:
		if (pos > line_start(buf, pos)) pos--;
		break;
	case 'l':
	case KEY_RIGHT:
		if (pos < line_end(buf, pos) - 1 && pos < len - 1) pos++;
		break;
	case 'j':
	case KEY_DOWN: {
		int ls = line_start(buf, pos);
		int col = pos - ls;
		int le = line_end(buf, pos);
		if (le < len) {
			int next_ls = le + 1;
			int next_le = line_end(buf, next_ls);
			int next_len = next_le - next_ls;
			pos = next_ls + (col < next_len ? col : (next_len > 0 ? next_len - 1 : 0));
		}
		break;
	}
	case 'k':
	case KEY_UP: {
		int ls = line_start(buf, pos);
		int col = pos - ls;
		if (ls > 0) {
			int prev_le = ls - 1;
			int prev_ls = line_start(buf, prev_le);
			int prev_len = prev_le - prev_ls;
			pos = prev_ls + (col < prev_len ? col : (prev_len > 0 ? prev_len - 1 : 0));
		}
		break;
	}
	case '0':
		pos = line_start(buf, pos);
		break;
	case '$':
		pos = line_end(buf, pos);
		if (pos > 0 && gap_char_at(buf, pos) == '\n') pos--;
		if (pos < line_start(buf, pos)) pos = line_start(buf, pos);
		break;
	case '^': {
		int ls = line_start(buf, pos);
		pos = ls;
		while (pos < len && gap_char_at(buf, pos) == ' ')
			pos++;
		break;
	}
	case 'w': {
		/* next word start */
		if (pos < len && is_word_char(gap_char_at(buf, pos))) {
			while (pos < len && is_word_char(gap_char_at(buf, pos))) pos++;
		} else {
			while (pos < len && !is_word_char(gap_char_at(buf, pos)) && gap_char_at(buf, pos) != '\n') pos++;
		}
		while (pos < len && (gap_char_at(buf, pos) == ' ' || gap_char_at(buf, pos) == '\n')) pos++;
		break;
	}
	case 'b': {
		/* prev word start */
		if (pos > 0) pos--;
		while (pos > 0 && !is_word_char(gap_char_at(buf, pos))) pos--;
		while (pos > 0 && is_word_char(gap_char_at(buf, pos - 1))) pos--;
		break;
	}
	case 'e': {
		/* end of word */
		if (pos < len - 1) pos++;
		while (pos < len && !is_word_char(gap_char_at(buf, pos))) pos++;
		while (pos < len - 1 && is_word_char(gap_char_at(buf, pos + 1))) pos++;
		break;
	}
	case 'G':
		/* end of buffer */
		pos = len > 0 ? len - 1 : 0;
		break;
	}
	return pos;
}

/* Result codes from vim_keypress, communicated via VimState */
#define VIM_RESULT_NONE    0
#define VIM_RESULT_SAVE    1
#define VIM_RESULT_QUIT    2
#define VIM_RESULT_SAVEQUIT 3
#define VIM_RESULT_NEWBOX  4

void vim_keypress(struct VimState *v, struct GapBuf *buf, int key)
{
	int pos, len;

	v->result = VIM_RESULT_NONE;
	pos = buf->gap_start;
	len = gap_len(buf);

	switch (v->mode) {

	/* ================================================================== */
	case MODE_INSERT:
		switch (key) {
		case KEY_ESC:
			v->mode = MODE_NORMAL;
			/* nudge cursor left (vim convention) */
			if (buf->gap_start > 0 &&
			    gap_char_at(buf, buf->gap_start - 1) != '\n')
				gap_move(buf, buf->gap_start - 1);
			break;
		case KEY_BACKSPACE:
			if (buf->gap_start > 0)
				gap_delete(buf, 1);
			break;
		case KEY_DEL:
			gap_delete_fwd(buf, 1);
			break;
		case '\r':
		case '\n':
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
			int np = normal_motion(buf, buf->gap_start, 'k');
			gap_move(buf, np);
			break;
		}
		case KEY_DOWN: {
			int np = normal_motion(buf, buf->gap_start, 'j');
			gap_move(buf, np);
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
		switch (key) {
		/* enter insert */
		case 'i':
			v->mode = MODE_INSERT;
			break;
		case 'a':
			if (pos < len && gap_char_at(buf, pos) != '\n')
				gap_move(buf, pos + 1);
			v->mode = MODE_INSERT;
			break;
		case 'I':
			gap_move(buf, line_start(buf, pos));
			v->mode = MODE_INSERT;
			break;
		case 'A':
			gap_move(buf, line_end(buf, pos));
			v->mode = MODE_INSERT;
			break;
		case 'o':
			gap_move(buf, line_end(buf, pos));
			gap_insert(buf, '\n');
			v->mode = MODE_INSERT;
			break;
		case 'O':
			gap_move(buf, line_start(buf, pos));
			gap_insert(buf, '\n');
			gap_move(buf, buf->gap_start - 1);
			v->mode = MODE_INSERT;
			break;

		/* motions */
		case 'h': case 'l': case 'j': case 'k':
		case '0': case '$': case '^':
		case 'w': case 'b': case 'e':
		case 'G':
		case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
			gap_move(buf, normal_motion(buf, pos, key));
			break;

		/* gg — go to start */
		case 'g':
			/* simplified: single g goes to top */
			gap_move(buf, 0);
			break;

		/* x — delete char under cursor */
		case 'x':
			if (pos < len)
				gap_delete_fwd(buf, 1);
			break;

		/* X — delete char before cursor */
		case 'X':
			if (pos > 0)
				gap_delete(buf, 1);
			break;

		/* dd — delete line (simplified: single d deletes line) */
		case 'd': {
			int ls = line_start(buf, pos);
			int le = line_end(buf, pos);
			/* include the newline */
			if (le < gap_len(buf)) le++;
			gap_move(buf, ls);
			gap_delete_fwd(buf, le - ls);
			break;
		}

		/* J — join lines */
		case 'J': {
			int le = line_end(buf, pos);
			if (le < gap_len(buf)) {
				gap_move(buf, le);
				gap_delete_fwd(buf, 1);
				/* collapse leading whitespace */
				while (buf->gap_end < buf->cap &&
				       buf->buf[buf->gap_end] == ' ')
					gap_delete_fwd(buf, 1);
				gap_insert(buf, ' ');
			}
			break;
		}

		/* p — paste (TODO: needs yank register) */

		/* : — enter command mode */
		case ':':
			v->mode = MODE_COMMAND;
			v->cmd_len = 0;
			v->cmd_buf[0] = '\0';
			break;

		default:
			break;
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
		case '\r':
		case '\n':
			/* execute command */
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
