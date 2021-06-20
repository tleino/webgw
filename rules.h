#ifndef RULES_H
#define RULES_H

#include <stddef.h>

const char *rules_match          (const char *, int);
void        rules_load           (void);
void        rules_load_from_data (char *);
char*       rules_to_data        (void);

#endif
