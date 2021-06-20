#include "extern.h"
#include "client.h"
#include "server.h"
#include "http.h"
#include "dynstr.h"
#include "hostdb.h"
#include "host.h"
#include "rules.h"

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

static void	 webclient_read(struct webgw *, struct client *);
static void	 webclient_write_response(struct webgw *, struct client *,
		    int, const char *);
static void	 webclient_list_unauthorized(struct webgw *, struct client *);
static void	 webclient_redirect(struct webgw *, struct client *);

void
webclient_init(struct webgw *ctx, struct client *client, int fd)
{
	struct kevent changelist[2];

	client->type = CLIENT_WEBSERVER;
	client->fd = fd;
	client->targetfd = -1;

	client->parser.type = HTTP_REQUEST;
	client->parser.state = HTTP_STARTLINE;

	client->clientcallback.client = client;
	client->clientcallback.readfunc = webclient_read;

	EV_SET(&changelist[0], client->fd,
	    EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
	    &client->clientcallback);

	if (kevent(ctx->kq, changelist, 1, NULL, 0, NULL) == -1) {
		syslog(LOG_ERR, "adding listening socket to event queue: %s",
		    strerror(errno));
	}
}

static void
webclient_read(struct webgw *ctx, struct client *client)
{
	int parsed, n, len;
	char *buf;
	static char line[4096];
	struct http_parser *parser;
	char *host;
	int port;

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
			clientlog(client, LOG_ERR,
			    "parser->state == HTTP_ERROR");
			switch (parser->error_state) {
			case HTTP_HEADER_TOO_LONG:
				write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
				    "A submitted header was too long.\r\n");
				break;
			case HTTP_HEADER_TOO_MANY:
				write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
				    "Too many headers submitted.\r\n");
				break;
			case HTTP_HEADER_PARSE_ERROR:
				write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
				    "Parse error while parsing a header.\r\n");
				break;
			case HTTP_STARTLINE_PARSE_ERROR:
				write_error(client->fd, HTTP_STATUS_BAD_REQUEST,
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
			if (strcmp(parser->path, "/") == 0) {
				webclient_list_unauthorized(ctx, client);
			} else if (strncmp(parser->path, "/authorize/",
			    strlen("/authorize/")) == 0) {
				if (http_parse_hostport(
				    &parser->path[strlen("/authorize/")],
				    &host, &port) == -1) {
					write_error(client->fd,
					    HTTP_STATUS_BAD_REQUEST,
					    "Error parsing hostport.\r\n");
					return;
				}
				syslog(LOG_INFO,
				    "authorize req for host=%s port=%d",
				    host, port);
				host_authorize(hostdb_find(
				    ctx->hostdb, host, port), NULL);
				webclient_redirect(ctx, client);
			} else if (strncmp(parser->path, "/unauthorize/",
			    strlen("/unauthorize/")) == 0) {
				if (http_parse_hostport(
				    &parser->path[strlen("/unauthorize/")],
				    &host, &port) == -1) {
					write_error(client->fd,
					    HTTP_STATUS_BAD_REQUEST,
					    "Error parsing hostport.\r\n");
					return;
				}
				syslog(LOG_INFO,
				    "unauthorize req for host=%s port=%d",
				    host, port);
				host_unauthorize(hostdb_find(
				    ctx->hostdb, host, port));
				webclient_redirect(ctx, client);
			} else if (strncmp(parser->path, "/rules",
			    strlen("/rules")) == 0) {
				printf("was rules path\n");
				webclient_redirect(ctx, client);
			}
		}
	}
}

static void
webclient_redirect(struct webgw *ctx, struct client *client)
{
	webclient_write_response(ctx, client, 200, 
	    "<html>\n"
	    "  <head>\n"
	    "    <meta http-equiv=\"refresh\" "
	    "content=\"0; url=http://192.168.2.2:8080\"/>\n"
	    "  </head>\n"
	    "<body></body>\n"
	    "</html>\n"
	    );
}

