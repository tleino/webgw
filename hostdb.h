#ifndef HOSTDB_H
#define HOSTDB_H

struct hostnode;
struct hostdb;
struct host;

struct hostdb *hostdb_create  (void);
void           hostdb_free    (struct hostdb *);

struct host   *hostdb_find    (struct hostdb *, const char *, int);

struct host   *hostdb_iterate (struct hostdb *, struct hostnode **);

#endif
