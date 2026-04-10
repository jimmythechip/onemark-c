/* onemark — text operations: bold, italic, surround, checkbox, heading
 *
 * Pure functions on gap buffers. Each returns the new cursor position.
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "om.h"

/* --- word boundary ------------------------------------------------------- */

static int is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void word_boundary(struct GapBuf *buf, int pos, int *from, int *to)
{
	int len = gap_len(buf);
	int f = pos, t = pos;
	while (f > 0 && is_word(gap_char_at(buf, f - 1))) f--;
	while (t < len && is_word(gap_char_at(buf, t))) t++;
	*from = f;
	*to = t;
}

/* --- toggle wrap (bold / italic / code) ---------------------------------- */

int edit_toggle_wrap(struct GapBuf *buf, struct UndoRing *undo,
		     int from, int to, const char *marker)
{
	int mlen = strlen(marker);
	int len = gap_len(buf);

	/* if no selection, expand to word */
	if (from == to) {
		word_boundary(buf, from, &from, &to);
		if (from == to) {
			/* no word: insert marker pair, cursor between */
			undo_push(undo, buf);
			gap_move(buf, from);
			gap_insert_str(buf, marker, mlen);
			gap_insert_str(buf, marker, mlen);
			gap_move(buf, from + mlen);
			return from + mlen;
		}
	}

	/* check if already wrapped (outside) */
	if (from >= mlen && from + (to - from) + mlen <= len) {
		int wrapped = 1;
		for (int i = 0; i < mlen; i++) {
			if (gap_char_at(buf, from - mlen + i) != marker[i]) { wrapped = 0; break; }
			if (gap_char_at(buf, to + i) != marker[i]) { wrapped = 0; break; }
		}
		if (wrapped) {
			/* unwrap */
			undo_push(undo, buf);
			gap_move(buf, to + mlen);
			gap_delete(buf, mlen);
			gap_move(buf, from);
			gap_delete(buf, mlen);
			return from - mlen;
		}
	}

	/* check if wrapped inside selection */
	if (to - from >= 2 * mlen) {
		int wrapped = 1;
		for (int i = 0; i < mlen; i++) {
			if (gap_char_at(buf, from + i) != marker[i]) { wrapped = 0; break; }
			if (gap_char_at(buf, to - mlen + i) != marker[i]) { wrapped = 0; break; }
		}
		if (wrapped) {
			undo_push(undo, buf);
			gap_move(buf, to);
			gap_delete(buf, mlen);
			gap_move(buf, from + mlen);
			gap_delete(buf, mlen);
			return from;
		}
	}

	/* wrap */
	undo_push(undo, buf);
	gap_move(buf, to);
	gap_insert_str(buf, marker, mlen);
	gap_move(buf, from);
	gap_insert_str(buf, marker, mlen);
	return from + mlen;
}

/* --- checkbox rotation --------------------------------------------------- */

int edit_checkbox_rotate(struct GapBuf *buf, struct UndoRing *undo, int pos)
{
	int len = gap_len(buf);
	/* find line start and content */
	int ls = pos;
	while (ls > 0 && gap_char_at(buf, ls - 1) != '\n') ls--;
	int le = pos;
	while (le < len && gap_char_at(buf, le) != '\n') le++;

	/* extract line */
	int ll = le - ls;
	char *line = malloc(ll + 1);
	for (int i = 0; i < ll; i++) line[i] = gap_char_at(buf, ls + i);
	line[ll] = '\0';

	/* determine state and transform */
	char *newline = NULL;
	int indent = 0;
	while (indent < ll && line[indent] == ' ') indent++;

	char *rest = line + indent;
	int rlen = ll - indent;

	if (rlen >= 6 && strncmp(rest, "- [x] ", 6) == 0) {
		/* checked → plain */
		newline = malloc(indent + rlen - 6 + 1);
		memcpy(newline, line, indent);
		strcpy(newline + indent, rest + 6);
	} else if (rlen >= 6 && strncmp(rest, "- [ ] ", 6) == 0) {
		/* unchecked → checked */
		newline = malloc(ll + 1);
		memcpy(newline, line, indent);
		memcpy(newline + indent, "- [x] ", 6);
		strcpy(newline + indent + 6, rest + 6);
	} else if (rlen >= 2 && rest[0] == '-' && rest[1] == ' ') {
		/* bullet → unchecked */
		newline = malloc(indent + 6 + (rlen - 2) + 1);
		memcpy(newline, line, indent);
		memcpy(newline + indent, "- [ ] ", 6);
		strcpy(newline + indent + 6, rest + 2);
	} else {
		/* plain → unchecked */
		newline = malloc(indent + 6 + rlen + 1);
		memcpy(newline, line, indent);
		memcpy(newline + indent, "- [ ] ", 6);
		strcpy(newline + indent + 6, rest);
	}

	undo_push(undo, buf);
	int newlen = strlen(newline);
	gap_move(buf, le);
	gap_delete(buf, ll);
	gap_insert_str(buf, newline, newlen);
	int newpos = ls + (pos - ls < newlen ? pos - ls : newlen);

	free(line);
	free(newline);
	return newpos;
}

/* --- surround ------------------------------------------------------------ */

struct SurrPair { const char *open; const char *close; };

