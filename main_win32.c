/* onemark — Win32 GUI frontend
 *
 * Win32 window + GDI double-buffered rendering.
 * Upgrade path: swap GDI rendering for Direct2D + DirectWrite.
 */
/* config variables are defined in app.c (CONFIG_IMPL) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>  /* GET_X_LPARAM etc. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "app.h"

/* --- globals ------------------------------------------------------------- */

static struct App app;
static HWND hwnd;
static HFONT hfont_body, hfont_title, hfont_status;
static int font_w, font_h;  /* character cell metrics for body font */
static int char_ascent;      /* distance from top of cell to baseline */

/* double-buffer */
static HDC memdc;
static HBITMAP membmp;
static int mem_w, mem_h;

/* --- colors -------------------------------------------------------------- */

#define COL_BG      RGB(255, 255, 255)
#define COL_FG      RGB(0, 0, 0)
#define COL_FG_DIM  RGB(102, 102, 102)
#define COL_BORDER  RGB(208, 208, 208)
#define COL_BORDER_STRONG  RGB(128, 128, 128)
#define COL_ACCENT  RGB(31, 111, 235)
#define COL_SHADOW  RGB(230, 230, 230)
#define COL_SELECT  RGB(184, 207, 243)
#define COL_STATUS_BG  RGB(38, 38, 38)
#define COL_STATUS_FG  RGB(220, 220, 220)
#define COL_TAG_IDEA   RGB(31, 111, 235)
#define COL_TAG_TODO   RGB(217, 119, 6)
#define COL_TAG_REF    RGB(184, 50, 128)

static COLORREF tag_color(enum Tag tag) {
	switch (tag) {
	case TAG_IDEA:      return COL_TAG_IDEA;
	case TAG_TODO:      return COL_TAG_TODO;
	case TAG_REFERENCE: return COL_TAG_REF;
	default:            return COL_BORDER;
	}
}

/* --- coordinate conversion ----------------------------------------------- */

/* file-pixel to screen-pixel (apply viewport offset) */
static int fpx(int file_x) { return file_x - app.vp_x; }
static int fpy(int file_y) { return file_y - app.vp_y; }

/* screen-pixel to file-pixel */
static int sfx(int screen_x) { return screen_x + app.vp_x; }
static int sfy(int screen_y) { return screen_y + app.vp_y; }

/* --- font setup ---------------------------------------------------------- */

