#include "extern.h"
#include "client.h"
#include "host.h"

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <syslog.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>
#include <string.h>
#include <assert.h>

void
mkrid(struct client *client)
{
	int i;
	static const char alphabet[] =
	    "01234567890"
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZ_"
	    "abcdefghijklmnopqrstuvwxyz-";

	for (i = 0; i < sizeof(client->rid) - 1; i++) {
		client->rid[i] =
		    alphabet[arc4random() % (sizeof(alphabet) - 1)];
	}
	client->rid[i] = '\0';
}

int
write_fd(int fd, const char *buf, size_t n)
{
	size_t off;
	ssize_t nw;

	for (off = 0; off < n; off += nw)
		if ((nw = write(fd, buf + off, n - off)) == 0 || nw == -1) {
			syslog(LOG_ERR, "write: %s", strerror(errno));
			break;
		}
	if (off != n)
		return -1;
	return n;
}

const char *
http_status(int code)
{
	static const struct { int code; char *msg; } status[] = {
		{ .code = 200, .msg = "Connection Established" },
		{ .code = 400, .msg = "Bad Request" },
		{ .code = 403, .msg = "Forbidden" },
		{ .code = 500, .msg = "Internal Error" },
		{ .code = 502, .msg = "Proxy Failed Connection" },
		{ .code = 503, .msg = "Service Unavailable" },
	};
	int i;

	for (i = 0; i < sizeof(status) / sizeof(status[0]); i++)
		if (status[i].code == code)
			return status[i].msg;

	assert(0);
	return "Unknown Error";
}

void
write_error(int fd, int code, char *text)
{
	static char buf[4096];
	int n;
	char datebuf[80];
	struct tm *tm;
	time_t t;

	t = time(0);
	tm = gmtime(&t);
	strftime(datebuf, sizeof(datebuf), "%a, %d %b %Y %T %Z", tm);

	n = snprintf(buf, sizeof(buf),
	    "HTTP/1.1 %d %s\r\n"
	    "Server: webgw/1.0\r\n"
	    "Date: %s\r\n"
	    "Content-Type: text/plain;charset=us-ascii\r\n"
	    "Content-Length: %lu\r\n"
	    "Via: 1.1 spirit (webgw/1.0)\r\n"
	    "Connection: close\r\n\r\n"
	    "%s", code, http_status(code), datebuf, strlen(text), text);
	if (write_fd(fd, buf, n) == -1)
		syslog(LOG_ERR, "write_error: %m");
}

void
clientlog(struct client *client, int priority, const char *msg, ...)
{
	va_list ap;
	static char str[1024];

	va_start(ap, msg);
	if (vsnprintf(str, sizeof(str), msg, ap) >= sizeof(str)) {
		str[sizeof(str)-3] = '.';
		str[sizeof(str)-2] = '.';
		str[sizeof(str)-1] = '\0';
	}
	va_end(ap);
	if (priority == LOG_ERR || priority == LOG_CRIT ||
	    priority == LOG_ALERT || priority == LOG_EMERG)
		syslog(priority, "[%s] err: %s", client->rid, str);
	else if (priority == LOG_WARNING)
		syslog(priority, "[%s] warning: %s", client->rid, str);
	else if (priority == LOG_DEBUG)
		syslog(priority, "[%s] debug: %s", client->rid, str);
	else
		syslog(priority, "[%s] %s", client->rid, str);
}

void
removeclient(struct webgw *ctx, struct client *client)
{
	struct kevent changelist;

	if (client->target_host != NULL)
		host_unref(client->target_host);

	EV_SET(&changelist, client->fd,
	    EVFILT_TIMER, EV_DELETE | EV_DISABLE,
	    0, 0, NULL);
	if (kevent(ctx->kq, &changelist, 1, NULL, 0, NULL) == -1 &&
	    errno != ENOENT)
		err(1, "removing idle timer");

	if (client->targetfd != -1) {
		clientlog(client, LOG_INFO, "closing targetfd %d",
		    client->targetfd);
		close(client->targetfd);
	}
	clientlog(client, LOG_INFO, "closing clientfd %d", client->fd);
	close(client->fd);

	if (client->asr_query != NULL) {
		clientlog(client, LOG_WARNING, "aborted asr query to %s",
		    client->parser.host);
		asr_abort(client->asr_query);
		client->asr_query = NULL;
	}

	clock_gettime(CLOCK_MONOTONIC, &client->ts_end);

	if (client->bytes_from_target > 0) {
		clientlog(client, LOG_INFO,
		    "target was: %s:%d "
		    "from_client: %.1f kB "
		    "from_target: %.1f kB",
		    client->parser.host, client->parser.port,
	    	    client->bytes_from_client / 1024.0,
		    client->bytes_from_target / 1024.0);
	}

	if (client->targetconnected == 1) {
		time_t s;
		double ms;
		double comb;

		s = client->ts_end.tv_sec - client->ts_begin.tv_sec;
		ms = (client->ts_end.tv_nsec - client->ts_begin.tv_nsec) /
		    1000.0 / 1000.0;
		comb = s + (ms / 1000.0);
		clientlog(client, LOG_INFO, "lifetime: %.3f s (%s)",
		    comb, client->parser.host);

		s = client->ts_connect.tv_sec - client->ts_begin.tv_sec;
		ms = (client->ts_connect.tv_nsec - client->ts_begin.tv_nsec) /
		    1000.0 / 1000.0;
		comb = (s * 1000.0) + ms;
		clientlog(client, LOG_INFO, "time to connect: %.1f ms (%s)",
		    comb, client->parser.host);

		if (client->bytes_from_target > 0) {
			s = client->ts_firstbyte.tv_sec -
			    client->ts_begin.tv_sec;
			ms = (client->ts_firstbyte.tv_nsec -
			    client->ts_begin.tv_nsec) /
			    1000.0 / 1000.0;
			comb = (s * 1000.0) + ms;
			clientlog(client, LOG_INFO,
			    "time to firstbyte: %.1f ms (%s)",
			    comb, client->parser.host);

		}
	}

	if (client->request_size > 0) {
		ctx->request_size_sum += client->request_size;
		ctx->request_size_samples++;
		if (client->request_size > ctx->request_size_max)
			ctx->request_size_max = client->request_size;
	}

	free(client);
	ctx->nclient--;
	ctx->refill_queue = 1;
	syslog(LOG_INFO, "clients now: %d", ctx->nclient);

	syslog(LOG_INFO,
	    "server [%.2fms max, %.2fms avg] "
	    "client [%.2fms max, %.2fms avg] "
	    "target [%.2fms max, %.2fms avg] "
	    "resolv+connect [%.2fms max, %.2fms avg] "
	    "request_size [%dB max, %dB avg] ",
	    ctx->server_max_usec / 1000.0, ctx->server_samples > 0 ?
	        ctx->server_sum_usec / ctx->server_samples / 1000.0 : 0.0,
	    ctx->client_max_usec / 1000.0, ctx->client_samples > 0 ?
	        ctx->client_sum_usec / ctx->client_samples / 1000.0: 0.0,
	    ctx->target_max_usec / 1000.0, ctx->target_samples > 0 ?
	        ctx->target_sum_usec / ctx->target_samples / 1000.0: 0.0,
	    ctx->resolv_max_usec / 1000.0, ctx->resolv_samples > 0 ?
	        ctx->resolv_sum_usec / ctx->resolv_samples / 1000.0: 0.0,
	    ctx->request_size_max, ctx->request_size_sum > 0 ?
	        ctx->request_size_sum / ctx->request_size_samples: 0);
}
