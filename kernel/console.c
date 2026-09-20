//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "errno.h"
#include "signal.h"

#include "termios.h"
#include "posix.h"

#define BACKSPACE 0x100       // erase the last output character
#define C(x)      ((x) - '@') // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printk and to echo input
// characters.
//
void
consputc(int c)
{
  if (c == BACKSPACE) {
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b');
    uartputc_sync(' ');
    uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;

  // input
#define INPUT_BUF_SIZE 512
  char buf[INPUT_BUF_SIZE];
  uint r; // Read index
  uint w; // Write index
  uint e; // Edit index
  int raw; // the host console is relaying a guest's console

  // terminal settings and the session that owns the terminal
  struct termios tio;
  struct winsize ws;
  int pgrp;  // the foreground process group
  int sid;   // the session that has this terminal, 0 if none
} cons;

// the default settings: canonical input, echo, signals, no output
// translation (the serial line wants a bare newline).
static void
tty_defaults(struct termios *t)
{
  memset(t, 0, sizeof(*t));
  t->c_iflag = ICRNL;
  t->c_oflag = 0;
  t->c_cflag = CS8 | CREAD | CLOCAL | B38400;
  t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN | ECHOCTL;
  t->c_cc[VINTR] = C('C');
  t->c_cc[VQUIT] = C('\\');
  t->c_cc[VERASE] = '\x7f';
  t->c_cc[VKILL] = C('U');
  t->c_cc[VEOF] = C('D');
  t->c_cc[VSUSP] = C('Z');
  t->c_cc[VSTART] = C('Q');
  t->c_cc[VSTOP] = C('S');
  t->c_cc[VWERASE] = C('W');
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
  t->c_ispeed = t->c_ospeed = B38400;
}

// switch the console between cooked (line-edited, echoed) and raw mode.
void
consoleraw(int on)
{
  acquire(&cons.lock);
  cons.raw = on;
  if (on) {
    // discard any partially edited line.
    cons.w = cons.e;
    wakeup(&cons.r);
  }
  release(&cons.lock);
}

// a whole kernel message, in one burst.
void
consputs(const char *s, int n)
{
  uartputs_sync(s, n);
}

// the terminal ioctls: settings, window size, foreground group and the
// controlling terminal.
int
consoleioctl(uint64 req, uint64 arg)
{
  struct proc *p = myproc();
  struct termios t;
  struct winsize ws;
  int v;

  switch (req) {
  case TCGETS:
    acquire(&cons.lock);
    t = cons.tio;
    release(&cons.lock);
    return either_copyout(1, arg, &t, sizeof(t)) < 0 ? -EFAULT : 0;
  case TCSETS:
  case TCSETSW:
  case TCSETSF:
    if (either_copyin(&t, 1, arg, sizeof(t)) < 0)
      return -EFAULT;
    acquire(&cons.lock);
    cons.tio = t;
    if (req == TCSETSF)
      cons.r = cons.w = cons.e = 0; // flush what was typed
    release(&cons.lock);
    return 0;
  case TIOCGWINSZ:
    acquire(&cons.lock);
    ws = cons.ws;
    release(&cons.lock);
    return either_copyout(1, arg, &ws, sizeof(ws)) < 0 ? -EFAULT : 0;
  case TIOCSWINSZ:
    if (either_copyin(&ws, 1, arg, sizeof(ws)) < 0)
      return -EFAULT;
    acquire(&cons.lock);
    cons.ws = ws;
    int pgrp = cons.pgrp;
    release(&cons.lock);
    if (pgrp)
      signal_kill(-pgrp, SIGWINCH, 0);
    return 0;
  case TIOCGPGRP:
    acquire(&cons.lock);
    v = cons.pgrp;
    release(&cons.lock);
    return either_copyout(1, arg, &v, sizeof(v)) < 0 ? -EFAULT : 0;
  case TIOCSPGRP:
    if (either_copyin(&v, 1, arg, sizeof(v)) < 0)
      return -EFAULT;
    if (v <= 0)
      return -EINVAL;
    acquire(&cons.lock);
    if (cons.sid && p->sid != cons.sid) {
      release(&cons.lock);
      return -EPERM;
    }
    cons.pgrp = v;
    release(&cons.lock);
    return 0;
  case TIOCSCTTY:
    // a session leader claims the terminal
    if (p->sid != p->pid)
      return -EPERM;
    acquire(&cons.lock);
    if (cons.sid && cons.sid != p->sid) {
      release(&cons.lock);
      return -EPERM;
    }
    cons.sid = p->sid;
    cons.pgrp = p->pgid;
    release(&cons.lock);
    p->tty = CONSOLE;
    return 0;
  case TIOCNOTTY:
    acquire(&cons.lock);
    if (cons.sid == p->sid) {
      cons.sid = 0;
      cons.pgrp = 0;
    }
    release(&cons.lock);
    p->tty = 0;
    return 0;
  case TCFLSH:
    acquire(&cons.lock);
    cons.r = cons.w = cons.e = 0;
    release(&cons.lock);
    return 0;
  case TCXONC:
    return 0;
  }
  return -ENOTTY;
}

// how many characters are waiting (a whole line is needed for a read
// to return, but poll reports the bytes that are there).
int
consoleready(int write)
{
  if (write)
    return 1;
  acquire(&cons.lock);
  int n = cons.w - cons.r;
  release(&cons.lock);
  return n;
}

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;
  struct proc *p = myproc();

  // a background write is stopped only when the terminal says so
  acquire(&cons.lock);
  int onlcr = !cons.raw && (cons.tio.c_oflag & OPOST) && (cons.tio.c_oflag & ONLCR);
  int tostop = !cons.raw && (cons.tio.c_lflag & TOSTOP) && cons.pgrp && p && p->tty && p->pgid != cons.pgrp;
  release(&cons.lock);
  if (tostop) {
    signal_kill(-p->pgid, SIGTTOU, 0);
    return -EINTR;
  }

  while (i < n) {
    int nn = sizeof(buf);
    if (nn > n - i)
      nn = n - i;
    if (either_copyin(buf, user_src, src + i, nn) == -1)
      break;
    if (onlcr) {
      for (int j = 0; j < nn; j++) {
        if (buf[j] == '\n')
          uartwrite("\r", 1);
        uartwrite(&buf[j], 1);
      }
    } else {
      uartwrite(buf, nn);
    }
    i += nn;
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dst indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char line[INPUT_BUF_SIZE];
  int k;
  struct proc *p = myproc();

  // a background process reading from its controlling terminal is
  // stopped, as POSIX says (unless it cannot take the signal).
  acquire(&cons.lock);
  if (!cons.raw && p && p->tty && cons.pgrp && p->pgid != cons.pgrp) {
    release(&cons.lock);
    signal_kill(-p->pgid, SIGTTIN, 0);
    return -EINTR;
  }
  target = n;
  while (n > 0) {
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while (cons.r == cons.w) {
      if (killed(myproc()) || signal_pending(myproc())) {
        release(&cons.lock);
        return target > n ? (int)(target - n) : -EINTR;
      }
      sleep_prepare(&cons.r);
      release(&cons.lock);
      sleep_intr();
      acquire(&cons.lock);
    }

    // take a batch of characters under the lock, up to n, stopping at
    // an end-of-file or a newline; copy them out with the lock released
    // (bringing a user page in may sleep).
    k = 0;
    int eof = 0, nl = 0;
    int canon = cons.raw ? 0 : (cons.tio.c_lflag & ICANON);
    while (n > 0 && cons.r != cons.w && k < sizeof(line)) {
      c = cons.buf[cons.r % INPUT_BUF_SIZE];
      if (canon && c == cons.tio.c_cc[VEOF]) { // end-of-file
        if (n < target) {
          // Save ^D for next time, to make sure
          // caller gets a 0-byte result.
          break;
        }
        cons.r++;
        eof = 1;
        break;
      }
      cons.r++;
      line[k++] = c;
      n--;
      if (canon && c == '\n') {
        // a whole line has arrived, return to
        // the user-level read().
        nl = 1;
        break;
      }
      if (!canon)
        nl = 1; // raw mode returns what is there
    }
    release(&cons.lock);
    if (k > 0 && either_copyout(user_dst, dst, line, k) == -1)
      return target - n - k;
    dst += k;
    if (eof || nl || (n > 0 && k == 0))
      return target - n;
    acquire(&cons.lock);
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
// send a signal to the terminal's foreground group (cons.lock held)
static void
tty_signal(int sig)
{
  int pgrp = cons.pgrp;
  if (pgrp == 0)
    return;
  release(&cons.lock);
  signal_kill(-pgrp, sig, 0);
  acquire(&cons.lock);
}

// echo one character, showing control characters as ^X when ECHOCTL
static void
tty_echo(int c)
{
  if (!(cons.tio.c_lflag & ECHO)) {
    if (c == '\n' && (cons.tio.c_lflag & ECHONL))
      consputc(c);
    return;
  }
  if (c < ' ' && c != '\n' && c != '\t' && (cons.tio.c_lflag & ECHOCTL)) {
    consputc('^');
    consputc(c + '@');
  } else {
    consputc(c);
  }
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character: the line discipline
// (termios) handles signals, editing and echo.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  if (cons.raw) {
    // the console is relaying a guest's terminal: no interpretation
    if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;
      cons.w = cons.e;
      wakeup(&cons.r);
    }
    release(&cons.lock);
    return;
  }

  if (c == 0) {
    release(&cons.lock);
    return;
  }

  struct termios *t = &cons.tio;

  // input translation
  if (c == '\r') {
    if (t->c_iflag & IGNCR) {
      release(&cons.lock);
      return;
    }
    if (t->c_iflag & ICRNL)
      c = '\n';
  } else if (c == '\n' && (t->c_iflag & INLCR)) {
    c = '\r';
  }

  // the signal characters
  if (t->c_lflag & ISIG) {
    if (c == t->c_cc[VINTR]) {
      tty_echo(c);
      cons.e = cons.w; // discard the partial line
      tty_signal(SIGINT);
      release(&cons.lock);
      return;
    }
    if (c == t->c_cc[VQUIT]) {
      tty_echo(c);
      cons.e = cons.w;
      tty_signal(SIGQUIT);
      release(&cons.lock);
      return;
    }
    if (c == t->c_cc[VSUSP]) {
      tty_echo(c);
      cons.e = cons.w;
      tty_signal(SIGTSTP);
      release(&cons.lock);
      return;
    }
  }

  // Ctrl-P prints the process list, as xv6 has always done
  if (c == C('P')) {
    release(&cons.lock);
    procdump();
    return;
  }

  if (t->c_lflag & ICANON) {
    if (c == t->c_cc[VERASE] || c == C('H')) {
      if (cons.e != cons.w) {
        cons.e--;
        if (t->c_lflag & ECHOE)
          consputc(BACKSPACE);
      }
      release(&cons.lock);
      return;
    }
    if (c == t->c_cc[VKILL]) {
      while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n') {
        cons.e--;
        if (t->c_lflag & ECHOK)
          consputc(BACKSPACE);
      }
      release(&cons.lock);
      return;
    }
    if (c == t->c_cc[VWERASE] && (t->c_lflag & IEXTEN)) {
      while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] == ' ')
        cons.e--, consputc(BACKSPACE);
      while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != ' ' &&
             cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n')
        cons.e--, consputc(BACKSPACE);
      release(&cons.lock);
      return;
    }
  }

  if (cons.e - cons.r < INPUT_BUF_SIZE) {
    tty_echo(c);
    cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;
    // a line is ready in canonical mode; every character is ready in raw
    if (!(t->c_lflag & ICANON) || c == '\n' || c == t->c_cc[VEOF] ||
        cons.e - cons.r == INPUT_BUF_SIZE) {
      cons.w = cons.e;
      wakeup(&cons.r);
    }
  }

  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  tty_defaults(&cons.tio);
  cons.ws.ws_row = 24;
  cons.ws.ws_col = 80;
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].ready = consoleready;
  devsw[CONSOLE].ioctl = consoleioctl;
}
