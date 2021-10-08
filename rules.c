#include "rules.h"
#include "dynstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <err.h>

struct rules
{
	char *pattern;
	struct rules *next;
};

static void  _add_rule(const char *);
static char *_mk_hostport_str(char *, size_t, const char *, int);
static void  _clear_rules(void);

struct rules *_head;

const char *
rules_match(const char *host, int port)
{
	struct rules *np;
	char hostport[1024];

	_mk_hostport_str(hostport, sizeof(hostport), host, port);

	for (np = _head; np != NULL; np = np->next) {
		if (fnmatch(np->pattern, hostport, 0) == FNM_NOMATCH)
			continue;
		return np->pattern;
	}

	return NULL;
}

void
rules_load_from_data(char *buf)
{
	char *eol, *bol;

	_clear_rules();

	bol = eol = buf;
	do {
		bol = eol;
		eol = strchr(bol, '\n');
		if (eol != NULL)
			*eol = '\0';

		_add_rule(bol);
	} while (eol++ != NULL);	
}

void
rules_load()
{
	char buf[1024];
	FILE *fp;
	const char *file = "rules";

	_clear_rules();

	if ((fp = fopen(file, "r")) == NULL) {
		warn("fopen %s", file);
		return;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		buf[strcspn(buf, "\r\n")] = '\0';
		_add_rule(buf);
	}

	fclose(fp);
}

char *
rules_to_data()
{
	struct rules *np;
	struct dynstr dn = { 0 };
	const char *s;
	char *p;

	for (np = _head; np != NULL; np = np->next)
		dynstr_add(&dn, "%s\n", np->pattern);

	if ((s = dynstr_get(&dn)) == NULL)
		return "";

	p = strdup(s);

	dynstr_clear(&dn);

	return p;
}

static void
_add_rule(const char *pattern)
{
	struct rules *np;

	if ((np = calloc(1, sizeof(struct rules))) == NULL)
		err(1, "_add_rule");
	printf("_add_rule, pattern=%s\n", pattern);
	np->next = _head;
	np->pattern = strdup(pattern);
	_head = np;
}

static char *
_mk_hostport_str(char *dst, size_t dstsz, const char *host, int port)
{
	snprintf(dst, dstsz, "%s:%d", host, port);

	return dst;
}

static void
_clear_rules()
{
	struct rules *np, *next;

	for (np = _head; np != NULL; np = next) {
		next = np->next;
		free(np->pattern);
		free(np);
	}
	_head = NULL;
}
