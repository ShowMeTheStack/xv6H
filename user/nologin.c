// nologin: the shell of an account that may not log in.
#include "user/user.h"

int
main(void)
{
  fprintf(stderr, "This account is not available.\n");
  exit(1);
}
