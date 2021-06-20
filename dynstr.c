/*
 * Of course we could have used stdio open_memstream()
 */

#include "dynstr.h"

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

void dynstr_grow  (struct dynstr *);

struct dynstr *
dynstr_create()
{
	struct dynstr *ds;

	if ((ds = calloc(1, sizeof(ds))) == NULL)
		return NULL;
	return ds;
}

void
dynstr_free(struct dynstr *ds)
{
	if (ds != NULL)
		free(ds->buf);
	free(ds);
}

void
dynstr_clear(struct dynstr *ds)
{
	if (ds == NULL)
		return;

	if (ds->buf == NULL) {
		dynstr_grow(ds);
		if (ds->err)
			return;
	}
	ds->cursor = ds->buf;
	ds->buf[0] = '\0';
	ds->len = 0;
	ds->err = 0;
}

void
dynstr_grow(struct dynstr *ds)
{
	if (ds == NULL || ds->err)
		return;

	if (ds->alloc == 0)
		ds->alloc = DYNSTR_INITIAL_ALLOC;
	else
		ds->alloc *= 2;

	ds->buf = realloc(ds->buf, (sizeof(char) * ds->alloc));
	if (ds->buf == NULL)
		ds->err = errno;
	else
		ds->cursor = ds->buf + ds->len;
}

void
dynstr_add(struct dynstr *ds, const char *fmt, ...)
{
	va_list ap;
	size_t ret;

	if (ds == NULL || ds->err)
		return;

	/*
	 * Calculate length of format string output without using
	 * any buffer.
	 */
	va_start(ap, fmt);
	if ((ret = vsnprintf(ds->cursor, 0, fmt, ap)) < 0)
		ds->err = errno;
	else {
		while (ds->len + ret >= ds->alloc) {
			dynstr_grow(ds);
			if (ds->err)
				break;
		}
	}
	va_end(ap);
	if (ds->err)
		return;

	/*
	 * If all was okay and we have space, actually write to the
	 * buffer now.
	 */
	va_start(ap, fmt);
	if ((ret = vsnprintf(ds->cursor, ds->alloc - (ds->cursor - ds->buf),
	    fmt, ap)) < 0 || ret >= ds->alloc)
		ds->err = errno;
	else {
		ds->len += ret;
		ds->cursor = (ds->buf + ds->len);
	}
	va_end(ap);
}

const char *
dynstr_get(struct dynstr *ds)
{
	if (ds == NULL)
		return NULL;
	if (ds->err) {
		errno = ds->err;
		return NULL;
	}
	return ds->buf;
}
