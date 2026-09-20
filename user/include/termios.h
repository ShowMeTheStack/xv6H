#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <sys/cdefs.h>
#include <sys/types.h>
#include "kernel/termios.h" // struct termios and every flag
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
int tcflush(int fd, int q);
int tcdrain(int fd);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int dur);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgid);
pid_t tcgetsid(int fd);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t s);
int cfsetospeed(struct termios *t, speed_t s);
int cfsetspeed(struct termios *t, speed_t s);
void cfmakeraw(struct termios *t);
#endif
