// whoami: the name of the effective user.
#include "user/user.h"
#include <pwd.h>

int
main(void)
{
  struct passwd *pw = getpwuid(geteuid());

  if (pw)
    printf("%s\n", pw->pw_name);
  else
    printf("%d\n", (int)geteuid());
  exit(0);
}
