#ifndef XV6_TERMIOS_H
#define XV6_TERMIOS_H
#include "types.h"
//
// terminal settings, as POSIX defines them (the subset a line
// discipline over a serial console needs).
//
#define NCCS 20

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

// c_cc indices
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VWERASE  14
#define VREPRINT 12
#define VLNEXT   15

// c_iflag
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000
#define IUTF8   0x4000

// c_oflag
#define OPOST   0x0001
#define OLCUC   0x0002
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020
#define OFILL   0x0040
#define OFDEL   0x0080

// c_cflag
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CRTSCTS 0x80000000
#define CREAD   0x0080
#define CLOCAL  0x0800

// c_lflag
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define ECHOCTL 0x0200
#define ECHOPRT 0x0400
#define ECHOKE  0x0800
#define FLUSHO  0x1000
#define PENDIN  0x4000
#define IEXTEN  0x8000
#define XCASE   0x0004

// tcflush queues and tcflow actions
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

// the baud rates. like Linux, c_cflag holds a small code rather than
// the rate itself, and c_ispeed/c_ospeed hold the rate in bits per
// second; a console ignores both.
#define B0       0x0000
#define B50      0x0001
#define B75      0x0002
#define B110     0x0003
#define B134     0x0004
#define B150     0x0005
#define B200     0x0006
#define B300     0x0007
#define B600     0x0008
#define B1200    0x0009
#define B1800    0x000a
#define B2400    0x000b
#define B4800    0x000c
#define B9600    0x000d
#define B19200   0x000e
#define B38400   0x000f
#define B57600   0x1001
#define B115200  0x1002
#define B230400  0x1003
#define B460800  0x1004
#define B921600  0x1006
#define CBAUD    0x100f

// tcsetattr actions (ioctl requests carry them)
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

// the ioctl requests
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCFLSH     0x540B
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCNOTTY  0x5422
#define TCXONC     0x540A
// TIOCGWINSZ and TIOCSWINSZ are in posix.h

#endif
