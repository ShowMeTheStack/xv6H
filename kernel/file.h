#ifndef XV6_FILE_H
#define XV6_FILE_H
struct gendisk;

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_BLOCK, FD_SOCK } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint64 off;        // FD_INODE
  int flags;         // the open() flags this description was made with
  short major;       // FD_DEVICE
  struct gendisk *disk; // FD_BLOCK: the disk this file is a view of
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

  // who it belongs to and what may be done to it. the file system
  // fills these in load() and writes them back in update(); a file
  // system with no such thing of its own gives the values it wants
  // every one of its files to have.
  uint mode;  // the permission bits, without the format
  uint uid, gid;
  uint64 atime, mtime, ctime; // seconds
  // the flags chattr(1) sets, in ext4's own numbering (EXT4_*_FL).
  // two of them are acted on here -- immutable and append-only -- and
  // the rest are carried faithfully so that a file system written
  // here and one written by Linux say the same thing about a file.
  uint attr;
};

// the two this kernel enforces, from mkfs/ext4_ondisk.h
#define ATTR_IMMUTABLE 0x00000010
#define ATTR_APPEND    0x00000020

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ready)(int write); // bytes readable, or 1 if writable
  int (*ioctl)(uint64 req, uint64 arg);
  // a device that costs something while it is open says so here: open
  // is called when a file is made for it and close when the last one
  // goes, so nothing is spent on a device nobody is using.
  int (*open)(void);
  void (*close)(void);
};

extern struct devsw devsw[];

// the major numbers of the character devices
#define CONSOLE   1
#define DEV_NULL   2
#define DEV_ZERO   3
#define DEV_FULL   4
#define DEV_RANDOM 5

// the block devices all share one major; the minor is the disk's index
// in the block layer's table, so /dev/vda and /dev/vda1 differ only in
// which gendisk they name.
#define DEV_BLOCK  STAT_DEV_BLOCK  // stat.h, so a program can tell one from a character device

// /dev/loop-control: the device that makes and unmakes loop devices
#define DEV_LOOPCTL 7
#define DEV_LOG     8   // /dev/log: what syslog(3) writes and syslogd reads
#define DEV_KMSG    9   // /dev/kmsg: the kernel's own messages
#define DEV_PKTCAP 10   // /dev/pktcap: every frame, as pcap records
#endif
