#ifndef XV6_FILE_H
#define XV6_FILE_H
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_SOCK } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint64 off;        // FD_INODE
  int flags;         // the open() flags this description was made with
  short major;       // FD_DEVICE
  struct sock *sock; // FD_SOCK
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define mkdev(m, n) ((uint)((m) << 16 | (n)))

// in-memory copy of an inode
struct inode {
  uint dev;              // Device number
  uint inum;             // Inode number
  int ref;               // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;             // inode has been read from disk?

  // which file system this inode belongs to, and how to work on it.
  // the operations dispatch through here, so a path may cross from one
  // file system into another without the caller knowing.
  struct super *sb;
  const struct inode_ops *ops;
  void *fsdata;          // the file system's own per-inode state

  short type; // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint64 size;
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ready)(int write); // bytes readable, or 1 if writable
  int (*ioctl)(uint64 req, uint64 arg);
};

extern struct devsw devsw[];

// the major numbers of the character devices
#define CONSOLE   1
#define DEV_NULL   2
#define DEV_ZERO   3
#define DEV_FULL   4
#define DEV_RANDOM 5
#endif
