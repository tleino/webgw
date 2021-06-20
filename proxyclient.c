#include <sys/select.h>
#include <stddef.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <netdb.h>
#include <err.h>
#include <fnmatch.h>
#include <stdio.h>
#include <sys/wait.h>
#include <assert.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/event.h>

#include "extern.h"
#include "server.h"
#include "config.h"
#include "hostdb.h"
#include "host.h"
#include "rules.h"
#include "client.h"

static void			 readclient(struct webgw *, struct client *);
static void			 resolv(struct webgw *, struct client *);
static void			 readtarget(struct webgw *, struct client *);
static void			 reprocess_body(struct webgw *,
				    struct client *);
static void			 dotimer(struct webgw *, struct client *);
static void			 connect_completed(struct webgw *,
				    struct client *);
static void			 reprocess_body(struct webgw *,
				    struct client *);

static void
removeclient_with_error(struct webgw *ctx, struct client *client, int err)
{
	static const struct { int code; char *msg; } errmsg[] = {
		/* CLIENT_ERR_TRUNCATED_STARTLINE */
		{
		.code = HTTP_STATUS_BAD_REQUEST,
		.msg =
		    "<p>\r\n"
		    "  The client program sent an unreasonably long\r\n"
		    "  HTTP startline.\r\n"
		    "</p>\r\n"
		    "<p>\r\n"
		    "  The problem might be caused by an malformed request,\r\n"
		    "  unreasonable assumptions in the web site's code, or\r\n"
		    "  by a user error.\r\n"
		    "</p>\r\n"
		    "<p>\r\n"
		    "  In some cases, the proxy server needs to be\r\n"
		    "  reconfigured or updated.\r\n"
		    "</p>\r\n"
		},
		/* CLIENT_ERR_ */
	};

	write_error(client->fd, errmsg[err].code, errmsg[err].msg);
	removeclient(ctx, client);
}

void
initclient(struct client *client, int fd, struct webgw *ctx)
{
	struct kevent changelist[2];

	client->fd = fd;
	client->targetfd = -1;
	client->request_size = 0;
	client->parser.n_header = 0;

	clock_gettime(CLOCK_MONOTONIC, &client->ts_begin);

	client->parser.type = HTTP_REQUEST;
	client->parser.state = HTTP_STARTLINE;

#if 0
	if (fcntl(client->fd, F_SETFL, O_NONBLOCK) == -1)
		err(1, "while setting non-blocking I/O");
#endif

	client->clientcallback.client = client;
	client->clientcallback.readfunc = readclient;

	client->targetcallback.client = client;
	client->targetcallback.readfunc = readtarget;
	client->targetcallback.writefunc = connect_completed;

	client->resolvcallback.client = client;
	client->resolvcallback.readfunc = resolv;
	client->resolvcallback.writefunc = resolv;

	client->timercallback.client = client;
	client->timercallback.readfunc = dotimer;

	client->reprocesscallback.client = client;
	client->reprocesscallback.readfunc = reprocess_body;

	EV_SET(&changelist[0], client->fd,
	    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
	    &client->clientcallback);

#if 0
	idle_timeout_ms = 60000;
	EV_SET(&changelist[1], client->fd,
	    EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
	    0, idle_timeout_ms, &client->timercallback);
#endif

	if (kevent(ctx->kq, changelist, 1, NULL, 0, NULL) == -1)
		err(1, "adding listening socket to event queue");
}



static void
client_resolve(struct webgw *ctx, struct client *client, const char *host)
{
	client->asr_query = gethostbyname_async(host, NULL);

	resolv(ctx, client);
}

static void
dotimer(struct webgw *ctx, struct client *client)
{
	if (client->parser.state != HTTP_BODY || client->targetconnected == 0) {
		clientlog(client, LOG_INFO, "client fd=%d timeout", client->fd);
		removeclient(ctx, client);
	}
}

static int
write_header(struct webgw *ctx, struct client *client,
    const char *key, const char *value)
{
	int len;
	char line[1024];

	len = snprintf(line, sizeof(line), "%s: %s\r\n",
	    key, value);
	clientlog(client, LOG_INFO, "write header %s", line);
	if (len >= sizeof(line)) {
		clientlog(client, LOG_ERR, "truncated header line");
		write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
		    "Truncated header line.\r\n");
		removeclient(ctx, client);
		return -1;
	}
	if (write_fd(client->targetfd, line, len) == -1) {
		clientlog(client, LOG_ERR, "write on header line: %s",
		    strerror(errno));
		write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
		    "Write on header line.\r\n");
		removeclient(ctx, client);
		return -1;
	}
	return 0;
}

