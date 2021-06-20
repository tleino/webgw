#ifndef WEBCLIENT_H
#define WEBCLIENT_H

struct webgw;
struct client;

void webclient_init(struct webgw *, struct client *, int);

#endif
