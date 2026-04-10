/* onemark — file format parse/serialize (stub for phase 1 build) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "om.h"

int file_parse(struct NotebookFile *f, const char *path)
{
	(void)f; (void)path;
	/* TODO: implement file parser */
	return -1;
}

int file_serialize(const struct NotebookFile *f, char **out, size_t *outlen)
{
	(void)f; (void)out; (void)outlen;
	/* TODO: implement file serializer */
	return -1;
}

int file_save(const struct NotebookFile *f)
{
	(void)f;
	/* TODO: implement file save */
	return -1;
}