static void
connect_completed(struct webgw *ctx, struct client *client)
{
	struct kevent changelist;
	char line[1024];
	int error, i, len;
	socklen_t socklen;

	if (getsockopt(client->targetfd, SOL_SOCKET, SO_ERROR, &error, &socklen)
	    == -1) {
		clientlog(client, LOG_ERR, "connect %s:%d: %s",
		    client->parser.host, client->parser.port, strerror(errno));
		write_error(client->fd, HTTP_STATUS_FAILED_CONNECTION,
		    "Failed to connect.\r\n");
		removeclient(ctx, client);
		return;
	}

	clientlog(client, LOG_INFO, "connected %s:%d via %s",
	    client->parser.host, client->parser.port, client->parser.method);
	clock_gettime(CLOCK_MONOTONIC, &client->ts_connect);

	client->targetconnected = 1;

	if (fcntl(client->targetfd, F_SETFL, 0) == -1) {
		clientlog(client, LOG_ERR, "fcntl: %s", strerror(errno));
		write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
		    "Failed to unset non-blocking socket.\r\n");
		removeclient(ctx, client);
		return;
	}

	if (strcmp(client->parser.method, "CONNECT") != 0) {
		len = snprintf(line, sizeof(line), "%s /%s HTTP/1.1\r\n",
		    client->parser.method, client->parser.path);
		if (len >= sizeof(line)) {
			clientlog(client, LOG_ERR, "truncated startline");
			removeclient_with_error(ctx, client,
			    CLIENT_ERR_TRUNCATED_STARTLINE);
			return;
		}
		if (write_fd(client->targetfd, line, len) == -1) {
			clientlog(client, LOG_ERR, "write on startline: %s",
			    strerror(errno));
			write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
			    "Write on startline.\r\n");
			removeclient(ctx, client);
			return;
		}
		/*
		 * Write headers.
		 */
		for (i = 0; i < client->parser.n_header; i++) {
			if (strcasecmp(client->parser.header[i].key,
			    "Proxy-Connection") == 0)
				continue;
			if (write_header(ctx, client,
			    client->parser.header[i].key,
			    client->parser.header[i].value) == -1)
				return;
		}
		snprintf(line, sizeof(line), "for=_%s", client->rid);
		if (write_header(ctx, client, "Forwarded", line) == -1)
			return;

		if (write_fd(client->targetfd, "\r\n", 2) == -1) {
			clientlog(client, LOG_ERR, "write on body separator: %s",
			    strerror(errno));
			write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
			    "Write on body separator.\r\n");
			removeclient(ctx, client);
			return;
		}
	} else {
		for (i = 0; i < client->parser.n_header; i++) {
			if (strcasecmp(client->parser.header[i].key,
			    "Referer") == 0) {
				clientlog(client, LOG_INFO, "Referer: %s",
				    client->parser.header[i].value);
				break;
			}
		}

#define SUCCESS_REPLY "HTTP/1.1 200 Connection Established\r\n\r\n"
		if (write_fd(client->fd, SUCCESS_REPLY, strlen(SUCCESS_REPLY)) == -1) {
			clientlog(client, LOG_ERR, "write on success reply: %s",
			    strerror(errno));
			removeclient(ctx, client);
			return;
		}
	}

	EV_SET(&changelist, client->targetfd,
	    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
	    &client->targetcallback);

	if (kevent(ctx->kq, &changelist, 1, NULL, 0, NULL) == -1)
		err(1, "adding targetfd to evset");
}

