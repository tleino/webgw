#ifndef HOST_H
#define HOST_H

#include <stddef.h>

struct host;

struct host *host_create(const char *, int, int);
struct host *host_create_from_data(char *);

void         host_free(struct host *);

const char  *host_name(struct host *);
int          host_port(struct host *);
int          host_visits(struct host *);
int          host_rx_bytes(struct host *);
int          host_tx_bytes(struct host *);

void         host_authorize(struct host *, const char *);
void         host_unauthorize(struct host *);
int          host_is_authorized(struct host *);
int          host_is_held(struct host *);

const char  *host_pattern(struct host *);

void         host_incr_visits(struct host *);
void         host_add_rx_bytes(struct host *, int bytes);
void         host_add_tx_bytes(struct host *, int bytes);

void         host_ref(struct host *);
void         host_unref(struct host *);
int          host_ref_count(struct host *);

const char  *host_serialize(struct host *, char *, size_t);

#endif
