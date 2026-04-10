#define _POSIX_C_SOURCE 200809L
/* onemark — file format parse/serialize
 *
 * Hand-written line-by-line parser for the OneMark .md format.
 * No YAML library needed — the format is a strict subset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "om.h"

/* --- helpers ------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
	char *d;
	if (!s) return NULL;
	d = malloc(strlen(s) + 1);
	strcpy(d, s);
	return d;
}

static void trim_trailing(char *s)
{
	int len = strlen(s);
	while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
		s[--len] = '\0';
}

/* Parse "key: value" from a line. Returns 1 if found, 0 otherwise.
 * key_out and val_out point into the line buffer (modified in place). */
static int parse_kv(char *line, char **key_out, char **val_out)
{
	char *colon = strchr(line, ':');
	if (!colon || colon == line) return 0;
	*colon = '\0';
	*key_out = line;
	*val_out = colon + 1;
	/* skip leading space after colon */
	while (**val_out == ' ') (*val_out)++;
	trim_trailing(*val_out);
	return 1;
}

/* JSON-unescape a quoted value: "foo bar" -> foo bar
 * Returns a new allocation. */
static char *json_unescape(const char *s)
{
	int len = strlen(s);
	char *out, *p;

	if (len < 2 || s[0] != '"' || s[len-1] != '"')
		return xstrdup(s);

	out = malloc(len);
	p = out;
	for (int i = 1; i < len - 1; i++) {
		if (s[i] == '\\' && i + 1 < len - 1) {
			i++;
			switch (s[i]) {
			case 'n': *p++ = '\n'; break;
			case 't': *p++ = '\t'; break;
			case '\\': *p++ = '\\'; break;
			case '"': *p++ = '"'; break;
			default: *p++ = '\\'; *p++ = s[i]; break;
			}
		} else {
			*p++ = s[i];
		}
	}
	*p = '\0';
	return out;
}

static enum Tag parse_tag(const char *s)
{
	if (strcmp(s, "idea") == 0) return TAG_IDEA;
	if (strcmp(s, "todo") == 0) return TAG_TODO;
	if (strcmp(s, "reference") == 0) return TAG_REFERENCE;
	return TAG_NONE;
}

static const char *tag_str(enum Tag t)
{
	switch (t) {
	case TAG_IDEA: return "idea";
	case TAG_TODO: return "todo";
	case TAG_REFERENCE: return "reference";
	default: return "none";
	}
}

/* Promote headings: ## -> #, ### -> ##, etc. (disk -> editor) */
static void promote_headings(char *body)
{
	/* in-place: for each line starting with ##, remove one # */
	char *p = body;
	char *out = body;
	int at_line_start = 1;

	while (*p) {
		if (at_line_start && p[0] == '#' && p[1] == '#') {
			/* skip one # */
			p++;
		}
		if (*p == '\n') at_line_start = 1;
		else at_line_start = 0;
		*out++ = *p++;
	}
	*out = '\0';
}

/* Demote headings for serialization: # -> ##, etc.
 * Returns new allocation. */
static char *demote_headings(const char *body)
{
	/* worst case: every line starts with #, each gains 1 char */
	int lines = 1;
	for (const char *p = body; *p; p++)
		if (*p == '\n') lines++;

	int len = strlen(body);
	char *out = malloc(len + lines + 1);
	char *o = out;
	const char *p = body;
	int at_line_start = 1;

	while (*p) {
		if (at_line_start && *p == '#') {
			*o++ = '#'; /* add extra # */
		}
		if (*p == '\n') at_line_start = 1;
		else at_line_start = 0;
		*o++ = *p++;
	}
	*o = '\0';
	return out;
}