static void setup_fonts(HDC hdc)
{
	/* Consolas 13px — good monospace font on Windows */
	hfont_body = CreateFontW(
		-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

	hfont_title = CreateFontW(
		-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

	hfont_status = CreateFontW(
		-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

	/* measure character cell */
	HFONT old = SelectObject(hdc, hfont_body);
	TEXTMETRICW tm;
	GetTextMetricsW(hdc, &tm);
	font_w = tm.tmAveCharWidth;
	font_h = tm.tmHeight;
	char_ascent = tm.tmAscent;
	SelectObject(hdc, old);
}

/* --- double buffer ------------------------------------------------------- */

static void ensure_backbuffer(HDC hdc, int w, int h)
{
	if (memdc && mem_w >= w && mem_h >= h) return;
	if (memdc) { DeleteObject(membmp); DeleteDC(memdc); }
	memdc = CreateCompatibleDC(hdc);
	membmp = CreateCompatibleBitmap(hdc, w, h);
	SelectObject(memdc, membmp);
	mem_w = w;
	mem_h = h;
}

/* --- drawing helpers ----------------------------------------------------- */

static void fill_rect(HDC hdc, int x, int y, int w, int h, COLORREF col)
{
	RECT r = { x, y, x + w, y + h };
	HBRUSH br = CreateSolidBrush(col);
	FillRect(hdc, &r, br);
	DeleteObject(br);
}

static void draw_text_at(HDC hdc, int x, int y, const char *s, int len,
			 HFONT font, COLORREF fg, COLORREF bg, int opaque)
{
	HFONT old = SelectObject(hdc, font);
	SetTextColor(hdc, fg);
	if (opaque) {
		SetBkColor(hdc, bg);
		SetBkMode(hdc, OPAQUE);
	} else {
		SetBkMode(hdc, TRANSPARENT);
	}
	/* convert UTF-8 to wide chars (simplified: ASCII range) */
	WCHAR wbuf[1024];
	int wlen = MultiByteToWideChar(CP_UTF8, 0, s, len < 0 ? -1 : len,
				       wbuf, 1024);
	if (len >= 0 && wlen > 0)
		TextOutW(hdc, x, y, wbuf, wlen);
	else if (len < 0 && wlen > 1)
		TextOutW(hdc, x, y, wbuf, wlen - 1); /* exclude null */
	SelectObject(hdc, old);
}

static void draw_box_shadow(HDC hdc, int x, int y, int w, int h)
{
	fill_rect(hdc, x + 2, y + 2, w, h, COL_SHADOW);
}

/* --- box rendering ------------------------------------------------------- */

static void draw_box(HDC hdc, struct Box *b, int idx)
{
	int x = fpx(b->x), y = fpy(b->y);
	int w = b->w < 48 ? 48 : b->w;
	int h = b->h < 48 ? 48 : b->h;

	int focused = (idx == app.focused_box);
	int hovered = (idx == app.hovered_box);
	int editing = (focused && app.editing);

	/* shadow on hover/focus */
	if (hovered || focused)
		draw_box_shadow(hdc, x, y, w, h);

	/* background */
	fill_rect(hdc, x, y, w, h, COL_BG);

	/* border */
	COLORREF border_col = COL_BORDER;
	if (focused) border_col = COL_BORDER_STRONG;
	if (b->tag != TAG_NONE) border_col = tag_color(b->tag);

	if (hovered || focused) {
		HPEN pen = CreatePen(PS_SOLID, 1, border_col);
		HPEN old = SelectObject(hdc, pen);
		HBRUSH oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
		Rectangle(hdc, x, y, x + w, y + h);
		SelectObject(hdc, old);
		SelectObject(hdc, oldBr);
		DeleteObject(pen);
	}

	/* drag strip (top 5px) */
	if (hovered || focused)
		fill_rect(hdc, x + 1, y + 1, w - 2, 5, border_col);

	/* title */
	int title_y = y + 7;
	int title_x = x + 8;
	int title_w = w - 16;
	if (title_w > 0) {
		int tlen = (int)strlen(b->title);
		if (editing && app.editing_title) {
			/* draw title editing cursor */
			fill_rect(hdc, title_x, title_y, title_w, font_h + 2, COL_SELECT);
			draw_text_at(hdc, title_x + 2, title_y + 1, app.title_buf,
				     app.title_len, hfont_title, COL_FG, COL_SELECT, 0);
			/* simple cursor line */
			int cx = title_x + 2 + app.title_cursor * font_w;
			fill_rect(hdc, cx, title_y + 1, 1, font_h, COL_FG);
		} else {
			draw_text_at(hdc, title_x + 2, title_y + 1, b->title,
				     tlen, hfont_title, COL_FG, 0, 0);
		}
	}

	/* body */
	int body_x = x + 8;
	int body_y = title_y + font_h + 6;
	int body_w = w - 16;
	int body_h = y + h - body_y - 4;
	if (body_w <= 0 || body_h <= 0) return;

	/* flatten body text */
	char *content = gap_contents(&b->body);
	int cursor_pos = b->body.gap_start;
	int blen = gap_len(&b->body);

	/* compute visual highlight range */
	int vis_from = -1, vis_to = -1;
	if (editing && (app.vim.mode == MODE_VISUAL || app.vim.mode == MODE_VLINE)) {
		vis_from = app.vim.visual_start;
		vis_to = cursor_pos;
		if (vis_from > vis_to) { int t = vis_from; vis_from = vis_to; vis_to = t; }
		vis_to++;
	}

	/* find cursor line for scrolling */
	int cursor_line = 0;
	{
		int ln = 0;
		for (int ci = 0; ci < blen; ci++) {
			if (ci == cursor_pos) cursor_line = ln;
			if (content[ci] == '\n') ln++;
		}
		if (cursor_pos == blen) cursor_line = ln;
	}

	int max_rows = body_h / font_h;
	if (max_rows < 1) max_rows = 1;
	int scroll = cursor_line >= max_rows ? cursor_line - max_rows + 1 : 0;

	/* render lines */
	HFONT old_font = SelectObject(hdc, hfont_body);
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, COL_FG);

	char *line = content;
	int line_num = 0;
	int draw_row = 0;
	int char_off = 0;

	while (line && *line && draw_row < max_rows + scroll) {
		char *nl = strchr(line, '\n');
		int ll = nl ? (int)(nl - line) : (int)strlen(line);

		if (line_num >= scroll && draw_row - scroll < max_rows) {
			int row_idx = draw_row - scroll;
			int ty = body_y + row_idx * font_h;

			/* draw visual highlight background */
			if (vis_from >= 0) {
				int line_start_off = char_off;
				int line_end_off = char_off + ll;
				int hl_start = vis_from > line_start_off ? vis_from - line_start_off : 0;
				int hl_end = vis_to < line_end_off ? vis_to - line_start_off : ll;
				if (hl_start < ll && hl_end > 0 && hl_end > hl_start) {
					fill_rect(hdc, body_x + hl_start * font_w, ty,
						  (hl_end - hl_start) * font_w, font_h, COL_SELECT);
				}
			}

			/* draw text */
			int max_chars = body_w / font_w;
			int draw_len = ll < max_chars ? ll : max_chars;
			if (draw_len > 0) {
				WCHAR wbuf[1024];
				int wlen = MultiByteToWideChar(CP_UTF8, 0, line, draw_len, wbuf, 1024);
				TextOutW(hdc, body_x, ty, wbuf, wlen);
			}

			/* draw cursor */
			if (editing && char_off <= cursor_pos &&
			    cursor_pos <= char_off + ll) {
				int cx = cursor_pos - char_off;
				int cursor_x = body_x + cx * font_w;
				if (app.vim.mode == MODE_INSERT) {
					/* bar cursor */
					fill_rect(hdc, cursor_x, ty, 2, font_h, COL_ACCENT);
				} else {
					/* block cursor */
					char under = (cursor_pos < blen && content[cursor_pos] != '\n')
						? content[cursor_pos] : ' ';
					fill_rect(hdc, cursor_x, ty, font_w, font_h, COL_FG);
					if (under != ' ') {
						WCHAR wc;
						MultiByteToWideChar(CP_UTF8, 0, &under, 1, &wc, 1);
						SetTextColor(hdc, COL_BG);
						TextOutW(hdc, cursor_x, ty, &wc, 1);
						SetTextColor(hdc, COL_FG);
					}
				}
			}
		}

		char_off += ll + 1;
		draw_row++;
		line_num++;
		line = nl ? nl + 1 : NULL;
	}

	SelectObject(hdc, old_font);
	free(content);
}

/* --- status bar ---------------------------------------------------------- */

static void draw_status_bar(HDC hdc, int win_w, int win_h)
{
	int bar_h = 22;
	int bar_y = win_h - bar_h;

	fill_rect(hdc, 0, bar_y, win_w, bar_h, COL_STATUS_BG);

	char status[256];
	const char *mode_str;
	switch (app.vim.mode) {
	case MODE_INSERT:     mode_str = "INSERT"; break;
	case MODE_VISUAL:     mode_str = "VISUAL"; break;
	case MODE_VLINE:      mode_str = "V-LINE"; break;
	case MODE_OP_PENDING: mode_str = "OP-PENDING"; break;
	case MODE_COMMAND:    mode_str = "COMMAND"; break;
	default:              mode_str = app.leader_pending ? "LEADER" : "NORMAL"; break;
	}

	if (app.vim.search_active) {
		snprintf(status, sizeof status, "  /%.250s", app.vim.search_buf);
	} else if (app.vim.mode == MODE_COMMAND) {
		snprintf(status, sizeof status, "  :%.250s", app.vim.cmd_buf);
	} else {
		const char *fname = app.file.name ? app.file.name : "(no file)";
		int box_num = app.focused_box >= 0 ? app.focused_box + 1 : 0;
		snprintf(status, sizeof status, "  %s | %s | box %d/%d %s",
			 mode_str, fname, box_num, app.file.box_count,
			 app.file.dirty ? "[+]" : "");
	}

	draw_text_at(hdc, 4, bar_y + 3, status, -1,
		     hfont_status, COL_STATUS_FG, COL_STATUS_BG, 1);
}

/* --- full repaint -------------------------------------------------------- */

static void paint(HWND hwnd_arg)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd_arg, &ps);
	RECT cr;
	GetClientRect(hwnd_arg, &cr);
	int w = cr.right, h = cr.bottom;

	ensure_backbuffer(hdc, w, h);

	/* clear */
	fill_rect(memdc, 0, 0, w, h, RGB(245, 245, 245));

	/* update view dimensions */
	app.view_w = w;
	app.view_h = h - 22; /* minus status bar */

	/* draw boxes (back to front) */
	for (int i = 0; i < app.file.box_count; i++)
		draw_box(memdc, &app.file.boxes[i], i);

	/* empty state hint */
	if (app.file.box_count == 0) {
		draw_text_at(memdc, 20, 20,
			"Click anywhere to create a box.  Type  :newbox  for more.",
			-1, hfont_title, COL_FG_DIM, 0, 0);
	}

	draw_status_bar(memdc, w, h);

	/* blit */
	BitBlt(hdc, 0, 0, w, h, memdc, 0, 0, SRCCOPY);
	EndPaint(hwnd_arg, &ps);
}

