// true and false: the two programs that do nothing, differently.
//
// the shell has both as builtins, so these exist for the times a
// program is what is wanted -- find -exec, xargs, a cron line, the
// shell of an account that must not have one.
#include "user/user.h"

int
main(int argc, char *argv[])
{
  const char *me = argv[0];

  for (const char *p = argv[0]; *p; p++)
    if (*p == '/')
      me = p + 1;
  exit(strcmp(me, "false") == 0 ? 1 : 0);
}
