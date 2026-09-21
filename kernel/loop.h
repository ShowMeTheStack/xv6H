#ifndef XV6_LOOP_H
#define XV6_LOOP_H
//
// loop devices: a file made to look like a disk. the ioctl numbers and
// the structure are Linux's, so losetup(8) written against Linux finds
// what it expects.
//
struct gendisk;
struct inode;

#define LOOP_SET_FD        0x4C00
#define LOOP_CLR_FD        0x4C01
#define LOOP_SET_STATUS64  0x4C04
#define LOOP_GET_STATUS64  0x4C05
#define LOOP_SET_CAPACITY  0x4C07

// a copy-on-write view of a base image: reads that have not been
// written come from the base, writes go to the delta, and the base is
// never changed -- so many devices can share one image and each keeps
// its own changes. not Linux's; Linux does this with device-mapper.
#define LOOP_SET_COW       0x4C10

struct loop_cow {
  int base_fd;   // the image every device shares, read-only
  int delta_fd;  // this device's own changes, made if the file is empty
};

#define LOOP_CTL_ADD       0x4C80
#define LOOP_CTL_REMOVE    0x4C81
#define LOOP_CTL_GET_FREE  0x4C82

#define LO_FLAGS_READ_ONLY 1
#define LO_FLAGS_AUTOCLEAR 4
#define LO_FLAGS_PARTSCAN  8

#define LO_NAME_SIZE 64

struct loop_info64 {
  uint64 lo_device;
  uint64 lo_inode;
  uint64 lo_rdevice;
  uint64 lo_offset;
  uint64 lo_sizelimit;
  uint32 lo_number;
  uint32 lo_encrypt_type;
  uint32 lo_encrypt_key_size;
  uint32 lo_flags;
  uchar  lo_file_name[LO_NAME_SIZE];
  uchar  lo_crypt_name[LO_NAME_SIZE];
  uchar  lo_encrypt_key[32];
  uint64 lo_init[2];
};

void loopinit(void);
int loop_add(int index);
int loop_remove(int index);
int loop_get_free(void);
int loop_set_fd(int index, struct inode *ip, uint64 offset, uint64 sizelimit, int readonly);
int loop_clr_fd(int index);
int loop_status(int index, struct loop_info64 *info);
int loop_set_status(int index, const struct loop_info64 *info);
int loop_set_name(int index, const char *name);
int loop_set_cow(int index, struct inode *base, struct inode *delta);
int loop_index_of(struct gendisk *d);

#endif
