// strace: every system call a program makes, as it makes them.
//
//   strace [-f] [-c] [-e NAME[,NAME...]] [-o FILE] [-p PID] COMMAND [ARG...]
//
//   -e  only these calls, by name
//   -c  a count per call at the end instead of a line per call
//   -o  write to this file rather than to standard error
//   -s  how much of a string to print (32 by default)
//   -T  how long each call took
//
// it works the way it has always worked: fork, the child asks to be
// watched and execs, and the parent walks it forward a system call at
// a time. each stop hands over the registers -- a7 is the number and
// a0 to a5 the arguments on the way in, a0 the answer on the way out.
//
// -p is not offered. Attaching to a process that is already running
// means taking it away from its parent, and this kernel's ptrace does
// not do that on purpose: a tracer here is a parent, so a program is
// only ever watched by something that started it.
#include "user/user.h"
#include "kernel/ptrace.h"

// the same shape the kernel hands over
struct user_regs_struct {
  uint64 pc;
  uint64 ra, sp, gp, tp;
  uint64 t0, t1, t2;
  uint64 s0, s1;
  uint64 a0, a1, a2, a3, a4, a5, a6, a7;
  uint64 s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
  uint64 t3, t4, t5, t6;
};

#define MAXCALL 256

// the names come from the header that numbers the calls, turned into
// a table by the build (user/syscallnames.h). writing them out here
// by hand would go quietly wrong the first time a call was added, and
// strace would then name the wrong one.
static const char *const names[] = {
#include "user/syscallnames.h"
};
#define NNAMES ((int)(sizeof(names) / sizeof(names[0])))

static long counts[MAXCALL];
static long errors[MAXCALL];
static int only[MAXCALL];
static int have_filter;
static int summary, show_time, strmax = 32;
static FILE *out;
static long total_calls;

// how each call's arguments are best shown: s for a string, d for a
// number and x for an address. a call that is not named here has its
// first three arguments printed as numbers, which is honest about
// what is and is not known.
struct callinfo {
  const char *name;
  const char *args;
};

static const struct callinfo known[] = {
  {"exit", "d"},        {"wait", "x"},        {"pipe", "x"},
  {"read", "dxd"},      {"write", "dsd"},     {"close", "d"},
  {"kill", "dd"},       {"exec", "sx"},       {"execve", "sxx"},
  {"fstat", "dx"},      {"chdir", "s"},       {"dup", "d"},
  {"dup2", "dd"},       {"sbrk", "dd"},       {"open", "sx"},
  {"openat", "dsx"},    {"mknod", "sdd"},     {"unlink", "s"},
  {"unlinkat", "dsd"},  {"link", "ss"},       {"linkat", "dsdsd"},
  {"mkdir", "sd"},      {"mkdirat", "dsd"},   {"stat", "sx"},
  {"lstat", "sx"},      {"fstatat", "dsxd"},  {"chmod", "sd"},
  {"chown", "sdd"},     {"rename", "ss"},     {"renameat", "dsds"},
  {"symlink", "ss"},    {"readlink", "sxd"},  {"lseek", "ddd"},
  {"ioctl", "dxx"},     {"fcntl", "ddx"},     {"mmap", "xdddd"},
  {"munmap", "xd"},     {"mprotect", "xdd"},  {"nanosleep", "xx"},
  {"getcwd", "xd"},     {"mount", "sssx"},    {"umount", "s"},
  {"truncate", "sd"},   {"ftruncate", "dd"},  {"access", "sd"},
  {"faccessat", "dsdd"},{"getdents", "dxd"},  {"pread", "dxdd"},
  {"pwrite", "dxdd"},   {"socket", "d"},      {"connect", "dxd"},
  {"bind", "dd"},       {"listen", "dd"},     {"accept", "dxx"},
  {"sendto", "dxdxd"},  {"recvfrom", "dxdxxd"},
  {"setuid", "d"},      {"setgid", "d"},      {"chroot", "s"},
  {"ptrace", "ddxx"},   {"sockopt", "ddd"},
  // the ones that take nothing, so that they print as they should
  {"fork", ""},         {"getpid", ""},       {"gettid", ""},
  {"getppid", ""},      {"getuid", ""},       {"geteuid", ""},
  {"getgid", ""},       {"getegid", ""},      {"uptime", ""},
  {"sync", ""},         {"setsid", ""},       {"umask", "d"},
  {0, 0}
};

