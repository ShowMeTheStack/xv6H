#ifndef _GETOPT_H
#define _GETOPT_H
#include <sys/cdefs.h>
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *spec);
struct option {
  const char *name;
  int has_arg;
  int *flag;
  int val;
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2
int getopt_long(int argc, char *const argv[], const char *spec, const struct option *opts, int *index);
int getopt_long_only(int argc, char *const argv[], const char *spec, const struct option *opts, int *index);
#endif