static void
resolv(struct webgw *ctx, struct client *client)
{
	struct asr_result r;
	struct kevent changelist;
	struct hostent *h;
	struct timespec tv_before, tv_after;
	int usec;

	clock_gettime(CLOCK_MONOTONIC, &tv_before);

	if (asr_run(client->asr_query, &r) == 0) {
		if (r.ar_cond == ASR_WANT_READ)
			EV_SET(&changelist, r.ar_fd,
			    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
			    &client->resolvcallback);
		else
			EV_SET(&changelist, r.ar_fd,
			    EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
			    &client->resolvcallback);

		if (kevent(ctx->kq, &changelist, 1, NULL, 0, NULL) == -1)
			err(1, "adding resolv to kevent");
	} else {
		struct in_addr **pptr;
		struct sockaddr_in sa;

		client->asr_query = NULL;

		if (r.ar_h_errno != 0 || r.ar_hostent == NULL) {
			clientlog(client, LOG_WARNING, "resolv %s: %s",
			    client->parser.host, hstrerror(r.ar_h_errno));
			write_error(client->fd, HTTP_STATUS_SERVICE_UNAVAILABLE,
			    "Proxy failed to resolve host.\r\n");
			removeclient(ctx, client);
			return;
		}

		h = r.ar_hostent;
		pptr = (struct in_addr **) h->h_addr_list;
		sa.sin_family = AF_INET;
		sa.sin_port = htons(client->parser.port);
		memcpy(&sa.sin_addr, *pptr, sizeof(struct in_addr));
		free(h);

#if 0
		clientlog(client, LOG_INFO, "Host: %s ip: %s", h->h_name, 
		    inet_ntoa(*((struct in_addr *)h->h_addr)));
#endif

		if ((client->targetfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			clientlog(client, LOG_ERR, "socket: %s",
			    strerror(errno));
			write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
			    "Failed to create socket for connection.\r\n");
			removeclient(ctx, client);
			return;
		}

		if (fcntl(client->targetfd, F_SETFL, O_NONBLOCK) == -1) {
			clientlog(client, LOG_ERR, "fcntl: %s",
			    strerror(errno));
			write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
			    "Failed to set non-blocking socket\r\n");
			removeclient(ctx, client);			
			return;
		}

		if (connect(client->targetfd,
		    (struct sockaddr *) &sa, sizeof(sa)) < 0 &&
		    errno != EINPROGRESS) {
			clientlog(client, LOG_WARNING, "connect %s:%d: %s",
			    client->parser.host, client->parser.port,
			    strerror(errno));
			write_error(client->fd, HTTP_STATUS_FAILED_CONNECTION,
			    "Failed to connect.\r\n");
			removeclient(ctx, client);
			return;
		}
		if (errno == EINPROGRESS) {
			EV_SET(&changelist, client->targetfd,
			    EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0,
			    &client->targetcallback);
			if (kevent(ctx->kq, &changelist, 1, NULL, 0, NULL) == -1)
				err(1, "adding targetfd to kevent");
		} else {
			clientlog(client, LOG_INFO, "immediate connect ok");
			connect_completed(ctx, client);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &tv_after);
	usec = (tv_after.tv_nsec - tv_before.tv_nsec) / 1000;
	if (usec > ctx->resolv_max_usec)
		ctx->resolv_max_usec = usec;
	ctx->resolv_sum_usec += usec;
	ctx->resolv_samples++;
}

static int
process_body(struct webgw *ctx, struct client *client);

static void
reprocess_body(struct webgw *ctx, struct client *client)
{
	(void) process_body(ctx, client);	
}

static int
process_body(struct webgw *ctx, struct client *client)
{
	struct http_parser *parser;
	struct kevent changelist;

	parser = &client->parser;

	if (parser->port != 443 && parser->port != 80 &&
	    parser->port != 8080) {
		clientlog(client, LOG_ERR, "Illegal port %d", parser->port);
		write_error(client->fd, HTTP_STATUS_FORBIDDEN,
		    "Illegal port.\r\n");
		removeclient(ctx, client);
		return -1;
	}

	client->target_host = 
	    hostdb_find(ctx->hostdb, parser->host, parser->port);

	if (host_is_authorized(client->target_host) == 0) {
		if (host_is_held(client->target_host) == 0) {
			clientlog(client, LOG_WARNING,
			    "tried to connect: %s (unauthorized)",
			    parser->host);
			server_unauthorize(ctx, parser->host, parser->port);
			write_error(client->fd, HTTP_STATUS_FORBIDDEN,
			    "Illegal host.\r\n");
			removeclient(ctx, client);
		} else {
			clientlog(client, LOG_WARNING,
			    "tried to connect: %s (holding)", parser->host);
			EV_SET(&changelist, client->fd,
			    EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
			    0, 1000, &client->reprocesscallback);
			if (kevent(ctx->kq, &changelist, 1, NULL, 0, NULL) ==
			    -1)
				err(1, "adding listening socket "
				    "to event queue");
		}
		return -1;
	}
	if (strcmp(parser->method, "CONNECT") == 0) {
		client_resolve(ctx, client, parser->host);
	} else if (strcmp(parser->method, "GET") == 0 ||
		    strcmp(parser->method, "POST") == 0 ||
		    strcmp(parser->method, "HEAD") == 0 ||
		    strcmp(parser->method, "PUT") == 0 ||
		    strcmp(parser->method, "DELETE") == 0 ||
		    strcmp(parser->method, "OPTIONS") == 0 ||
		    strcmp(parser->method, "PATCH") == 0) {
		client_resolve(ctx, client, parser->host);
	} else {
		clientlog(client, LOG_ERR, "Unsupported method");
		write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
		    "Unsupported method.\r\n");
		removeclient(ctx, client);
		return -1;
	}
	return 0;
}

