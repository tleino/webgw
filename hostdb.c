#include "hostdb.h"
#include "host.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>

struct hostnode
{
	struct host *host;
	struct hostnode *next;
};

struct hostdb
{
	struct hostnode *head;
	int loaded;
};

static struct hostnode *_add_new_host(struct hostdb *, struct host *);
static void             _free_hostnode(struct hostnode *);

static void             _load_hostdb(struct hostdb *);
static void             _save_hostdb(struct hostdb *);

struct hostdb*
hostdb_create()
{
	struct hostdb *self;

	if ((self = calloc(1, sizeof(struct hostdb))) == NULL)
		err(1, "hostdb_create");

	return self;
}

void
hostdb_free(struct hostdb *self)
{
	struct hostnode *np, *next;

	for (np = self->head; np != NULL; np = next) {
		host_free(np->host);
		next = np->next;
		_free_hostnode(np);
	}
	self->head = NULL;
	free(self);
}

struct host*
hostdb_find(struct hostdb *self, const char *name, int port)
{
	struct hostnode *np;
	struct host *host;

	if (self->loaded == 0) {
		_load_hostdb(self);
		self->loaded = 1;
	}

	for (np = self->head; np != NULL; np = np->next) {
		host = np->host;

		if (strcmp(host_name(host), name) == 0 &&
		    host_port(host) == port) {
			host_incr_visits(host);
			return host;
		}
	}

	host = host_create(name, port, 0);
	_add_new_host(self, host);
	return host;
}

struct host*
hostdb_iterate(struct hostdb *self, struct hostnode **n)
{
	if (*n == NULL)
		*n = self->head;
	else
		*n = (*n)->next;

	if (*n == NULL)
		return NULL;
	else
		return (*n)->host;
}

static struct hostnode*
_add_new_host(struct hostdb *hostdb, struct host *host)
{
	struct hostnode *self;

	if ((self = calloc(1, sizeof(struct hostnode))) == NULL)
		err(1, "hostnode_create");
	self->host = host;
	self->next = hostdb->head;
	hostdb->head = self;

	_save_hostdb(hostdb);

	return self;
}

static void
_free_hostnode(struct hostnode *self)
{
	free(self);
}

static void
_load_hostdb(struct hostdb *self)
{
	FILE *fp;
	const char *file = "known_hosts";
	struct host *host;
	char line[256], buf[1024];

	if ((fp = fopen(file, "r")) == NULL) {
		warn("_load_hostdb: %s", file);
		return;
	}

	buf[0] = '\0';
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (line[0] == '\n') {
			host = host_create_from_data(buf);
			_add_new_host(self, host);
			buf[0] = '\0';
		} else {
			if (strlcat(buf, line, sizeof(buf)) >= sizeof(buf))
				errx(1, "_load_hostdb: truncated");
		}
	}

	fclose(fp);
}

static void
_save_hostdb(struct hostdb *self)
{
	FILE *fp;
	const char *file = "known_hosts";
	struct hostnode *np;
	char dst[1024];

	if ((fp = fopen(file, "w")) == NULL)
		err(1, "_save_hostdb: %s", file);

	for (np = self->head; np != NULL; np = np->next)
		fprintf(fp, "%s\n",
		    host_serialize(np->host, dst, sizeof(dst)));

	fclose(fp);
}
