// the environment: exec leaves it above the stack and start() records
// it here; setenv() moves the array to the heap the first time it grows.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libc.h"

static char **env_heap; // the copy we own, or 0
static int env_cap;

static int
name_len(const char *s)
{
  int i = 0;
  while (s[i] && s[i] != '=')
    i++;
  return i;
}

char *
getenv(const char *name)
{
  int n = strlen(name);
  for (char **e = environ; e && *e; e++) {
    if (name_len(*e) == n && memcmp(*e, name, n) == 0 && (*e)[n] == '=')
      return *e + n + 1;
  }
  return 0;
}

// make the array one we may modify, with room for one more entry
static int
env_own(int want)
{
  int n = 0;
  while (environ && environ[n])
    n++;
  if (env_heap && want <= env_cap)
    return n;
  int cap = want < 16 ? 16 : want * 2;
  char **t = malloc((cap + 1) * sizeof(char *));
  if (t == 0)
    return -1;
  for (int i = 0; i < n; i++)
    t[i] = environ[i];
  t[n] = 0;
  if (env_heap)
    free(env_heap);
  env_heap = t;
  env_cap = cap;
  environ = t;
  return n;
}

int
setenv(const char *name, const char *value, int overwrite)
{
  int nl = strlen(name), vl = strlen(value);
  if (nl == 0 || strchr(name, '='))
    return -1;
  char *entry = malloc(nl + vl + 2);
  if (entry == 0)
    return -1;
  memmove(entry, name, nl);
  entry[nl] = '=';
  memmove(entry + nl + 1, value, vl + 1);

  for (char **e = environ; e && *e; e++) {
    if (name_len(*e) == nl && memcmp(*e, name, nl) == 0) {
      if (!overwrite) {
        free(entry);
        return 0;
      }
      if (env_own(0) < 0) {
        free(entry);
        return -1;
      }
      for (char **f = environ; *f; f++) {
        if (name_len(*f) == nl && memcmp(*f, name, nl) == 0) {
          *f = entry;
          return 0;
        }
      }
    }
  }
  int n = env_own(0);
  if (n < 0 || n + 1 > env_cap) {
    if (env_own(n + 1) < 0) {
      free(entry);
      return -1;
    }
    n = 0;
    while (environ[n])
      n++;
  }
  environ[n] = entry;
  environ[n + 1] = 0;
  return 0;
}

int
unsetenv(const char *name)
{
  int nl = strlen(name);
  if (env_own(0) < 0)
    return -1;
  for (char **e = environ; *e; e++) {
    if (name_len(*e) == nl && memcmp(*e, name, nl) == 0) {
      for (char **f = e; *f; f++)
        *f = *(f + 1);
      return 0;
    }
  }
  return 0;
}

int
putenv(char *s)
{
  char *eq = strchr(s, '=');
  if (eq == 0)
    return unsetenv(s);
  int n = env_own(0);
  if (n < 0)
    return -1;
  int nl = eq - s;
  for (char **e = environ; *e; e++) {
    if (name_len(*e) == nl && memcmp(*e, s, nl) == 0) {
      *e = s;
      return 0;
    }
  }
  if (n + 1 > env_cap && env_own(n + 1) < 0)
    return -1;
  n = 0;
  while (environ[n])
    n++;
  environ[n] = s;
  environ[n + 1] = 0;
  return 0;
}

int
clearenv(void)
{
  if (env_own(0) < 0)
    return -1;
  environ[0] = 0;
  return 0;
}