static const char *
call_name(int n)
{
  static char buf[24];

  if (n > 0 && n < NNAMES && names[n])
    return names[n];
  snprintf(buf, sizeof(buf), "syscall_%d", n);
  return buf;
}

static const char *
call_args(int n)
{
  const char *name = (n > 0 && n < NNAMES) ? names[n] : 0;

  if (name == 0)
    return 0;
  for (int i = 0; known[i].name; i++)
    if (strcmp(known[i].name, name) == 0)
      return known[i].args;
  return 0;
}

// one word out of the traced process
static int
peek(int pid, uint64 addr, uint64 *out_word)
{
  return (int)ptrace(PTRACE_PEEKDATA, pid, (void *)addr, out_word);
}

// a string out of it, one word at a time, printed the way strace
// prints one: in quotes, with the escapes, and cut off at -s
static void
print_string(int pid, uint64 addr)
{
  char buf[512];
  int n = 0;

  if (addr == 0) {
    fprintf(out, "NULL");
    return;
  }
  while (n < strmax && n < (int)sizeof(buf) - 8) {
    uint64 word = 0;
    if (peek(pid, addr + n, &word) < 0)
      break;
    int done = 0;
    for (int k = 0; k < 8 && n < strmax; k++) {
      char c = (char)((word >> (k * 8)) & 0xff);
      if (c == 0) {
        done = 1;
        break;
      }
      buf[n++] = c;
    }
    if (done)
      break;
  }
  fprintf(out, "\"");
  for (int i = 0; i < n; i++) {
    unsigned char c = buf[i];
    if (c == '\n')
      fprintf(out, "\\n");
    else if (c == '\t')
      fprintf(out, "\\t");
    else if (c == '\r')
      fprintf(out, "\\r");
    else if (c == '"')
      fprintf(out, "\\\"");
    else if (c == '\\')
      fprintf(out, "\\\\");
    else if (c >= 0x20 && c < 0x7f)
      fputc(c, out);
    else
      fprintf(out, "\\%o", c);
  }
  fprintf(out, "\"");
  if (n >= strmax)
    fprintf(out, "...");
}

static void
print_call(int pid, const struct user_regs_struct *r)
{
  int num = (int)r->a7;
  const char *fmt = call_args(num);
  uint64 a[6] = {r->a0, r->a1, r->a2, r->a3, r->a4, r->a5};

  fprintf(out, "%s(", call_name(num));
  if (fmt) {
    for (int i = 0; fmt[i] && i < 6; i++) {
      if (i)
        fprintf(out, ", ");
      switch (fmt[i]) {
      case 's': print_string(pid, a[i]); break;
      case 'd': fprintf(out, "%lld", (long long)a[i]); break;
      case 'x': fprintf(out, "0x%llx", (unsigned long long)a[i]); break;
      default: fprintf(out, "%lld", (long long)a[i]); break;
      }
    }
  } else {
    // nothing is known about this one's arguments, so the first three
    // go out as numbers rather than being guessed at
    fprintf(out, "%lld, %lld, %lld", (long long)a[0], (long long)a[1],
            (long long)a[2]);
  }
  fprintf(out, ")");
}

static void
print_result(long long ret)
{
  if (ret < 0 && ret > -256)
    fprintf(out, " = -1 %s\n", strerror((int)-ret));
  else
    fprintf(out, " = %lld\n", ret);
}

static void
usage(void)
{
  fprintf(stderr, "usage: strace [-f] [-c] [-T] [-s N] [-e NAME,...]"
                  " [-o FILE] COMMAND [ARG...]\n");
  exit(1);
}

static void
add_filter(const char *list)
{
  char name[64];
  int n = 0;

  have_filter = 1;
  for (const char *p = list;; p++) {
    if (*p == ',' || *p == 0) {
      name[n] = 0;
      if (n > 0) {
        int found = 0;
        for (int k = 1; k < NNAMES && k < MAXCALL; k++)
          if (names[k] && strcmp(names[k], name) == 0) {
            only[k] = 1;
            found = 1;
          }
        if (!found)
          fprintf(stderr, "strace: there is no call named %s\n", name);
      }
      n = 0;
      if (*p == 0)
        return;
      continue;
    }
    if (n < (int)sizeof(name) - 1)
      name[n++] = *p;
  }
}

