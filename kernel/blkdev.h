#ifndef XV6_BLKDEV_H
#define XV6_BLKDEV_H
//
// a block device opened as a file, and the ioctls it answers. the
// numbers are Linux's, because every tool that asks a device how big
// it is asks the same way.
//
struct gendisk;

#define BLKROSET    0x125d
#define BLKROGET    0x125e
#define BLKRRPART   0x125f
#define BLKGETSIZE  0x1260
#define BLKFLSBUF   0x1261
#define BLKSSZGET   0x1268
#define BLKBSZGET   0x80081270
#define BLKGETSIZE64 0x80081272

uint64 blkdev_size(struct gendisk *d);
int blkdev_read(struct gendisk *d, int user, uint64 addr, uint64 off, uint64 n);
int blkdev_write(struct gendisk *d, int user, uint64 addr, uint64 off, uint64 n);
int blkdev_ioctl(struct gendisk *d, uint64 req, uint64 arg);

#endif
