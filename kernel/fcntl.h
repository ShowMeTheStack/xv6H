#ifndef XV6_FCNTL_H
#define XV6_FCNTL_H
// open() flags: the low two bits are the access mode, as POSIX wants.
#define O_RDONLY    0x000
#define O_WRONLY    0x001
#define O_RDWR      0x002
#define O_ACCMODE   0x003
#define O_CREATE    0x200
#define O_CREAT     0x200
#define O_TRUNC     0x400
#define O_APPEND    0x800
#define O_NONBLOCK  0x1000
#define O_NDELAY    0x1000
#define O_CLOEXEC   0x2000
#define O_DIRECTORY 0x4000
#define O_EXCL      0x8000
#define O_NOFOLLOW  0x10000
#define O_SYNC      0x20000
#define O_PATH      0x40000
#endif
