#include "gd_io.h"

#include <util/bmem.h>

#include <stdio.h>

char *gd_file_read_alloc(const char *path, long max_sz)
{
	FILE *f;
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz <= 0 || sz > max_sz) {
		fclose(f);
		return NULL;
	}
	char *buf = (char *)bmalloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	return buf;
}
