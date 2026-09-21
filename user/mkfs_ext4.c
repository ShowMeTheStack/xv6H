// mkfs.ext4: the same program that writes the images this system boots
// from, built for here as well as for the build host.
//
// mkfs/mkext4.c is written against nothing but standard C and the
// POSIX file calls, so it compiles for this system very nearly
// unchanged. keeping one copy of it means an image made on the
// machine and one made beside it are the same shape, down to the
// checksums -- and that the code tools/ext4check.py hands to e2fsck
// and to a real Linux mount is this code.
//
// the one difference is the name of two fields. this system's struct
// stat calls them `size' and `mode' rather than `st_size' and
// `st_mode', for the reason sys/stat.h gives: a macro the other way
// round would rewrite every identifier in every file that called a
// variable `size'. the two names are bridged here, where the bridge
// is visible, rather than in the header or by editing the source that
// the build host also compiles.
#define st_size size
#define st_mode mode

#include "mkfs/mkext4.c"
