#ifndef _NL_TYPES_H
#define _NL_TYPES_H
#include <sys/cdefs.h>

#define NL_SETD 1
#define NL_CAT_LOCALE 1

typedef int nl_item;
typedef void *nl_catd;

nl_catd catopen(const char *name, int flag);
char *catgets(nl_catd catd, int set, int msg, const char *def);
int catclose(nl_catd catd);
#endif
