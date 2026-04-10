/* onemark — VT100/ANSI terminal platform backend
 *
 * Raw termios setup, ANSI escape sequences, SGR mouse protocol.
 * Adapted from termtris (jtsiomb/termtris) patterns.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include "../core/om.h"

static struct termios saved_term;
static int term_w = 80;
static int term_h = 24;
static volatile int resize_flag;

static void sigwinch(int s)
{
	(void)s;
	resize_flag = 1;
	signal(SIGWINCH, sigwinch);
}

static void update_size(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
		term_w = ws.ws_col;
		term_h = ws.ws_row;
	}
}

void plat_init(void)
{
	struct termios t;

	tcgetattr(STDIN_FILENO, &saved_term);
	t = saved_term;
	t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
	t.c_oflag &= ~OPOST;
	t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);

	update_size();
	signal(SIGWINCH, sigwinch);

	/* enable SGR mouse tracking (press + release, exact coords) */
	fputs("\033[?1006h\033[?1000h", stdout);
	/* hide cursor initially */
	fputs("\033[?25l", stdout);
	/* alternate screen buffer */
	fputs("\033[?1049h", stdout);
	fflush(stdout);
}

void plat_deinit(void)
{
	/* disable mouse, show cursor, restore main screen */
	fputs("\033[?1000l\033[?1006l", stdout);
	fputs("\033[?25h", stdout);
	fputs("\033[?1049l", stdout);
	fputs("\033[0m", stdout);
	fflush(stdout);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
}

void plat_clear(void)
{
	fputs("\033[H\033[2J", stdout);
}

void plat_move(int row, int col)
{
	if ((row | col) == 0)
		fputs("\033[H", stdout);
	else
		printf("\033[%d;%dH", row + 1, col + 1);
}

void plat_addstr(const char *s, int len, int attr)
{
	int bold = attr & ATTR_BOLD;
	int rev  = attr & ATTR_REVERSE;
	int dim  = attr & ATTR_DIM;
	int fg   = ATTR_FG(attr);

	if (bold || rev || dim || fg) {
		fputs("\033[0", stdout);
		if (bold) fputs(";1", stdout);
		if (dim)  fputs(";2", stdout);
		if (rev)  fputs(";7", stdout);
		if (fg)   printf(";%d", 30 + fg);
		fputc('m', stdout);
	}

	if (len < 0)
		fputs(s, stdout);
	else
		fwrite(s, 1, len, stdout);

	if (bold || rev || dim || fg)
		fputs("\033[0m", stdout);
}

void plat_addch(char c, int attr)
{
	plat_addstr(&c, 1, attr);
}

void plat_refresh(void)
{
	fflush(stdout);
}

int plat_rows(void) { return term_h; }
int plat_cols(void) { return term_w; }

/* Parse an SGR mouse sequence: \033[<btn;col;rowM or m */
static int parse_mouse(const char *buf, int len, struct MouseEvent *m)
{
	int btn, col, row, i;
	char trail;

	if (len < 6 || buf[0] != '\033' || buf[1] != '[' || buf[2] != '<')
		return 0;

	i = 3;
	btn = 0;
	while (i < len && buf[i] >= '0' && buf[i] <= '9')
		btn = btn * 10 + (buf[i++] - '0');
	if (i >= len || buf[i] != ';') return 0;
	i++;

	col = 0;
	while (i < len && buf[i] >= '0' && buf[i] <= '9')
		col = col * 10 + (buf[i++] - '0');
	if (i >= len || buf[i] != ';') return 0;
	i++;

	row = 0;
	while (i < len && buf[i] >= '0' && buf[i] <= '9')
		row = row * 10 + (buf[i++] - '0');
	if (i >= len) return 0;

	trail = buf[i];
	if (trail != 'M' && trail != 'm') return 0;

	m->button = btn & 3;  /* 0=left, 1=middle, 2=right */
	m->col = col - 1;     /* 1-based → 0-based */
	m->row = row - 1;
	m->pressed = (trail == 'M');
	return i + 1;  /* bytes consumed */
}

/* Parse an escape sequence for special keys */
static int parse_escape(const char *buf, int len, int *key)
{
	if (len < 2) return 0;

	if (buf[1] == '[') {
		if (len < 3) return 0;
		switch (buf[2]) {
		case 'A': *key = KEY_UP;    return 3;
		case 'B': *key = KEY_DOWN;  return 3;
		case 'C': *key = KEY_RIGHT; return 3;
		case 'D': *key = KEY_LEFT;  return 3;
		case 'H': *key = KEY_HOME;  return 3;
		case 'F': *key = KEY_END;   return 3;
		case '3':
			if (len >= 4 && buf[3] == '~') { *key = KEY_DEL; return 4; }
			return 0;
		case '5':
			if (len >= 4 && buf[3] == '~') { *key = KEY_PGUP; return 4; }
			return 0;
		case '6':
			if (len >= 4 && buf[3] == '~') { *key = KEY_PGDN; return 4; }
			return 0;
		case '<':
			/* could be mouse — handled separately */
			return 0;
		}
	}
	return 0;
}

int plat_getinput(int *key, struct MouseEvent *mouse, int timeout_ms)
{
	fd_set fds;
	struct timeval tv;
	static char buf[64];
	static int buf_len;
	int rd, consumed;

	/* check for pending resize */
	if (resize_flag) {
		resize_flag = 0;
		update_size();
		return INPUT_RESIZE;
	}

	/* try to read more data */
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
		rd = read(STDIN_FILENO, buf + buf_len, sizeof(buf) - buf_len);
		if (rd > 0)
			buf_len += rd;
	}

	if (buf_len == 0)
		return INPUT_NONE;

	/* try mouse first */
	consumed = parse_mouse(buf, buf_len, mouse);
	if (consumed > 0) {
		buf_len -= consumed;
		if (buf_len > 0) memmove(buf, buf + consumed, buf_len);
		return INPUT_MOUSE;
	}

	/* try escape sequence */
	if (buf[0] == '\033' && buf_len > 1) {
		consumed = parse_escape(buf, buf_len, key);
		if (consumed > 0) {
			buf_len -= consumed;
			if (buf_len > 0) memmove(buf, buf + consumed, buf_len);
			return INPUT_KEY;
		}
		/* lone escape or unrecognised — wait briefly for more bytes */
		if (buf_len == 1) {
			/* lone Escape — return it as KEY_ESC */
			*key = KEY_ESC;
			buf_len = 0;
			return INPUT_KEY;
		}
	}

	/* plain byte */
	*key = (unsigned char)buf[0];
	if (*key == 127) *key = KEY_BACKSPACE;
	buf_len--;
	if (buf_len > 0) memmove(buf, buf + 1, buf_len);
	return INPUT_KEY;
}
