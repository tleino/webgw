#ifndef SERVER_H
#define SERVER_H

struct hostport;

void			 server_unauthorize(struct webgw *, const char *, int);
void			 server_authorize(struct webgw *, const char *, int);
int			 server_hold(struct webgw *, const char *, int);
int			 server_on_hold(struct webgw *, const char *, int);
int			 server_has_authorized(struct webgw *,
			    const char *, int);
struct hostport		*server_iterate_unauthorized(struct webgw *,
			    struct hostport **, const char **, int *, int *);

#endif
