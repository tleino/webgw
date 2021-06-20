#include "extern.h"
#include "webclient.h"
#include "client.h"
#include "server.h"
#include "hostdb.h"
#include "rules.h"

#include <assert.h>
#include <err.h>
#include <syslog.h>
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>

static void		 acceptclient(struct webgw *, struct client *);
static void	 	 acceptclient_webserver(struct webgw *,
			    struct client *);
static void		 dotimer(struct webgw *ctx, struct client *);

static struct hostport *
server_find_from_authlist(struct hostport *head, const char *host, int port)
{
	struct hostport *n;

	for (n = head; n != NULL; n = n->next)
		if (strcmp(n->host, host) == 0 && n->port == port)
			break;
	if (n != NULL)
		return n;
	return NULL;
}

int
server_has_authorized(struct webgw *server, const char *host, int port)
{
	return server_find_from_authlist(server->authorized_head, host, port)
	    != NULL;
}

int
server_on_hold(struct webgw *server, const char *host, int port)
{
	struct hostport *n;

	n = server_find_from_authlist(server->unauthorized_head, host, port);
	if (n != NULL && n->hold_timer > 0)
		return 1;
	return 0;
}

struct hostport *
server_iterate_unauthorized(struct webgw *server, struct hostport **iter,
    const char **host, int *port, int *hold)
{
	int diff;
	struct hostport *current;

	if (*iter == NULL)
		*iter = server->unauthorized_head;
	if (*iter == NULL)
		return *iter;
	*host = (*iter)->host;
	*port = (*iter)->port;

	diff = time(0) - (*iter)->hold_timer;
	if (diff >= 30)
		*hold = 0;
	else
		*hold = 30 - diff;

	current = *iter;
	*iter = (*iter)->next;
	return current;	
}

static struct hostport *
server_add_to_authlist(struct hostport **head, const char *host, int port)
{
	struct hostport *n;

	if ((n = malloc(sizeof(struct hostport))) == NULL) {
		syslog(LOG_ERR, "malloc in %s: %m", __FUNCTION__);
		return NULL;
	}
	if (strlcpy(n->host, host, sizeof(n->host)) >= sizeof(n->host) ||
	    port > USHRT_MAX) {
		syslog(LOG_WARNING,
		    "bogus data while adding to unauthorized list");
		free(n);
		return NULL;
	}

	n->port = port;
	n->next = *head;
	if (*head != NULL)
		(*head)->prev = n;
	n->prev = NULL;
	*head = n;

	return n;
}

int
server_hold(struct webgw *server, const char *host, int port)
{
	struct hostport *n;

	n = server_find_from_authlist(server->unauthorized_head, host, port);
	if (n == NULL) {
		n = server_add_to_authlist(&server->unauthorized_head, host,
		    port);
		if (n != NULL) {
			n->hold_timer = time(0);
			return n->hold_timer;
		}
		return 0;
	} else {
		if (time(0) - n->hold_timer >= 30)
			return 0;
		return 30 - (time(0) - n->hold_timer);
	}
}

void
server_unauthorize(struct webgw *server, const char *host, int port)
{
	struct hostport *n;

	n = server_find_from_authlist(server->unauthorized_head, host, port);
	if (n == NULL)
		n = server_add_to_authlist(&server->unauthorized_head,
		    host, port);
	if (n != NULL)
		n->hold_timer = 0;
}

void
server_authorize(struct webgw *server, const char *host, int port)
{
	struct hostport *n;

	if (server_has_authorized(server, host, port))
		return;

	n = server_find_from_authlist(server->unauthorized_head, host,
	    port);
	if (n != NULL) {
		if (n->next != NULL)
			n->next->prev = n->prev;
		if (n->prev != NULL)
			n->prev->next = n->next;
		if (n == server->unauthorized_head)
			server->unauthorized_head = n->next;
		free(n);
	}

	n = server_add_to_authlist(&server->authorized_head, host, port);
}

static void
init_webserver(struct webgw *ctx, const char *addr, int port)
{
	struct kevent changelist[2];
	static struct evcallback callback;

	assert(ctx != NULL);
	assert(addr != NULL);
	assert(port > 0);

	if ((ctx->serverfd_webserver = tcpbind(addr, port)) < 0) 
		err(1, "listening on TCP %s:%d (webserver)", addr, port);
	else
		syslog(LOG_INFO, "listening on %s:%d (webserver) fd=%d",
		    addr, port, ctx->serverfd_webserver);

	callback.readfunc = acceptclient_webserver;

	EV_SET(&changelist[0], ctx->serverfd_webserver,
	    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &callback);

	if (kevent(ctx->kq, changelist, 1, NULL, 0, NULL) == -1)
		err(1, "adding listening socket to event queue");	
}

