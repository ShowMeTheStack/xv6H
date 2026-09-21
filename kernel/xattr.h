#ifndef XV6_XATTR_H
#define XV6_XATTR_H
//
// extended attributes: the flags setxattr takes, shared with user
// space through <sys/xattr.h>.
//
#define XATTR_CREATE  1  // the name must not be there already
#define XATTR_REPLACE 2  // the name must be there already

#define XATTR_NAME_MAX  255   // the longest name
#define XATTR_SIZE_MAX  65536 // the largest value
#define XATTR_LIST_MAX  65536 // the longest list of names

// the first argument of the four system calls says what the second one
// is: a descriptor, or a path that does or does not follow a symbolic
// link at its end. the twelve POSIX names are these four calls with
// this argument set three ways.
#define XATTR_AT_PATH  (-1)
#define XATTR_AT_LPATH (-2)

#endif
