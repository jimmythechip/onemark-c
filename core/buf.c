/* onemark — gap buffer implementation */
#include <stdlib.h>
#include <string.h>
#include "om.h"

void gap_init(struct GapBuf *g, const char *text, int len)
{
	int cap = len + GAP_INIT_CAP;
	g->buf = malloc(cap);
	g->cap = cap;
	if (text && len > 0) {
		memcpy(g->buf, text, len);
		g->gap_start = len;
	} else {
		g->gap_start = 0;
	}
	g->gap_end = cap;
}

void gap_free(struct GapBuf *g)
{
	free(g->buf);
	g->buf = NULL;
	g->cap = 0;
	g->gap_start = 0;
	g->gap_end = 0;
}

static void gap_grow(struct GapBuf *g, int need)
{
	int gap_size = g->gap_end - g->gap_start;
	int tail;
	int new_cap;
	char *new_buf;

	if (gap_size >= need) return;

	new_cap = g->cap + need + GAP_INIT_CAP;
	new_buf = malloc(new_cap);

	/* copy before gap */
	memcpy(new_buf, g->buf, g->gap_start);
	/* copy after gap */
	tail = g->cap - g->gap_end;
	memcpy(new_buf + new_cap - tail, g->buf + g->gap_end, tail);

	free(g->buf);
	g->buf = new_buf;
	g->gap_end = new_cap - tail;
	g->cap = new_cap;
}

void gap_insert(struct GapBuf *g, char c)
{
	gap_grow(g, 1);
	g->buf[g->gap_start++] = c;
}

void gap_insert_str(struct GapBuf *g, const char *s, int len)
{
	gap_grow(g, len);
	memcpy(g->buf + g->gap_start, s, len);
	g->gap_start += len;
}

void gap_delete(struct GapBuf *g, int n)
{
	if (n > g->gap_start) n = g->gap_start;
	g->gap_start -= n;
}

void gap_delete_fwd(struct GapBuf *g, int n)
{
	int avail = g->cap - g->gap_end;
	if (n > avail) n = avail;
	g->gap_end += n;
}

void gap_move(struct GapBuf *g, int pos)
{
	int len = gap_len(g);
	if (pos < 0) pos = 0;
	if (pos > len) pos = len;
	if (pos == g->gap_start) return;

	if (pos < g->gap_start) {
		int count = g->gap_start - pos;
		memmove(g->buf + g->gap_end - count, g->buf + pos, count);
		g->gap_start = pos;
		g->gap_end -= count;
	} else {
		int count = pos - g->gap_start;
		memmove(g->buf + g->gap_start, g->buf + g->gap_end, count);
		g->gap_start += count;
		g->gap_end += count;
	}
}

int gap_len(const struct GapBuf *g)
{
	return g->cap - (g->gap_end - g->gap_start);
}

char gap_char_at(const struct GapBuf *g, int pos)
{
	if (pos < 0 || pos >= gap_len(g)) return '\0';
	if (pos < g->gap_start)
		return g->buf[pos];
	return g->buf[g->gap_end + (pos - g->gap_start)];
}

char *gap_contents(const struct GapBuf *g)
{
	int len = gap_len(g);
	char *s = malloc(len + 1);
	int tail = g->cap - g->gap_end;

	memcpy(s, g->buf, g->gap_start);
	memcpy(s + g->gap_start, g->buf + g->gap_end, tail);
	s[len] = '\0';
	return s;
}