void
init(struct webgw *ctx, const char *addr, int port)
{
	struct kevent changelist[2];
	static struct evcallback callback;
	static struct evcallback timercallback;

	assert(ctx != NULL);
	assert(addr != NULL);
	assert(port > 0);

	memset(ctx->serverhostname, '\0', sizeof(ctx->serverhostname));
	if (gethostname(ctx->serverhostname, sizeof(ctx->serverhostname)) ==
	    -1)
		err(1, "gethostname");
	if (ctx->serverhostname[sizeof(ctx->serverhostname)-1] != '\0')
		errx(1, "serverhostname not null terminated");

	syslog(LOG_INFO, "hostname: %s", ctx->serverhostname);

	if ((ctx->serverfd = tcpbind(addr, port)) < 0) 
		err(1, "listening on TCP %s:%d", addr, port);
	else
		syslog(LOG_INFO, "listening on %s:%d (fd=%d)", addr, port,
		    ctx->serverfd);

	if ((ctx->kq = kqueue()) == -1)
		err(1, "setting up event queue");

	callback.readfunc = acceptclient;

	timercallback.readfunc = dotimer;
	timercallback.writefunc = dotimer;

	ctx->refill_queue = 0;

	ctx->server_max_usec = 0;
	ctx->server_sum_usec = 0;
	ctx->server_samples = 0;

	ctx->client_max_usec = 0;
	ctx->client_sum_usec = 0;
	ctx->client_samples = 0;

	ctx->target_max_usec = 0;
	ctx->target_sum_usec = 0;
	ctx->target_samples = 0;

	ctx->resolv_max_usec = 0;
	ctx->resolv_sum_usec = 0;
	ctx->resolv_samples = 0;

	ctx->request_size_max = 0;
	ctx->request_size_sum = 0;
	ctx->request_size_samples = 0;

	EV_SET(&changelist[0], ctx->serverfd,
	    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &callback);

#if 0
	EV_SET(&changelist[1], 1,
	    EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, 5000, &timercallback);
#endif

	if (kevent(ctx->kq, changelist, 1, NULL, 0, NULL) == -1)
		err(1, "adding listening socket to event queue");

	init_webserver(ctx, addr, 8080);

	ctx->hostdb = hostdb_create();
	rules_load();
}

static void
dotimer(struct webgw *ctx, struct client *client)
{
	syslog(LOG_INFO, "server timer tick");
}

void
server_dispatch_events(struct webgw *ctx)
{
	static struct kevent evlist[QUEUE_DEPTH];
	struct evcallback *callback;
	struct kevent *ev;
	int i, nevents;

	nevents = kevent(ctx->kq, NULL, 0, evlist, QUEUE_DEPTH, NULL);
	if (nevents == -1)
		err(1, "reading events from event queue");

	/*
	 * We need to keep QUEUE_DEPTH as 1 until we fix the situation
	 * where a client is deleted from a callback while still having
	 * pending callbacks.
	 */
	for (i = 0; i < nevents; i++) {
		ev = &evlist[i];

		callback = ev->udata;
		if (ev->filter == EVFILT_READ)
			callback->readfunc(ctx, callback->client);
		else if (ev->filter == EVFILT_WRITE)
			callback->writefunc(ctx, callback->client);
		else if (ev->filter == EVFILT_TIMER)
			callback->readfunc(ctx, callback->client);
		else
			assert(0);

		/*
		 * We need this hack until we implement a more elegant
		 * solution for breaking this event-callback loop in
		 * a situation where one callback causes a client
		 * removal but the queue has another pending callback for the
		 * same client.
		 */
		if (ctx->refill_queue == 1) {
			ctx->refill_queue = 0;
			break;
		}
	}
}

static void
acceptclient_webserver(struct webgw *ctx, struct client *client)
{
	int fd;
	struct sockaddr_in a;
	socklen_t sz = sizeof(a);
	char *addr;

	syslog(LOG_INFO, "acceptclient_webserver");

	if ((fd = accept(ctx->serverfd_webserver,
	    (struct sockaddr *) &a, &sz)) == -1) {
		syslog(LOG_ERR, "accept: %m");
		return;
	}

	/*
	 * If we're full, we simply start dropping connections.
	 */
	if (ctx->nclient == MAX_CLIENTS) {
		syslog(LOG_ERR, "dropped connection (max clients reached)");
		close(fd);
		return;
	}

	client = calloc(1, sizeof(struct client));
	if (client == NULL) {
		syslog(LOG_ERR, "couldn't allocate client: %m");
		close(fd);
		return;
	}
	ctx->nclient++;

	addr = inet_ntoa(a.sin_addr);
	mkrid(client);
	syslog(LOG_INFO, "[%s] new client (webserver) fd=%d ip=%s",
	    client->rid, fd, addr);

	webclient_init(ctx, client, fd);
}

static void acceptclient(struct webgw *ctx, struct client *client)
{
	int fd;
	struct sockaddr_in a;
	socklen_t sz = sizeof(a);
	int usec;
	char *addr;

	struct timespec tv_before, tv_after;

	clock_gettime(CLOCK_MONOTONIC, &tv_before);

	if ((fd = accept(ctx->serverfd, (struct sockaddr *) &a, &sz)) == -1) {
		syslog(LOG_ERR, "accept: %m");
		return;
	}

	/*
	 * If we're full, we simply start dropping connections.
	 */
	if (ctx->nclient == MAX_CLIENTS) {
		syslog(LOG_ERR, "dropped connection (max clients reached)");
		close(fd);
		return;
	}

	client = calloc(1, sizeof(struct client));
	if (client == NULL) {
		syslog(LOG_ERR, "couldn't allocate client: %m");
		close(fd);
		return;
	}
	ctx->nclient++;

	addr = inet_ntoa(a.sin_addr);
	mkrid(client);
	syslog(LOG_INFO, "[%s] new client fd=%d ip=%s",
	    client->rid, fd, addr);

	initclient(client, fd, ctx);

	clock_gettime(CLOCK_MONOTONIC, &tv_after);

	usec = (tv_after.tv_nsec - tv_before.tv_nsec) / 1000;
	if (usec > ctx->server_max_usec)
		ctx->server_max_usec = usec;
	ctx->server_sum_usec += usec;
	ctx->server_samples++;

}
