// getfattr: the names hung on a file, and with -d their values too.
//
//   getfattr [-d] [-n NAME] FILE...
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/xattr.h>

static int dump;

static void
show(const char *file, const char *name)
{
  char value[4096];

  if (!dump) {
    printf("%s\n", name);
    return;
  }
  int n = getxattr(file, name, value, sizeof(value));
  if (n < 0) {
    fprintf(stderr, "getfattr: %s: %s: %s\n", file, name, strerror(errno));
    return;
  }
  printf("%s=\"", name);
  for (int i = 0; i < n; i++) {
    if (value[i] >= 0x20 && value[i] < 0x7f && value[i] != '"' && value[i] != '\\')
      putchar(value[i]);
    else
      printf("\\%03o", (unsigned char)value[i]);
  }
  printf("\"\n");
}

int
main(int argc, char *argv[])
{
  const char *one = 0;
  int i = 1, rc = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-d") == 0)
      dump = 1;
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      one = argv[++i];
    else {
      fprintf(stderr, "usage: getfattr [-d] [-n name] file...\n");
      return 1;
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: getfattr [-d] [-n name] file...\n");
    return 1;
  }
  for (; i < argc; i++) {
    printf("# file: %s\n", argv[i]);
    if (one) {
      show(argv[i], one);
      continue;
    }
    char list[4096];
    int n = listxattr(argv[i], list, sizeof(list));
    if (n < 0) {
      fprintf(stderr, "getfattr: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    for (int k = 0; k < n; k += strlen(list + k) + 1)
      show(argv[i], list + k);
  }
  return rc;
}