static struct SurrPair resolve_pair(char ch)
{
	struct SurrPair p = { NULL, NULL };
	switch (ch) {
	case 'b': case '*': p.open = "**"; p.close = "**"; break;
	case 'i':           p.open = "*";  p.close = "*";  break;
	case '_':           p.open = "_";  p.close = "_";  break;
	case 'c': case '`': p.open = "`";  p.close = "`";  break;
	case '"':           p.open = "\""; p.close = "\""; break;
	case '\'':          p.open = "'";  p.close = "'";  break;
	case '(': case ')': p.open = "(";  p.close = ")";  break;
	case '[': case ']': p.open = "[";  p.close = "]";  break;
	case '{': case '}': p.open = "{";  p.close = "}";  break;
	case '<': case '>': p.open = "<";  p.close = ">";  break;
	}
	return p;
}

int edit_surround_add(struct GapBuf *buf, struct UndoRing *undo,
		      int from, int to, char ch)
{
	struct SurrPair p = resolve_pair(ch);
	if (!p.open) return from;
	int olen = strlen(p.open);
	int clen = strlen(p.close);

	/* if no selection, expand to word */
	if (from == to) {
		word_boundary(buf, from, &from, &to);
		if (from == to) return from;
	}

	undo_push(undo, buf);
	gap_move(buf, to);
	gap_insert_str(buf, p.close, clen);
	gap_move(buf, from);
	gap_insert_str(buf, p.open, olen);
	return from + olen;
}

int edit_surround_delete(struct GapBuf *buf, struct UndoRing *undo,
			 int pos, char ch)
{
	struct SurrPair p = resolve_pair(ch);
	if (!p.open) return pos;
	int olen = strlen(p.open);
	int clen = strlen(p.close);
	int len = gap_len(buf);

	/* search backward for opener */
	int ostart = -1;
	for (int i = pos - 1; i >= 0; i--) {
		int match = 1;
		for (int j = 0; j < olen; j++) {
			if (i + j >= len || gap_char_at(buf, i + j) != p.open[j])
				{ match = 0; break; }
		}
		if (match) { ostart = i; break; }
	}
	if (ostart < 0) return pos;

	/* search forward for closer */
	int sfrom = (ostart + olen > pos) ? ostart + olen : pos;
	int cstart = -1;
	for (int i = sfrom; i <= len - clen; i++) {
		int match = 1;
		for (int j = 0; j < clen; j++) {
			if (gap_char_at(buf, i + j) != p.close[j])
				{ match = 0; break; }
		}
		if (match) { cstart = i; break; }
	}
	if (cstart < 0) return pos;

	undo_push(undo, buf);
	gap_move(buf, cstart + clen);
	gap_delete(buf, clen);
	gap_move(buf, ostart + olen);
	gap_delete(buf, olen);
	return ostart;
}

/* --- heading navigation -------------------------------------------------- */

int edit_next_heading(struct GapBuf *buf, int pos)
{
	int len = gap_len(buf);
	int le = pos;
	while (le < len && gap_char_at(buf, le) != '\n') le++;
	if (le < len) le++;
	while (le < len) {
		if (gap_char_at(buf, le) == '#') return le;
		while (le < len && gap_char_at(buf, le) != '\n') le++;
		if (le < len) le++;
	}
	return pos;
}

int edit_prev_heading(struct GapBuf *buf, int pos)
{
	int ls = pos;
	while (ls > 0 && gap_char_at(buf, ls - 1) != '\n') ls--;
	if (ls > 0) ls--;
	while (ls > 0) {
		int lls = ls;
		while (lls > 0 && gap_char_at(buf, lls - 1) != '\n') lls--;
		if (gap_char_at(buf, lls) == '#') return lls;
		if (lls == 0) break;
		ls = lls - 1;
	}
	if (gap_char_at(buf, 0) == '#') return 0;
	return pos;
}

int edit_next_list_item(struct GapBuf *buf, int pos)
{
	int len = gap_len(buf);
	int le = pos;
	while (le < len && gap_char_at(buf, le) != '\n') le++;
	if (le < len) le++;
	while (le < len) {
		char c = gap_char_at(buf, le);
		/* skip leading spaces */
		int p = le;
		while (p < len && gap_char_at(buf, p) == ' ') p++;
		if (p < len && (gap_char_at(buf, p) == '-' || gap_char_at(buf, p) == '*'
		    || gap_char_at(buf, p) == '+'))
			return le;
		if (p < len && isdigit((unsigned char)gap_char_at(buf, p)))
			return le;
		while (le < len && gap_char_at(buf, le) != '\n') le++;
		if (le < len) le++;
		(void)c;
	}
	return pos;
}

int edit_prev_list_item(struct GapBuf *buf, int pos)
{
	int ls = pos;
	while (ls > 0 && gap_char_at(buf, ls - 1) != '\n') ls--;
	if (ls > 0) ls--;
	while (ls > 0) {
		int lls = ls;
		while (lls > 0 && gap_char_at(buf, lls - 1) != '\n') lls--;
		int p = lls;
		int len = gap_len(buf);
		while (p < len && gap_char_at(buf, p) == ' ') p++;
		if (p < len && (gap_char_at(buf, p) == '-' || gap_char_at(buf, p) == '*'
		    || gap_char_at(buf, p) == '+'))
			return lls;
		if (lls == 0) break;
		ls = lls - 1;
	}
	return pos;
}
