/* onemark — core types and constants */
#ifndef OM_H
#define OM_H

#include <stddef.h>

/* --- attributes for plat_addstr ----------------------------------------- */
#define ATTR_BOLD    (1 << 0)
#define ATTR_DIM     (1 << 1)
#define ATTR_REVERSE (1 << 2)
#define ATTR_ULINE   (1 << 3)
#define ATTR_ITALIC  (1 << 4)
#define ATTR_COLOR(n) (((n) & 0xf) << 8)
#define ATTR_FG(a)    (((a) >> 8) & 0xf)

/* ANSI colors */
enum {
	COL_BLACK, COL_RED, COL_GREEN, COL_YELLOW,
	COL_BLUE, COL_MAGENTA, COL_CYAN, COL_WHITE
};

/* --- tags ---------------------------------------------------------------- */
enum Tag {
	TAG_NONE = 0,
	TAG_IDEA,
	TAG_TODO,
	TAG_REFERENCE
};

/* --- gap buffer ---------------------------------------------------------- */
#define GAP_INIT_CAP 1024

struct GapBuf {
	char *buf;
	int   cap;
	int   gap_start;
	int   gap_end;
};

void  gap_init(struct GapBuf *g, const char *text, int len);
void  gap_free(struct GapBuf *g);
void  gap_insert(struct GapBuf *g, char c);
void  gap_insert_str(struct GapBuf *g, const char *s, int len);
void  gap_delete(struct GapBuf *g, int n);  /* delete n chars before gap */
void  gap_delete_fwd(struct GapBuf *g, int n); /* delete n chars after gap */
void  gap_move(struct GapBuf *g, int pos);  /* move gap to pos */
int   gap_len(const struct GapBuf *g);
char  gap_char_at(const struct GapBuf *g, int pos);
/* Copy content to a null-terminated string. Caller frees. */
char *gap_contents(const struct GapBuf *g);

/* --- undo ring ----------------------------------------------------------- */
#define UNDO_MAX 100

struct UndoEntry {
	char *text;       /* snapshot of buffer content (malloc'd) */
	int   cursor;     /* cursor position at snapshot time */
};

struct UndoRing {
	struct UndoEntry entries[UNDO_MAX];
	int current;      /* index of "now" (-1 = nothing saved) */
	int count;        /* number of valid entries */
};

void undo_init(struct UndoRing *u);
void undo_free(struct UndoRing *u);
void undo_push(struct UndoRing *u, struct GapBuf *buf);
int  undo_undo(struct UndoRing *u, struct GapBuf *buf);
int  undo_redo(struct UndoRing *u, struct GapBuf *buf);

/* --- custom fields ------------------------------------------------------- */
#define MAX_CUSTOM_FIELDS 16

struct CustomField {
	char *key;
	char *value;
};

/* --- box ----------------------------------------------------------------- */
#define ID_LEN 32

struct Box {
	char id[ID_LEN + 1];
	char *title;
	struct GapBuf body;
	struct UndoRing undo;
	int x, y, w, h;          /* pixel coordinates (Electron compat) */
	char created[32];
	char modified[32];
	int  title_is_default;
	enum Tag tag;
	struct CustomField custom[MAX_CUSTOM_FIELDS];
	int custom_count;
};

/* --- file ---------------------------------------------------------------- */
#define MAX_BOXES 256

struct FileFrontmatter {
	int  onemark;
	int  schema;
	char created[32];
	char modified[32];
	/* tags array omitted for phase 1 */
};

struct NotebookFile {
	char *path;
	char *name;
	struct FileFrontmatter fm;
	struct Box boxes[MAX_BOXES];
	int box_count;
	int dirty;
};

/* --- yank register ------------------------------------------------------- */
struct YankReg {
	char *text;
	int   len;
	int   linewise;   /* 1 if yanked whole lines (dd, yy, V) */
};

/* --- vim state ----------------------------------------------------------- */
enum VimMode {
	MODE_NORMAL,
	MODE_INSERT,
	MODE_VISUAL,
	MODE_VLINE,
	MODE_OP_PENDING,
	MODE_COMMAND
};

/* vim result codes (set after vim_keypress) */
enum VimResult {
	VIM_RESULT_NONE = 0,
	VIM_RESULT_SAVE,
	VIM_RESULT_QUIT,
	VIM_RESULT_SAVEQUIT,
	VIM_RESULT_NEWBOX,
	VIM_RESULT_DELBOX,
	VIM_RESULT_DUPBOX,
	VIM_RESULT_SET_MARK,   /* mark_letter set */
	VIM_RESULT_JUMP_MARK,  /* mark_letter set */
	VIM_RESULT_SET_FIELD,  /* cmd_buf has "key value" */
	VIM_RESULT_SET_TAG,    /* cmd_buf has tag name */
	VIM_RESULT_ZZ,
	VIM_RESULT_ZQ,
};

