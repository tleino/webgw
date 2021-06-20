#include <syslog.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include "extern.h"
#include "http.h"

int http_parse_hostport(char *, char **, int *);
static int parse_url(char *, char **, int *, char **);
static int parse_startline(char *, char **, char **);

/*
 * Parse 'host:port' to host, and port.
 * Modifies the original string and uses its memory. Returns -1 on failure.
 */
int
http_parse_hostport(char *hostport, char **host_out, int *port_out)
{
	char *p, *t;

	t = hostport;
	p = strchr(t, ':');
	*host_out = t;
	if (p != NULL) {
		*p++ = '\0';
		if (p != NULL && isdigit(*p))
			*port_out = atoi(p);
		else {
			/*
			 * It is error to have ':' without a port number.
			 */
			return -1;
		}
	}
	/*
	 * No need to handle 'else' here, because if we didn't get ':',
	 * we're fine, it's optional.
	 */

	if (*port_out == 0)
		*port_out = 80;

	return 0;
}

/*
 * Parse 'http://host:port/path' to host, port and path.
 * Modifies the original string and uses its memory. Returns -1 on failure.
 */
static int
parse_url(char *url, char **host_out, int *port_out, char **path_out)
{
	char *p, *t, *hostport;

	p = strstr(url, "://");
	if (p != NULL) {
		t = p + strlen("://");
		p = strchr(t, '/');
		if (p != NULL) {
			*p++ = '\0';
			hostport = t;
			*path_out = p;
		} else
			return -1;
	} else
		return -1;

	if (http_parse_hostport(hostport, host_out, port_out) == -1)
		return -1;

	return 0;
}

/*
 * Parse 'METHOD value HTTP/1.1' to method and uri (value).
 * Modifies the original string and uses its memory. Returns -1 on failure.
 * Ignores protocol version.
 */
static int
parse_startline(char *startline, char **method_out, char **uri_out)
{
	char *p, *t;

	t = startline;
	p = strchr(t, ' ');
	if (p != NULL) {
		*method_out = startline;
		*p++ = '\0';
	} else
		return -1;

	t = p;
	p = strchr(t, ' ');
	if (p != NULL) {
		*uri_out = t;
		*p++ = '\0';
	} else
		return -1;

	return 0;
}

static void
parse_startline_line(struct http_parser *parser, const char *line)
{
	strlcpy(parser->startline, line, sizeof(parser->startline));
	parser->state = HTTP_HEADERS;

	syslog(LOG_INFO, "%s", parser->startline);
	if (parse_startline(parser->startline, &parser->method,
	    &parser->uri) == -1) {
		parser->error_state = HTTP_STARTLINE_PARSE_ERROR;
		return;
	}

#if 1
	syslog(LOG_INFO, "Method: '%s' Uri: '%s'", parser->method,
	    parser->uri);
#endif
			
	if (strcasecmp(parser->method, "CONNECT") == 0) {
		if (http_parse_hostport(parser->uri, &parser->host,
		    &parser->port) == -1) {
			parser->error_state =
			    HTTP_STARTLINE_PARSE_ERROR;
			return;
		}
	} else if (parser->uri != NULL && parser->uri[0] == '/') {
		syslog(LOG_INFO, "local URL");
		parser->path = parser->uri;
	} else {
		if (parse_url(parser->uri, &parser->host,
		    &parser->port, &parser->path) == -1) {
			parser->error_state =
			    HTTP_STARTLINE_PARSE_ERROR;
			return;
		}
	}
#if 1
	syslog(LOG_INFO, "host: %s, port: %d path: %s",
	    parser->host, parser->port, parser->path);
#endif

	parser->state = HTTP_HEADERS;	
}

static void
parse_header_line(struct http_parser *parser, const char *line)
{
	static char key[4096];
	char *p;
	struct http_header *h;

	if (line[0] == '\0') {
		parser->state = HTTP_BODY;
		if (parser->error_state != HTTP_NO_ERROR)
			parser->state = HTTP_ERROR;
		return;
	}
	strlcpy(key, line, sizeof(key));
	p = strchr(key, ':');
	if (p != NULL) {
		*p++ = '\0';
		while (isspace(*p))
			p++;			
	} else {
		parser->error_state = HTTP_HEADER_PARSE_ERROR;
		return;
	}

	if (parser->n_header < 16) {
		h = &parser->header[parser->n_header++];
		if (strlcpy(h->key, key, sizeof(h->key)) >=
		    sizeof(h->key) ||
		    strlcpy(h->value, p, sizeof(h->value)) >=
		    sizeof(h->value)) {
			parser->error_state = HTTP_HEADER_TOO_LONG;
		}
	} else {
		parser->error_state = HTTP_HEADER_TOO_MANY;
	}
	return;
}

static void
parse_body_line(struct http_parser *parser, const char *line)
{

}

static void
http_parse_request(struct http_parser *parser, const char *line)
{
	switch (parser->state) {
	case HTTP_STARTLINE:
		parse_startline_line(parser, line);
		break;
	case HTTP_HEADERS:
		parse_header_line(parser, line);
		break;
	case HTTP_BODY:
		parse_body_line(parser, line);
		break;
	default:
		assert(0);
	}
}

static void
http_parse_response(struct http_parser *parser, const char *line)
{
}

void
http_parse(struct http_parser *parser, const char *line)
{
	switch (parser->type) {
	case HTTP_REQUEST:
		http_parse_request(parser, line);
		break;
	case HTTP_RESPONSE:
		http_parse_response(parser, line);
		break;
	default:
		assert(0);
	}
}
