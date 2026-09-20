//
// <nl_types.h>: message catalogues, of which this system has none.
//
// a catalogue is a compiled file of translated strings that catopen
// finds through NLSPATH and the locale. there is no NLSPATH here, no
// gencat to build one with and only the C locale to translate into, so
// the honest implementation is the one below: catopen always fails,
// which POSIX allows, and catgets then returns the default string its
// caller compiled in. a program written against this interface keeps
// working unchanged if catalogues ever arrive.
//
#include <nl_types.h>
#include <errno.h>

nl_catd
catopen(const char *name, int flag)
{
  (void)name;
  (void)flag;
  errno = ENOENT;
  return (nl_catd)-1;
}

char *
catgets(nl_catd catd, int set, int msg, const char *def)
{
  (void)catd;
  (void)set;
  (void)msg;
  // POSIX has catgets return the default string on failure, and the
  // string is the caller's own, so handing it straight back is right
  // even though the const is dropped to match the prototype.
  return (char *)def;
}

int
catclose(nl_catd catd)
{
  if (catd == (nl_catd)-1) {
    errno = EBADF;
    return -1;
  }
  return 0;
}