static void
readclient(struct webgw *ctx, struct client *client)
{
	int parsed, n, len;
	char *buf;
	static char line[4096];
	struct http_parser *parser;
	struct timespec tv_before, tv_after;
	int usec;
	const char *s;

	clock_gettime(CLOCK_MONOTONIC, &tv_before);

	parser = &client->parser;

	len = sizeof(client->buf) - 1 - client->sz;
	if (len <= 0) {
		client->sz = 0;
		clientlog(client, LOG_ERR, "discarded bytes; too long line");
		write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
		    "Too long line.\r\n");
		removeclient(ctx, client);
		return;
	}
	buf = &client->buf[client->sz];
	n = read(client->fd, buf, len);
	if (n <= 0) {
		if (n < 0)
			clientlog(client, LOG_WARNING, "read client: %s",
			    strerror(errno));
		else
			clientlog(client, LOG_WARNING, "read client: EOF");
		removeclient(ctx, client);
		return;
	}
	client->bytes_from_client += n;

	client->sz += n;
	buf[n] = '\0';

	if (parser->state != HTTP_BODY) {
		while ((parsed = parseline(client->buf, line, sizeof(line))) !=
		    -1) {
			client->sz -= parsed;
			client->request_size += parsed;
			http_parse(parser, line);
		}

		if (parser->state == HTTP_ERROR) {
			clientlog(client, LOG_ERR, "parser->state == "
			    "HTTP_ERROR");
			switch (parser->error_state) {
			case HTTP_HEADER_TOO_LONG:
				write_error(client->fd,
				    HTTP_STATUS_BAD_REQUEST,
				    "A submitted header was too long.\r\n");
				break;
			case HTTP_HEADER_TOO_MANY:
				write_error(client->fd,
				    HTTP_STATUS_BAD_REQUEST,
				    "Too many headers submitted.\r\n");
				break;
			case HTTP_HEADER_PARSE_ERROR:
				write_error(client->fd,
				    HTTP_STATUS_BAD_REQUEST,
				    "Parse error while parsing a header.\r\n");
				break;
			case HTTP_STARTLINE_PARSE_ERROR:
				write_error(client->fd,
				    HTTP_STATUS_BAD_REQUEST,
				    "Parse error while parsing startline.\r\n");
				break;
			default:
				write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
				    "Invalid request.\r\n");
			}
			removeclient(ctx, client);
			return;
		}
		if (parser->state == HTTP_BODY) {
			client->target_host = 
			    hostdb_find(ctx->hostdb, parser->host,
			    parser->port);

			host_ref(client->target_host);

			if ((s = rules_match(parser->host, parser->port)) !=
			    NULL)
				host_authorize(client->target_host, s);

			if (process_body(ctx, client) == -1)
				return;
		}
	} else {
		/*
		 * If we've already established connection with a target,
		 * we simply forward data without parsing.
		 */
		if (client->targetfd != -1 && client->targetconnected == 1) {
			if (write_fd(client->targetfd, buf, n) == -1) {
				clientlog(client, LOG_ERR,
				    "write to targetfd: %s",
				    strerror(errno));
				removeclient(ctx, client);
				return;
			}
			client->sz = 0;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &tv_after);

	usec = (tv_after.tv_nsec - tv_before.tv_nsec) / 1000;
	if (usec > ctx->client_max_usec)
		ctx->client_max_usec = usec;
	ctx->client_sum_usec += usec;
	ctx->client_samples++;
}

void
readtarget(struct webgw *ctx, Client *client)
{
	static char buf[READ_BLOCK_SZ];
	int n;
	struct timespec tv_before, tv_after;
	int usec;

	clock_gettime(CLOCK_MONOTONIC, &tv_before);

	if ((n = read(client->targetfd, buf, sizeof(buf))) <= 0) {
		if (n < 0)
			clientlog(client, LOG_WARNING,
			    "read target: %s", strerror(errno));
		if (n == 0)
			clientlog(client, LOG_INFO, "read target: EOF");

		removeclient(ctx, client);
		return;
	}

	if (client->bytes_from_target == 0) {
		clock_gettime(CLOCK_MONOTONIC, &client->ts_firstbyte);
	}
	client->bytes_from_target += n;

	host_add_rx_bytes(client->target_host, n);

	if ((n = write_fd(client->fd, buf, n)) < 0) {
		clientlog(client, LOG_ERR, "write: %s", strerror(errno));
		removeclient(ctx, client);
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &tv_after);

	usec = (tv_after.tv_nsec - tv_before.tv_nsec) / 1000;
	if (usec > ctx->target_max_usec)
		ctx->target_max_usec = usec;
	ctx->target_sum_usec += usec;
	ctx->target_samples++;
}
