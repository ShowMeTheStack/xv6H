//
// password hashing: pbkdf2 over hmac-sha-256.
//
// a stored password is the string
//
//   $pbkdf2-sha256$ROUNDS$SALT$HASH
//
// with the salt and the hash in base64. crypt() takes a setting of
// that shape -- which the whole stored string is -- and returns the
// whole string again, so checking a password is a string comparison
// against what is stored, and setting one is crypt() with a fresh
// salt. the rounds are in the string, so the cost can be raised later
// without invalidating what is already there.
//
// everything but the salt is in pbkdf2.h, which the tool that writes
// /etc/shadow at build time includes as well: the two must agree, and
// the way to be sure of that is one copy.
//
#include <shadow.h>
#include <sys/random.h>
#include "pbkdf2.h"

static char salt_result[64];

char *
crypt_gensalt(int rounds)
{
  unsigned char salt[12];
  char saltb64[32];

  if (rounds <= 0)
    rounds = 100000;
  if (getrandom(salt, sizeof(salt), 0) != (ssize_t)sizeof(salt)) {
    // without the kernel's generator there is nothing safe to fall
    // back to, so say so rather than make up a salt
    return 0;
  }
  b64_encode(salt, sizeof(salt), saltb64);
  snprintf(salt_result, sizeof(salt_result), PBKDF2_PREFIX "%d$%s$", rounds, saltb64);
  return salt_result;
}