/* Generate a simple timestamp-based ID (not SHA256 — good enough for phase 1) */
static void generate_id(char *out)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	snprintf(out, ID_LEN + 1, "%08lx%08lx%08lx%08lx",
		(unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec,
		(unsigned long)(ts.tv_sec ^ 0xdeadbeef),
		(unsigned long)(ts.tv_nsec ^ 0xcafebabe));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void timestamp_now(char *out, int size)
{
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	snprintf(out, (size_t)size, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		ts.tv_nsec / 1000);
}
#pragma GCC diagnostic pop

/* --- parse --------------------------------------------------------------- */

static char *read_file(const char *path, long *out_len)
{
	FILE *fp = fopen(path, "r");
	char *buf;
	long len;

	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = malloc(len + 1);
	len = fread(buf, 1, len, fp);
	buf[len] = '\0';
	fclose(fp);
	if (out_len) *out_len = len;
	return buf;
}

enum ParseState { PS_OUTSIDE, PS_FILE_FM, PS_BOX_YAML, PS_BODY };

int file_parse(struct NotebookFile *f, const char *path)
{
	long raw_len;
	char *raw = read_file(path, &raw_len);
	char *line, *saveptr;
	enum ParseState state = PS_OUTSIDE;
	int fence_count = 0;
	struct Box *cur = NULL;
	char body_buf[64 * 1024];
	int body_len = 0;

	if (!raw) return -1;

	memset(f, 0, sizeof *f);
	f->path = xstrdup(path);
	/* extract basename */
	{
		const char *slash = strrchr(path, '/');
		const char *base = slash ? slash + 1 : path;
		char *dot;
		f->name = xstrdup(base);
		dot = strrchr(f->name, '.');
		if (dot) *dot = '\0';
	}
	f->fm.onemark = 1;
	f->fm.schema = 1;

	for (line = strtok_r(raw, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr)) {

		if (strcmp(line, "---") == 0) {
			fence_count++;
			if (state == PS_OUTSIDE && fence_count == 1) {
				state = PS_FILE_FM;
				continue;
			}
			if (state == PS_FILE_FM) {
				state = PS_OUTSIDE;
				continue;
			}
			if (state == PS_OUTSIDE && cur) {
				/* box yaml fence open */
				state = PS_BOX_YAML;
				continue;
			}
			if (state == PS_BOX_YAML) {
				state = PS_BODY;
				body_len = 0;
				continue;
			}
			continue;
		}

		if (state == PS_FILE_FM) {
			char *key, *val;
			if (parse_kv(line, &key, &val)) {
				if (strcmp(key, "onemark") == 0)
					f->fm.onemark = atoi(val);
				else if (strcmp(key, "schema") == 0)
					f->fm.schema = atoi(val);
				else if (strcmp(key, "created") == 0)
					snprintf(f->fm.created, sizeof f->fm.created, "%s", val);
				else if (strcmp(key, "modified") == 0)
					snprintf(f->fm.modified, sizeof f->fm.modified, "%s", val);
			}
			continue;
		}

		/* H1 = new box */
		if (line[0] == '#' && line[1] == ' ' && state != PS_BOX_YAML) {
			/* flush previous box body */
			if (cur && body_len > 0) {
				/* trim trailing blank lines */
				while (body_len > 0 && body_buf[body_len-1] == '\n')
					body_len--;
				body_buf[body_len] = '\0';
				promote_headings(body_buf);
				gap_init(&cur->body, body_buf, strlen(body_buf));
				undo_init(&cur->undo);
			} else if (cur) {
				gap_init(&cur->body, "", 0);
				undo_init(&cur->undo);
			}

			if (f->box_count >= MAX_BOXES) continue;
			cur = &f->boxes[f->box_count++];
			memset(cur, 0, sizeof *cur);
			cur->title = xstrdup(line + 2);
			trim_trailing(cur->title);
			cur->tag = TAG_NONE;
			cur->x = 40; cur->y = 40;
			cur->w = 320; cur->h = 180;
			state = PS_OUTSIDE;
			body_len = 0;
			continue;
		}

		if (state == PS_BOX_YAML && cur) {
			char *key, *val;
			if (parse_kv(line, &key, &val)) {
				if (strcmp(key, "id") == 0)
					snprintf(cur->id, sizeof cur->id, "%s", val);
				else if (strcmp(key, "coordinates") == 0 || strcmp(key, "cordinates") == 0)
					sscanf(val, "%d, %d, %d, %d", &cur->x, &cur->y, &cur->w, &cur->h);
				else if (strcmp(key, "created") == 0)
					snprintf(cur->created, sizeof cur->created, "%s", val);
				else if (strcmp(key, "modified") == 0)
					snprintf(cur->modified, sizeof cur->modified, "%s", val);
				else if (strcmp(key, "tag") == 0)
					cur->tag = parse_tag(val);
				else if (cur->custom_count < MAX_CUSTOM_FIELDS) {
					/* custom field */
					cur->custom[cur->custom_count].key = xstrdup(key);
					cur->custom[cur->custom_count].value = json_unescape(val);
					cur->custom_count++;
				}
			}
			continue;
		}

		if (state == PS_BODY && cur) {
			int ll = strlen(line);
			if (body_len + ll + 1 < (int)sizeof body_buf) {
				if (body_len > 0)
					body_buf[body_len++] = '\n';
				memcpy(body_buf + body_len, line, ll);
				body_len += ll;
			}
			continue;
		}
	}

	/* flush last box body */
	if (cur && body_len > 0) {
		while (body_len > 0 && body_buf[body_len-1] == '\n')
			body_len--;
		body_buf[body_len] = '\0';
		promote_headings(body_buf);
		gap_init(&cur->body, body_buf, strlen(body_buf));
		undo_init(&cur->undo);
	} else if (cur && cur->body.buf == NULL) {
		gap_init(&cur->body, "", 0);
		undo_init(&cur->undo);
	}

	/* fill in missing IDs and timestamps */
	for (int i = 0; i < f->box_count; i++) {
		struct Box *b = &f->boxes[i];
		if (b->id[0] == '\0')
			generate_id(b->id);
		if (b->created[0] == '\0')
			timestamp_now(b->created, sizeof b->created);
		if (b->modified[0] == '\0')
			snprintf(b->modified, sizeof b->modified, "%s", b->created);
	}
	if (f->fm.created[0] == '\0' && f->box_count > 0)
		snprintf(f->fm.created, sizeof f->fm.created, "%s", f->boxes[0].created);
	if (f->fm.modified[0] == '\0')
		snprintf(f->fm.modified, sizeof f->fm.modified, "%s", f->fm.created);

	free(raw);
	return 0;
}

/* --- serialize ----------------------------------------------------------- */

/* JSON-escape a string value for output */
static void json_escape_to(FILE *fp, const char *s)
{
	fputc('"', fp);
	for (; *s; s++) {
		switch (*s) {
		case '"':  fputs("\\\"", fp); break;
		case '\\': fputs("\\\\", fp); break;
		case '\n': fputs("\\n", fp); break;
		case '\t': fputs("\\t", fp); break;
		default:   fputc(*s, fp); break;
		}
	}
	fputc('"', fp);
}

void file_init_empty(struct NotebookFile *f, const char *path)
{
	memset(f, 0, sizeof *f);
	f->path = xstrdup(path);
	/* extract basename */
	{
		const char *slash = strrchr(path, '/');
		const char *base = slash ? slash + 1 : path;
		char *dot;
		f->name = xstrdup(base);
		dot = strrchr(f->name, '.');
		if (dot) *dot = '\0';
	}
	f->fm.onemark = 1;
	f->fm.schema = 1;
	timestamp_now(f->fm.created, sizeof f->fm.created);
	snprintf(f->fm.modified, sizeof f->fm.modified, "%s", f->fm.created);
}

int file_save(const struct NotebookFile *f)
{
	FILE *fp = fopen(f->path, "w");
	if (!fp) return -1;

	/* file frontmatter */
	fprintf(fp, "---\n");
	fprintf(fp, "onemark: %d\n", f->fm.onemark);
	fprintf(fp, "schema: %d\n", f->fm.schema);
	fprintf(fp, "created: %s\n", f->fm.created);
	fprintf(fp, "modified: %s\n", f->fm.modified);
	fprintf(fp, "tags: []\n");
	fprintf(fp, "---\n\n");

	for (int i = 0; i < f->box_count; i++) {
		const struct Box *b = &f->boxes[i];
		char *body_text = gap_contents(&b->body);
		char *demoted = demote_headings(body_text);

		fprintf(fp, "# %s\n", b->title);
		fprintf(fp, "---\n");
		fprintf(fp, "id: %s\n", b->id);
		fprintf(fp, "coordinates: %d, %d, %d, %d\n", b->x, b->y, b->w, b->h);
		fprintf(fp, "created: %s\n", b->created);
		fprintf(fp, "modified: %s\n", b->modified);
		if (b->tag != TAG_NONE)
			fprintf(fp, "tag: %s\n", tag_str(b->tag));
		for (int j = 0; j < b->custom_count; j++) {
			fprintf(fp, "%s: ", b->custom[j].key);
			json_escape_to(fp, b->custom[j].value);
			fputc('\n', fp);
		}
		fprintf(fp, "---\n\n");
		if (demoted[0] != '\0')
			fprintf(fp, "%s\n", demoted);
		fprintf(fp, "\n");

		free(demoted);
		free(body_text);
	}

	fclose(fp);
	return 0;
}

int file_serialize(const struct NotebookFile *f, char **out, size_t *outlen)
{
	(void)f; (void)out; (void)outlen;
	/* TODO: serialize to buffer (for roundtrip testing) */
	return -1;
}

/* --- box helpers --------------------------------------------------------- */

void box_init_new(struct Box *b, int x, int y)
{
	memset(b, 0, sizeof *b);
	generate_id(b->id);
	timestamp_now(b->created, sizeof b->created);
	snprintf(b->modified, sizeof b->modified, "%s", b->created);
	/* default title from timestamp */
	{
		char buf[32];
		snprintf(buf, sizeof buf, "%.2s.%.2s.%.2s %.2s:%.2s",
			b->created + 2, b->created + 5, b->created + 8,
			b->created + 11, b->created + 14);
		b->title = xstrdup(buf);
	}
	b->title_is_default = 1;
	b->tag = TAG_NONE;
	b->x = x; b->y = y;
	b->w = 320; b->h = 180; /* default; caller can override via config */
	gap_init(&b->body, "", 0);
	undo_init(&b->undo);
}