#define SEARCH_MAX 128

struct VimState {
	enum VimMode mode;
	int  op;            /* pending operator: 'd', 'y', 'c', '>', '<', 0 */
	int  count;         /* numeric prefix */
	int  pending_g;     /* waiting for second key after 'g' */
	char cmd_buf[256];  /* ex command line buffer */
	int  cmd_len;
	enum VimResult result;
	struct YankReg yank;
	/* visual mode */
	int  visual_start;
	/* search */
	char search_buf[SEARCH_MAX];
	int  search_len;
	int  search_active;
	/* marks */
	int  mark_pending;  /* 1 = waiting for mark letter */
	int  mark_action;   /* 'm' = set, '\'' = jump */
	char mark_letter;   /* the letter, set with result */
	/* Z prefix */
	int  pending_Z;
};

/* --- mouse event --------------------------------------------------------- */
struct MouseEvent {
	int row, col;
	int button;   /* 0=left, 1=middle, 2=right */
	int pressed;  /* 1=press, 0=release */
};

/* --- input result -------------------------------------------------------- */
#define INPUT_KEY    1
#define INPUT_MOUSE  2
#define INPUT_NONE   0
#define INPUT_RESIZE 3

/* Special key codes (beyond ASCII) */
#define KEY_ESC      27
#define KEY_UP       0x100
#define KEY_DOWN     0x101
#define KEY_LEFT     0x102
#define KEY_RIGHT    0x103
#define KEY_HOME     0x104
#define KEY_END      0x105
#define KEY_PGUP     0x106
#define KEY_PGDN     0x107
#define KEY_DEL      0x108
#define KEY_BACKSPACE 0x109
#define KEY_CTRL(c)  ((c) & 0x1f)

/* --- app-level cross-box state ------------------------------------------- */

struct Mark {
	int box;    /* box index, -1 = unset */
	int pos;    /* cursor position in box body */
};

#define JUMP_MAX 50

struct JumpHistory {
	struct { int box; int pos; } ring[JUMP_MAX];
	int cursor;
	int count;
};

/* --- config (conf.c) ----------------------------------------------------- */
void conf_load(void);
void conf_ensure_dir(void);

/* --- platform interface (implemented in plat/ ) -------------------------- */
void plat_init(void);
void plat_deinit(void);
void plat_clear(void);
void plat_move(int row, int col);
void plat_addstr(const char *s, int len, int attr);
void plat_addch(char c, int attr);
void plat_adduc(int uc, int attr);  /* write Unicode codepoint */
void plat_refresh(void);
int  plat_rows(void);
int  plat_cols(void);
void plat_show_cursor(int row, int col);
void plat_hide_cursor(void);
/* Returns INPUT_KEY (key in *key), INPUT_MOUSE (*mouse filled),
 * INPUT_RESIZE, or INPUT_NONE (timeout). timeout_ms=0 for non-blocking. */
int  plat_getinput(int *key, struct MouseEvent *mouse, int timeout_ms);

/* --- file format --------------------------------------------------------- */
int  file_parse(struct NotebookFile *f, const char *path);
void file_init_empty(struct NotebookFile *f, const char *path);
int  file_serialize(const struct NotebookFile *f, char **out, size_t *outlen);
int  file_save(const struct NotebookFile *f);

/* --- vim engine ---------------------------------------------------------- */
void vim_init(struct VimState *v);
void vim_keypress(struct VimState *v, struct GapBuf *buf,
		  struct UndoRing *undo, int key);

/* --- box helpers --------------------------------------------------------- */
void box_init_new(struct Box *b, int x, int y, int w, int h);

/* --- text operations (edit.c) -------------------------------------------- */
int  edit_toggle_wrap(struct GapBuf *buf, struct UndoRing *undo,
		      int from, int to, const char *marker);
int  edit_checkbox_rotate(struct GapBuf *buf, struct UndoRing *undo, int pos);
int  edit_surround_add(struct GapBuf *buf, struct UndoRing *undo,
		       int from, int to, char ch);
int  edit_surround_delete(struct GapBuf *buf, struct UndoRing *undo,
			  int pos, char ch);
int  edit_next_heading(struct GapBuf *buf, int pos);
int  edit_prev_heading(struct GapBuf *buf, int pos);
int  edit_next_list_item(struct GapBuf *buf, int pos);
int  edit_prev_list_item(struct GapBuf *buf, int pos);

#endif /* OM_H */
