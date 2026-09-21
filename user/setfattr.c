// setfattr: hang a name and a value on a file.
//
//   setfattr -n NAME [-v VALUE] FILE...   set it
//   setfattr -n NAME -f VALUEFILE FILE... set it to a file's contents
//   setfattr -x NAME FILE...              remove it
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/xattr.h>

int
main(int argc, char *argv[])
{
  const char *name = 0, *value = "";
  static char buf[XATTR_SIZE_MAX];
  int remove = 0, i = 1, rc = 0;
  long vlen = -1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
      name = argv[++i];
      remove = 1;
    } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
      value = argv[++i];
    else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      // the value is what a file holds, which is how a value too long
      // to type on one line gets set
      FILE *f = fopen(argv[++i], "r");
      if (f == 0) {
        fprintf(stderr, "setfattr: %s: %s\n", argv[i], strerror(errno));
        return 1;
      }
      vlen = (long)fread(buf, 1, sizeof(buf), f);
      fclose(f);
      value = buf;
    } else {
      fprintf(stderr, "usage: setfattr -n name [-v value | -f file] file...\n");
      return 1;
    }
  }
  if (name == 0 || i >= argc) {
    fprintf(stderr, "usage: setfattr -n name [-v value | -f file] file...\n");
    return 1;
  }
  for (; i < argc; i++) {
    int r = remove ? removexattr(argv[i], name)
                   : setxattr(argv[i], name, value, vlen >= 0 ? (size_t)vlen : strlen(value), 0);
    if (r < 0) {
      fprintf(stderr, "setfattr: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}