static void
webclient_list_unauthorized(struct webgw *ctx, struct client *client)
{
	static struct dynstr dn;
	const char *s;
	struct hostnode *n;
	struct host *host;

	/*
	 * TODO: replace these with a real template system, not hardcoded
	 * strings...
	 */

	dynstr_add(&dn, 
	    "<html>\n"
	    "  <head>\n"
	    "    <title>Authorize targets</title>\n"
	    "  </head>\n"
	    "  <body>\n"
	    "    <h1>Active</h1>\n"
	    "    <ul>\n");

	n = NULL;
	while ((host = hostdb_iterate(ctx->hostdb, &n)) != NULL) {
		if (host_ref_count(host) <= 0)
			continue;

		dynstr_add(&dn,
		    "      <li>\n"
		    "        %s:%d (refs=%d)",
		    host_name(host), host_port(host), host_ref_count(host));

		if (!host_is_authorized(host))
			dynstr_add(&dn,
			    "        "
			    "<a href=\"/authorize/%s:%d\">Authorize</a>\n",
			    host_name(host), host_port(host));

		if (host_is_held(host))
			dynstr_add(&dn,
			    "        <a href=\"/unauthorize/%s:%d\">"
			    "Unauthorize</a>\n", host_name(host),
			    host_port(host));

		dynstr_add(&dn,
		    "      </li>");
	}

	dynstr_add(&dn,
	    "    </ul><h1>Wildcard Rules</h1>\n"
	    "    <form method=\"post\" action=\"/rules\" method=\"post\">\n"
	    "    <textarea>\n");

	dynstr_add(&dn, rules_to_data());

	dynstr_add(&dn,
	    "</textarea><br>\n"
	    "    <input type=\"submit\" value=\"Submit\"></form>\n");

	dynstr_add(&dn,
	    "    </ul><h1>Authorized</h1><ul>\n");

	n = NULL;
	while ((host = hostdb_iterate(ctx->hostdb, &n)) != NULL) {
		if (!host_is_authorized(host) || host_ref_count(host) > 0)
			continue;

		dynstr_add(&dn, "<li>%s:%d", host_name(host), host_port(host));

		dynstr_add(&dn,
		    "<a href=\"/unauthorize/%s:%d\">"
		    "Unauthorize</a>\n", host_name(host), host_port(host));

		dynstr_add(&dn,
		    "      </li>");
	}

	dynstr_add(&dn,
	    "    </ul><h1>Unauthorized</h1><ul>\n");

	n = NULL;
	while ((host = hostdb_iterate(ctx->hostdb, &n)) != NULL) {
		if (host_is_authorized(host) || host_ref_count(host) > 0)
			continue;

		dynstr_add(&dn,
		    "      <li>\n"
		    "        %s:%d",
		    host_name(host), host_port(host));

		dynstr_add(&dn,
		    "        <a href=\"/authorize/%s:%d\">Authorize</a>\n",
		    host_name(host), host_port(host));

		dynstr_add(&dn,
		    "      </li>");
	}

	dynstr_add(&dn,
	    "  </body>\n"
	    "</html>\n");

	if ((s = dynstr_get(&dn)) != NULL)
		webclient_write_response(ctx, client, 200, s);
	else {
		clientlog(client, LOG_ERR, "list_unauthorized: %s",
		    strerror(errno));
		write_error(client->fd, HTTP_STATUS_INTERNAL_ERROR,
		    "Server had trouble constructing content for \n"
		    "listing unauthorized clients.\n");
	}

	dynstr_clear(&dn);
}

static void
webclient_write_response(struct webgw *ctx, struct client *client,
    int code, const char *text)
{
	static char buf[8192];
	int n;
	char datebuf[80];
	struct tm *tm;
	time_t t;
	unsigned long len;

	t = time(0);
	tm = gmtime(&t);
	strftime(datebuf, sizeof(datebuf), "%a, %d %b %Y %T %Z", tm);

	len = strlen(text);
	n = snprintf(buf, sizeof(buf),
	    "HTTP/1.1 %d %s\r\n"
	    "Server: webgw/1.0\r\n"
	    "Date: %s\r\n"
	    "Content-Type: text/html;charset=us-ascii\r\n"
	    "Content-Length: %lu\r\n"
	    "Connection: close\r\n\r\n",
	    code, http_status(code), datebuf, len);
	if (write_fd(client->fd, buf, n) == -1)
		syslog(LOG_ERR, "write_error: %m");
	if (write_fd(client->fd, text, len) == -1)
		syslog(LOG_ERR, "write_error in body: %m");
	removeclient(ctx, client);
}
