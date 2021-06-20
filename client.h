#ifndef CLIENT_H
#define CLIENT_H

enum client_err {
	CLIENT_ERR_TRUNCATED_STARTLINE	= 0,
};

enum http_status_code
{
	HTTP_STATUS_SUCCESS		= 200,
	HTTP_STATUS_BAD_REQUEST		= 400,
	HTTP_STATUS_FORBIDDEN		= 403,
	HTTP_STATUS_INTERNAL_ERROR	= 500,
	HTTP_STATUS_FAILED_CONNECTION	= 502,
	HTTP_STATUS_SERVICE_UNAVAILABLE	= 503
};

void clientlog(struct client *, int, const char *, ...);
void removeclient(struct webgw *, struct client *);
int write_fd(int, const char *, size_t);
void write_error(int, int, char *);
void mkrid(struct client *);
const char *http_status(int);

#endif
