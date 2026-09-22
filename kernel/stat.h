#ifndef XV6_STAT_H
#define XV6_STAT_H
#include "types.h"
#define T_DIR    1 // Directory
#define T_FILE   2 // File
#define T_DEVICE 3 // Device
#define T_SYMLINK 4 // Symbolic link: its contents are the path it names
#define T_SOCK    5 // a socket bound to this name (AF_UNIX)

// the major number a block device has, so that a program looking at a
// stat can tell one from a character device without asking the kernel
#define STAT_DEV_BLOCK 6

struct stat {
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short type;  // Type of file
  short nlink; // Number of links to file
  uint64 size; // Size of file in bytes
  uint mode;   // permission bits (the format is in `type`)
  uint uid, gid;
  uint64 atime, mtime, ctime; // seconds
  uint64 blocks;              // 512-byte blocks the file occupies
  int rdev;                   // a device file: major and minor packed
};
#endif
