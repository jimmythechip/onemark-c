/* onemark — vim modal editing engine (stub for phase 1 build) */
#include "om.h"

void vim_init(struct VimState *v)
{
	v->mode = MODE_NORMAL;
	v->op = 0;
	v->count = 0;
	v->count2 = 0;
	v->cmd_len = 0;
}

void vim_keypress(struct VimState *v, struct GapBuf *buf, int key)
{
	(void)v; (void)buf; (void)key;
	/* TODO: implement vim state machine */
}
