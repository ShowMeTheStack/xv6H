#ifndef XV6_VFS_H
#define XV6_VFS_H
#include "types.h"
//
// the virtual file system.
//
// an inode belongs to a mounted file system and carries a pointer to
// the operations of its type, so that a path may cross from one file
// system into another without anything above this layer knowing which
// is which. the old on-disk format, ext4, and the file systems that
// have no disk at all (/proc, /sys, /dev) are all the same shape from
// here.
//
// the inode structure itself stays where it was, in file.h, because
// every file in the kernel already includes that; what is here is the
// dispatch around it.
//

struct inode;
struct stat;
struct super;
struct gendisk;

// what a file system can do to an inode
struct inode_ops {
  // read the inode's fields from the disk into the in-memory copy
  void (*load)(struct inode *ip);
  // write them back
  void (*update)(struct inode *ip);
  // the inode is being dropped, and had no links left
  void (*release)(struct inode *ip);

  int (*read)(struct inode *ip, int user_dst, uint64 dst, uint off, uint n);
  int (*write)(struct inode *ip, int user_src, uint64 src, uint off, uint n);
  void (*trunc)(struct inode *ip);
  // everything this file holds is on the disk when this returns: its
  // data first, then the metadata that points at it.
  int (*fsync)(struct inode *ip, int datasync);

  // a name in a directory. returns the inode, unlocked, with a
  // reference, or zero. *poff is the offset of the entry, when asked.
  struct inode *(*lookup)(struct inode *dir, char *name, uint *poff);
  // walk a directory. *pos is carried between calls; returns 1 for an
  // entry, 0 at the end, negative for an error.
  int (*readdir)(struct inode *dir, uint64 *pos, char *name, int namemax, uint *ino, uchar *type);
  // make a new inode of `type` in `dir` under `name`, locked
  struct inode *(*create)(struct inode *dir, char *name, short type, short major, short minor);
  int (*link)(struct inode *dir, char *name, struct inode *ip);
  int (*unlink)(struct inode *dir, char *name);
  // where a symbolic link points
  int (*readlink)(struct inode *ip, char *buf, int max);
};

// what a mounted file system can do
struct super_ops {
  struct inode *(*iget)(struct super *sb, uint inum);
  void (*statfs)(struct super *sb, uint64 *blocks, uint64 *bfree, uint64 *files, uint64 *ffree,
                 uint64 *bsize, uint64 *namelen);
  int (*sync)(struct super *sb);
  void (*unmount)(struct super *sb);
  // an inode that has lost its last link and its last reference. the
  // inode itself is gone by now, so this is given only what it needs.
  void (*ifree)(struct super *sb, uint inum, int isdir);
};

#define MAXMOUNT 16
#define MOUNTPATH 64

struct super {
  const struct super_ops *ops;
  const struct inode_ops *iops;   // the default for this file system's inodes
  struct gendisk *disk;           // zero for a file system with no disk
  void *priv;                     // the file system's own state
  char type[16];                  // ext4, xv6, devfs, procfs
  char source[MOUNTPATH];         // the device or the name it was mounted from
  char path[MOUNTPATH];           // where it is mounted
  struct inode *root;             // its root inode
  struct inode *covered;          // the inode it is mounted over, or zero for /
  int readonly;
  int used;
};

// a file system type the kernel knows how to mount
struct fs_type {
  char name[16];
  // mount the device (which may be zero) and give back the super
  struct super *(*mount)(struct gendisk *disk, const char *source, int readonly, const char *opts);
  struct fs_type *next;
};

void vfs_init(void);
void vfs_register(struct fs_type *type);
struct fs_type *vfs_find_type(const char *name);
struct super *vfs_mount(const char *source, const char *target, const char *type, int readonly,
                        const char *opts);
int vfs_umount(const char *target);
struct super *vfs_super_alloc(void);
void vfs_super_free(struct super *sb);
// the file system mounted on this inode, if any
struct super *vfs_mounted_on(struct inode *ip);
int vfs_sync_all(void);
void vfs_list(void (*emit)(const char *source, const char *path, const char *type, int readonly,
                           void *arg),
              void *arg);
struct super *vfs_root_super(void);
void vfs_set_root_super(struct super *sb);

#endif
