#ifndef XV6_FS_H
#define XV6_FS_H
//
// what the kernel's file system layer agrees on, whatever format is
// underneath. the formats themselves are in their own headers:
// kernel/ext4_ondisk.h for ext4, kernel/jbd2.h for its journal.
//

// the longest name a single path element may have: what ext4 allows,
// and what <limits.h> gives a program as NAME_MAX. DIRSIZ is the buffer
// that holds one -- the name and the byte that ends it -- and the
// kernel keeps one on the stack while it walks a path, so it is a
// number with a cost and not only a limit.
#define NAME_MAX 255
#define DIRSIZ   (NAME_MAX + 1)

// nlink is a short, so a file cannot have more links than one holds
#define NLINK_MAX 32767

#endif