/* --- key mapping --------------------------------------------------------- */

static int map_vk(WPARAM vk)
{
	switch (vk) {
	case VK_ESCAPE:  return KEY_ESC;
	case VK_UP:      return KEY_UP;
	case VK_DOWN:    return KEY_DOWN;
	case VK_LEFT:    return KEY_LEFT;
	case VK_RIGHT:   return KEY_RIGHT;
	case VK_HOME:    return KEY_HOME;
	case VK_END:     return KEY_END;
	case VK_PRIOR:   return KEY_PGUP;
	case VK_NEXT:    return KEY_PGDN;
	case VK_DELETE:  return KEY_DEL;
	case VK_BACK:    return KEY_BACKSPACE;
	case VK_RETURN:  return '\n';
	case VK_TAB:     return '\t';
	default:         return 0;
	}
}

/* --- window procedure ---------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
		paint(hw);
		return 0;

	case WM_SIZE:
		InvalidateRect(hw, NULL, FALSE);
		return 0;

	case WM_KEYDOWN: {
		int key = map_vk(wp);
		/* Ctrl+key */
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			if (wp >= 'A' && wp <= 'Z')
				key = KEY_CTRL((int)wp - 'A' + 'a');
		}
		if (key) {
			app_key(&app, key);
			if (!app.running) { PostQuitMessage(0); return 0; }
			InvalidateRect(hw, NULL, FALSE);
		}
		return 0;
	}

	case WM_CHAR: {
		int ch = (int)wp;
		/* skip control chars already handled by WM_KEYDOWN */
		if (ch >= 32 && ch < 127) {
			app_key(&app, ch);
			if (!app.running) { PostQuitMessage(0); return 0; }
			InvalidateRect(hw, NULL, FALSE);
		}
		return 0;
	}

	case WM_LBUTTONDOWN: {
		int sx = GET_X_LPARAM(lp);
		int sy = GET_Y_LPARAM(lp);
		app_mouse_down(&app, sfx(sx), sfy(sy), 0);
		SetCapture(hw);
		InvalidateRect(hw, NULL, FALSE);
		return 0;
	}

	case WM_LBUTTONUP: {
		int sx = GET_X_LPARAM(lp);
		int sy = GET_Y_LPARAM(lp);
		app_mouse_up(&app, sfx(sx), sfy(sy), 0);
		ReleaseCapture();
		InvalidateRect(hw, NULL, FALSE);
		return 0;
	}

	case WM_MOUSEMOVE: {
		int sx = GET_X_LPARAM(lp);
		int sy = GET_Y_LPARAM(lp);
		app_mouse_move(&app, sfx(sx), sfy(sy));

		/* set cursor shape based on hit zone */
		int bi;
		enum HitZone zone = app_hit_test(&app, sfx(sx), sfy(sy), &bi);
		switch (zone) {
		case HIT_BODY:   SetCursor(LoadCursor(NULL, IDC_IBEAM)); break;
		case HIT_BORDER: case HIT_TITLE: SetCursor(LoadCursor(NULL, IDC_SIZEALL)); break;
		case HIT_RESIZE: SetCursor(LoadCursor(NULL, IDC_SIZEWE)); break;
		default:         SetCursor(LoadCursor(NULL, IDC_ARROW)); break;
		}

		if (app.dragging)
			InvalidateRect(hw, NULL, FALSE);
		return 0;
	}

	case WM_MOUSEWHEEL: {
		int delta = GET_WHEEL_DELTA_WPARAM(wp);
		app.vp_y -= delta / 2;
		if (app.vp_y < 0) app.vp_y = 0;
		InvalidateRect(hw, NULL, FALSE);
		return 0;
	}

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hw, msg, wp, lp);
}

/* --- entry point --------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
	(void)hPrev; (void)nShow;

	/* parse command line (simplified: first arg is filename) */
	const char *path = "notes.md";
	if (cmdLine && cmdLine[0])
		path = cmdLine;

	conf_ensure_dir();
	conf_load();
	app_init(&app, path);

	/* register window class */
	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"OneMarkWindow";
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	RegisterClassExW(&wc);

	/* create window */
	hwnd = CreateWindowExW(
		0, L"OneMarkWindow", L"OneMark",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
		NULL, NULL, hInst, NULL);

	/* setup fonts */
	HDC hdc = GetDC(hwnd);
	setup_fonts(hdc);
	ReleaseDC(hwnd, hdc);

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	/* message pump */
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	/* cleanup */
	if (memdc) { DeleteObject(membmp); DeleteDC(memdc); }
	DeleteObject(hfont_body);
	DeleteObject(hfont_title);
	DeleteObject(hfont_status);
	app_destroy(&app);

	return (int)msg.wParam;
}
