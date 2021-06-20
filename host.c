#include "host.h"
#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct host
{
	char *name;
	char *pattern;
	int port;
	int visits;
	int rx;
	int tx;
	int is_authorized;
	int active;
};

struct host *
host_create(const char *name, int port, int visits)
{
	struct host *self;

	if ((self = calloc(1, sizeof(struct host))) == NULL)
		err(1, "host_create");

	self->name = strdup(name);
	self->port = port;
	self->visits = visits;

	return self;
}

static char *
_match_prefix_strdup(const char *str, const char *prefix)
{
	size_t len, str_len;

	len = strlen(prefix);
	str_len = strlen(str);
	if (strncmp(str, prefix, len) == 0 && len < str_len)
		return strdup(&str[len]);
	else
		return NULL;
}

struct host *
host_create_from_data(char *buf)
{
	char *s, *name, *eol, *bol, *pattern;
	int port, visits, rx, tx, is_authorized;
	struct host *host;

	bol = eol = buf;
	do {
		bol = eol;
		eol = strchr(bol, '\n');
		if (eol != NULL)
			*eol = '\0';

		if ((s = _match_prefix_strdup(bol, "host ")) != NULL)
			name = s;
		else if ((s = _match_prefix_strdup(bol, "pattern ")) != NULL)
			pattern = s;
		else if ((s = _match_prefix_strdup(bol, "port ")) != NULL)
			port = atoi(s);
		else if ((s = _match_prefix_strdup(bol, "visits ")) != NULL)
			visits = atoi(s);
		else if ((s = _match_prefix_strdup(bol, "rx_bytes ")) != NULL)
			rx = atoi(s);
		else if ((s = _match_prefix_strdup(bol, "tx_bytes ")) != NULL)
			tx = atoi(s);
		else if ((s = _match_prefix_strdup(bol, "is_authorized ")) !=
		    NULL)
			is_authorized = atoi(s);
	} while (eol++ != NULL);

	host = host_create(name, port, visits);
	host->rx = rx;
	host->tx = tx;
	host->is_authorized = is_authorized;
	host->pattern = pattern;

	return host;
}

const char *
host_serialize(struct host *self, char *dst, size_t szdst)
{
	if ((snprintf(dst, szdst,
	    "host %s\n"
	    "port %d\n"
	    "visits %d\n"
	    "rx_bytes %d\n"
	    "tx_bytes %d\n"
	    "is_authorized %d\n"
	    "pattern %s\n",
	    self->name, self->port, self->visits, self->rx, self->tx,
	    self->is_authorized, self->pattern)) >=
	    (int) szdst)
		errx(1, "host_serialize: truncated");

	return dst;
}

void
host_free(struct host *self)
{
	free(self->name);
	free(self->pattern);
	free(self);
}

const char *
host_name(struct host *self)
{
	return self->name;
}

int
host_port(struct host *self)
{
	return self->port;
}

int
host_visits(struct host *self)
{
	return self->visits;
}

void
host_incr_visits(struct host *self)
{
	self->visits++;
}

void
host_add_rx_bytes(struct host *self, int bytes)
{
	self->rx += bytes;
}

void
host_add_tx_bytes(struct host *self, int bytes)
{
	self->tx += bytes;
}

int
host_rx_bytes(struct host *self)
{
	return self->rx;
}

int
host_tx_bytes(struct host *self)
{
	return self->tx;
}

void
host_authorize(struct host *self, const char *pattern)
{
	self->is_authorized = 1;
	if (pattern != NULL)
		self->pattern = strdup(pattern);
}

void
host_unauthorize(struct host *self)
{
	self->is_authorized = -1;
}

const char *
host_pattern(struct host *self)
{
	return self->pattern;
}

int
host_is_authorized(struct host *self)
{
	return (self->is_authorized == 1);
}

int
host_is_held(struct host *self)
{
	return (self->is_authorized == 0);
}

void
host_ref(struct host *self)
{
	self->active++;
}

void
host_unref(struct host *self)
{
	self->active--;
}

int
host_ref_count(struct host *self)
{
	return self->active;
}