int
main(int argc, char *argv[])
{
  int i = 1;

  out = stderr;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strcmp(a, "-p") == 0) {
      fprintf(stderr,
              "strace: -p is not offered here. Attaching would mean taking\n"
              "a process away from its parent, and this kernel's ptrace\n"
              "does not: a tracer is a parent. Start the program under\n"
              "strace instead.\n");
      exit(1);
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 'f': break;  // children are not followed; see the note above
      case 'c': summary = 1; break;
      case 'T': show_time = 1; break;
      case 'e': case 'o': case 's': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        if (*p == 'e') {
          // -e trace=NAME,... as well as -e NAME,...
          const char *eq = strchr(v, '=');
          add_filter(eq ? eq + 1 : v);
        } else if (*p == 's') {
          strmax = atoi(v);
          if (strmax < 1)
            strmax = 1;
          if (strmax > 400)
            strmax = 400;
        } else {
          out = fopen(v, "w");
          if (out == 0) {
            fprintf(stderr, "strace: %s: %s\n", v, strerror(errno));
            exit(1);
          }
        }
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  if (i >= argc)
    usage();

  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "strace: fork: %s\n", strerror(errno));
    exit(1);
  }
  if (pid == 0) {
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
      fprintf(stderr, "strace: this kernel has no ptrace\n");
      _exit(1);
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "strace: %s: %s\n", argv[i], strerror(errno));
    _exit(127);
  }

  // the first stop is the exec, before the program has run at all
  int st = 0;
  if (waitpid(pid, &st, WUNTRACED) < 0) {
    fprintf(stderr, "strace: waitpid: %s\n", strerror(errno));
    exit(1);
  }
  if (WIFEXITED(st)) {
    // it never got as far as the new program
    exit(WEXITSTATUS(st));
  }

  int entering = 1;
  int pending_num = -1;
  int t0 = 0;
  int line_open = 0;   // a call has been printed and wants its answer

  for (;;) {
    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0)
      break;
    if (waitpid(pid, &st, WUNTRACED) < 0)
      break;
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
      // it went away inside the call whose entry was just printed;
      // real strace finishes that line with a question mark. a call
      // that was filtered out printed nothing, so it wants nothing.
      if (line_open) {
        fprintf(out, " = ?\n");
        fflush(out);
      }
      break;
    }
    if (!WIFSTOPPED(st))
      break;

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0)
      break;

    int num = (int)regs.a7;
    if (entering) {
      pending_num = num;
      if (!have_filter || (num > 0 && num < MAXCALL && only[num])) {
        if (!summary) {
          print_call(pid, &regs);
          line_open = 1;
          fflush(out);
        }
        if (show_time)
          t0 = uptime();
      }
      total_calls++;
      if (num > 0 && num < MAXCALL)
        counts[num]++;
    } else {
      long long ret = (long long)regs.a0;
      if (!have_filter ||
          (pending_num > 0 && pending_num < MAXCALL && only[pending_num])) {
        if (!summary) {
          print_result(ret);
          line_open = 0;
          if (show_time)
            fprintf(out, " <%d.%02d>\n", (uptime() - t0) / 100,
                    (uptime() - t0) % 100);
          fflush(out);
        }
      }
      if (ret < 0 && pending_num > 0 && pending_num < MAXCALL)
        errors[pending_num]++;
    }
    entering = !entering;
  }

  // whatever it exited with is what strace exits with
  int code = 0;
  if (WIFEXITED(st))
    code = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    code = 128 + WTERMSIG(st);

  if (summary) {
    fprintf(out, "%% time     calls    errors syscall\n");
    fprintf(out, "------ --------- --------- ----------------\n");
    for (int n = 0; n < MAXCALL; n++) {
      if (counts[n] == 0)
        continue;
      long pct = total_calls ? counts[n] * 1000 / total_calls : 0;
      fprintf(out, "%3ld.%ld %9ld %9ld %s\n", pct / 10, pct % 10, counts[n],
              errors[n], call_name(n));
    }
    fprintf(out, "------ --------- --------- ----------------\n");
    fprintf(out, "100.0 %9ld           total\n", total_calls);
  } else {
    fprintf(out, "+++ exited with %d +++\n", code);
  }
  if (out != stderr)
    fclose(out);
  exit(code);
}
