/* onemark — main entry point */
#include <stdio.h>
#include <stdlib.h>
#include "core/om.h"

static void draw_status(const char *msg)
{
	plat_move(plat_rows() - 1, 0);
	plat_addstr(msg, -1, ATTR_REVERSE);
	/* pad to end of line */
	int len = 0;
	const char *p = msg;
	while (*p++) len++;
	int cols = plat_cols();
	for (int i = len; i < cols; i++)
		plat_addch(' ', ATTR_REVERSE);
}

int main(int argc, char **argv)
{
	int key, running = 1;
	struct MouseEvent mouse;

	(void)argc; (void)argv;

	plat_init();
	plat_clear();

	plat_move(0, 0);
	plat_addstr("onemark", -1, ATTR_BOLD);
	plat_move(2, 0);
	plat_addstr("Press 'q' to quit. Click anywhere to test mouse.", -1, 0);

	draw_status(" NORMAL | onemark v0.1 ");
	plat_refresh();

	while (running) {
		int result = plat_getinput(&key, &mouse, 100);

		switch (result) {
		case INPUT_KEY:
			if (key == 'q' || key == KEY_CTRL('c'))
				running = 0;
			else {
				char info[64];
				snprintf(info, sizeof info, "key: %d (0x%x)  ", key, key);
				plat_move(4, 0);
				plat_addstr(info, -1, 0);
				plat_refresh();
			}
			break;

		case INPUT_MOUSE:
			{
				char info[64];
				snprintf(info, sizeof info,
					"mouse: btn=%d %s at row=%d col=%d  ",
					mouse.button,
					mouse.pressed ? "press" : "release",
					mouse.row, mouse.col);
				plat_move(5, 0);
				plat_addstr(info, -1, 0);
				plat_refresh();
			}
			break;

		case INPUT_RESIZE:
			plat_clear();
			{
				char info[64];
				snprintf(info, sizeof info,
					"resized: %dx%d", plat_cols(), plat_rows());
				plat_move(0, 0);
				plat_addstr(info, -1, ATTR_BOLD);
			}
			draw_status(" NORMAL | onemark v0.1 ");
			plat_refresh();
			break;

		case INPUT_NONE:
			break;
		}
	}

	plat_deinit();
	return 0;
}
