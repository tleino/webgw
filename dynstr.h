#ifndef DYNSTR_H
#define DYNSTR_H

#include <stddef.h>

/*
 * Dynamic string. We could have used open_memstream and use FILE *,
 * but this dynstr has simpler semantics for the user, because we can
 * check for error at the end, not after every call to write(2) or
 * fprintf(3).
 *
 * Preferred operation is to use staticly allocated container object
 * (struct dynstr) because this saves from additional error management.
 * Alternatively the user can use dynstr_create() and dynstr_free().
 *
 * Some compromises for convenience:
 * - Lazy evaluation of errors (use open_memstream(3)).
 * - No arbitrary seek (use open_memstream(3)).
 *
 * char *s;
 * static struct dynstr dn;
 *
 * dynstr_add(&dn, "foobar %d", 3);
 * dynstr_add(&dn, "barfoo %s", "foobar");
 * if ((s = dynstr_get(&dn)) != NULL)
 *     s = strdup(s);
 * else
 *     err(1, "dynstr_get");
 * dynstr_clear(&dn);
 */

#ifndef DYNSTR_INITIAL_ALLOC
#define DYNSTR_INITIAL_ALLOC	256
#endif

struct dynstr;

void           dynstr_add    (struct dynstr *, const char *, ...);
const char    *dynstr_get    (struct dynstr *);
void           dynstr_clear  (struct dynstr *);

struct dynstr *dynstr_create (void);
void           dynstr_free   (struct dynstr *);

/*
 * Private.
 */
struct dynstr
{
	size_t len;
	size_t alloc;
	char *buf;
	char *cursor;
	int err;
};

#endif
