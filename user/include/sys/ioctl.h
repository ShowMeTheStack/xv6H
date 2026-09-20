#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#include <sys/cdefs.h>
#include "kernel/posix.h"   // FIONREAD, TIOCGWINSZ, struct winsize
#include "kernel/termios.h" // the terminal requests
int ioctl(int fd, unsigned long req, ...);
#endif
