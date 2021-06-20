#ifndef EXTERN_H
#define EXTERN_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <asr.h>
#include <stddef.h>
#include <time.h>
#include "config.h"

enum http_type
{
	HTTP_REQUEST,
	HTTP_RESPONSE
};

enum http_state
{
	HTTP_STARTLINE,
	HTTP_HEADERS,
	HTTP_BODY,
	HTTP_ERROR
};

enum http_error_state
{
	HTTP_NO_ERROR,
	HTTP_HEADER_TOO_MANY,
	HTTP_HEADER_TOO_LONG,
	HTTP_HEADER_PARSE_ERROR,
	HTTP_STARTLINE_PARSE_ERROR
};

struct http_header
{
	char key[64];
	char value[1024];
};

struct http_parser
{
	int type;
	int state;
	int error_state;

	char buf[4096];
	int sz;
	int pos;

	char startline[4096];

	struct http_header header[16];
	int n_header;

	/*
	 * These points to startline.
	 */
	char *method;
	char *uri;
	char *host;
	int port;
	char *path;
};

struct webgw;

void
http_parse(struct http_parser *parser, const char *line);

size_t
parseline(char *base, char *dst, size_t dstsz);

struct evcallback
{
	struct client *client;
	void (*readfunc)(struct webgw *, struct client *);
	void (*writefunc)(struct webgw *, struct client *);
};

enum client_type
{
	CLIENT_PROXY,
	CLIENT_WEBSERVER
};

struct host;

typedef struct client
{
	int fd;
	int targetfd;
	int targetconnected;

	int request_size;

	struct http_parser parser;

	int bytes_in;	/* Statistics for logs */
	int bytes_out;

	char rid[8 + 1]; /* random id */

	char buf[4096];
	int sz;

	char host[256];
	char from_host[256];

	char verb[7 + 1];	/* Current method: GET, POST, etc. */
	char verb_args[256];
	int content_length;
	int have_separator;
	int nbuf;

	int bytes_from_target;
	int bytes_from_client;

	int type;

	struct host *target_host;

	/*
	 * We have three things to poll for:
	 * - reading from/writing to client;
	 * - reading from/writing to target;
	 * - reading from/writing to DNS.
	 *
	 * If any of these is unused, we set pollfd[n].fd to -1.
	 */
	struct evcallback clientcallback;
	struct evcallback targetcallback;
	struct evcallback resolvcallback;
	struct evcallback timercallback;
	struct evcallback reprocesscallback;

	struct asr_query *asr_query;

	struct timespec ts_begin;
	struct timespec ts_connect;
	struct timespec ts_end;
	struct timespec ts_firstbyte;
} Client;

struct webgw;

#include <sys/param.h>

struct hoststats
{
	unsigned int bytes_sent;
	unsigned int bytes_received;
	unsigned int num_requests;
};

struct hostport
{
	char host[256];
	unsigned short port;
	struct hoststats stats;
	int state;
	int hold_timer;
	struct hostport *next;
	struct hostport *prev;
};

struct hostdb;

struct webgw
{
	int serverfd;
	int serverfd_webserver;

	struct hostport *authorized_head;
	struct hostport *unauthorized_head;

	char serverhostname[MAXHOSTNAMELEN];

	int kq;		/* kqueue descriptor */

	int nclient;

	int refill_queue;

	/* statistics */
	int server_max_usec;
	int server_sum_usec;
	int server_samples;

	int client_max_usec;
	int client_sum_usec;
	int client_samples;

	int target_max_usec;
	int target_sum_usec;
	int target_samples;

	int resolv_max_usec;
	int resolv_sum_usec;
	int resolv_samples;

	int request_size_max;
	int request_size_sum;
	int request_size_samples;

	struct hostdb *hostdb;
};

void
init(struct webgw *ctx, const char *addr, int port);

void
initclient(struct client *, int, struct webgw *);

int tcpbind(const char *ip, int port);

void server_dispatch_events(struct webgw *ctx);

void collect_workers(void);

int read_client(Client *client);

#include <stddef.h>

void
send_response(Client *client, int code, const char *head,
	const char *extra_headers, const char *body);

#endif
