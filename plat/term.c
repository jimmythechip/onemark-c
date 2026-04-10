/* onemark — VT100/ANSI terminal platform backend (termbox2)
 *
 * Thin wrapper around termbox2, keeping the plat_* API unchanged
 * so core code is unaffected. termbox2 handles termios, escape
 * sequences, mouse protocols, SIGWINCH, and double-buffered
 * screen updates (only redraws changed cells).
 */
#define TB_IMPL
#include "../lib/termbox2.h"
#include "../core/om.h"

/* cursor position for streaming writes (plat_addstr advances this) */
int cur_x, cur_y;

void plat_init(void)
{
	tb_init();
	tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
	tb_set_output_mode(TB_OUTPUT_NORMAL);
	tb_hide_cursor();
	cur_x = 0;
	cur_y = 0;
}

void plat_deinit(void)
{
	tb_shutdown();
}

void plat_clear(void)
{
	tb_clear();
	cur_x = 0;
	cur_y = 0;
}

void plat_move(int row, int col)
{
	cur_y = row;
	cur_x = col;
}

static uintattr_t map_fg(int attr)
{
	/* White on black for everything — colors deferred to later */
	uintattr_t fg = TB_WHITE;
	if (attr & ATTR_BOLD)    fg |= TB_BOLD;
	if (attr & ATTR_REVERSE) fg |= TB_REVERSE;
	if (attr & ATTR_DIM)     fg = TB_DEFAULT; /* dim = terminal default (grey) */
	return fg;
}

void plat_addstr(const char *s, int len, int attr)
{
	uintattr_t fg = map_fg(attr);
	int w = tb_width();
	int h = tb_height();
	int n = (len < 0) ? (int)strlen(s) : len;

	for (int i = 0; i < n; i++) {
		if (cur_x >= 0 && cur_x < w && cur_y >= 0 && cur_y < h)
			tb_set_cell(cur_x, cur_y, (uint32_t)(unsigned char)s[i], fg, TB_DEFAULT);
		cur_x++;
	}
}

void plat_addch(char c, int attr)
{
	plat_addstr(&c, 1, attr);
}

void plat_adduc(int uc, int attr)
{
	uintattr_t fg = map_fg(attr);
	int w = tb_width();
	int h = tb_height();
	if (cur_x >= 0 && cur_x < w && cur_y >= 0 && cur_y < h)
		tb_set_cell(cur_x, cur_y, (uint32_t)uc, fg, TB_DEFAULT);
	cur_x++;
}

void plat_refresh(void)
{
	tb_present();
}

int plat_rows(void) { return tb_height(); }
int plat_cols(void) { return tb_width(); }

void plat_show_cursor(int row, int col)
{
	tb_set_cursor(col, row); /* termbox2 uses (x,y) */
}

void plat_hide_cursor(void)
{
	tb_hide_cursor();
}

/* Map termbox2 key codes to our KEY_* constants */
static int map_key(uint16_t tbkey)
{
	switch (tbkey) {
	case TB_KEY_ARROW_UP:    return KEY_UP;
	case TB_KEY_ARROW_DOWN:  return KEY_DOWN;
	case TB_KEY_ARROW_LEFT:  return KEY_LEFT;
	case TB_KEY_ARROW_RIGHT: return KEY_RIGHT;
	case TB_KEY_HOME:        return KEY_HOME;
	case TB_KEY_END:         return KEY_END;
	case TB_KEY_PGUP:        return KEY_PGUP;
	case TB_KEY_PGDN:        return KEY_PGDN;
	case TB_KEY_DELETE:       return KEY_DEL;
	case TB_KEY_BACKSPACE2:  return KEY_BACKSPACE;
	case TB_KEY_BACKSPACE:   return KEY_BACKSPACE;
	case TB_KEY_ENTER:       return '\n';
	case TB_KEY_ESC:         return KEY_ESC;
	default:
		/* ctrl keys: TB_KEY_CTRL_A = 0x01, etc. */
		if (tbkey >= 0x01 && tbkey <= 0x1a)
			return tbkey; /* matches KEY_CTRL(c) */
		return 0;
	}
}

int plat_getinput(int *key, struct MouseEvent *mouse, int timeout_ms)
{
	struct tb_event ev;
	int rv = tb_peek_event(&ev, timeout_ms);

	if (rv == TB_ERR_NO_EVENT || rv == TB_ERR_POLL)
		return INPUT_NONE;
	if (rv < 0)
		return INPUT_NONE;

	switch (ev.type) {
	case TB_EVENT_KEY:
		if (ev.ch) {
			*key = (int)ev.ch;
		} else {
			*key = map_key(ev.key);
			if (*key == 0) return INPUT_NONE;
		}
		return INPUT_KEY;

	case TB_EVENT_MOUSE:
		mouse->col = ev.x;
		mouse->row = ev.y;
		mouse->pressed = 1;
		switch (ev.key) {
		case TB_KEY_MOUSE_LEFT:       mouse->button = 0; break;
		case TB_KEY_MOUSE_MIDDLE:     mouse->button = 1; break;
		case TB_KEY_MOUSE_RIGHT:      mouse->button = 2; break;
		case TB_KEY_MOUSE_RELEASE:    mouse->button = 0; mouse->pressed = 0; break;
		case TB_KEY_MOUSE_WHEEL_UP:   mouse->button = 3; break;
		case TB_KEY_MOUSE_WHEEL_DOWN: mouse->button = 4; break;
		default: mouse->button = 0; break;
		}
		return INPUT_MOUSE;

	case TB_EVENT_RESIZE:
		return INPUT_RESIZE;
	}
	return INPUT_NONE;
}
