//
// crontab: a user's own table of things to run.
//
//   crontab FILE      install it
//   crontab -l        print it
//   crontab -r        remove it
//   crontab -e        edit it
//   crontab -u USER   whose, for root
//
#include "user/user.h"
#include <pwd.h>
#include <signal.h>

#define SPOOL "/var/spool/cron/crontabs"

static void
path_for(const char *user, char *out, int max)
{
  snprintf(out, max, "%s/%s", SPOOL, user);
}

static int
copy_file(const char *from, const char *to, int mode)
{
  int in = open(from, O_RDONLY);
  if (in < 0) {
    fprintf(stderr, "crontab: %s: %s\n", from, strerror(errno));
    return -1;
  }
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (out < 0) {
    fprintf(stderr, "crontab: %s: %s\n", to, strerror(errno));
    close(in);
    return -1;
  }
  char buf[4096];
  int n;
  while ((n = read(in, buf, sizeof(buf))) > 0)
    if (write(out, buf, n) != n) {
      fprintf(stderr, "crontab: %s: %s\n", to, strerror(errno));
      close(in);
      close(out);
      return -1;
    }
  close(in);
  close(out);
  return 0;
}

// crond re-reads its tables when it is told to
static void
poke_crond(void)
{
  FILE *f = fopen("/var/run/crond.pid", "r");
  int pid = 0;

  if (f && fscanf(f, "%d", &pid) == 1 && pid > 0)
    kill(pid, SIGHUP);
  if (f)
    fclose(f);
}

int
main(int argc, char *argv[])
{
  int list = 0, remove_it = 0, edit = 0, i = 1;
  const char *user = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-l") == 0)
      list = 1;
    else if (strcmp(argv[i], "-r") == 0)
      remove_it = 1;
    else if (strcmp(argv[i], "-e") == 0)
      edit = 1;
    else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
      user = argv[++i];
    else {
      fprintf(stderr, "usage: crontab [-u USER] [-l|-r|-e|FILE]\n");
      exit(1);
    }
  }
  struct passwd *me = getpwuid(getuid());
  if (user && geteuid() != 0) {
    fprintf(stderr, "crontab: only root may use -u\n");
    exit(1);
  }
  if (user == 0)
    user = me ? me->pw_name : "root";
  if (getpwnam(user) == 0) {
    fprintf(stderr, "crontab: %s: no such user\n", user);
    exit(1);
  }

  char path[256];
  path_for(user, path, sizeof(path));
  mkdir("/var/spool", 0755);
  mkdir("/var/spool/cron", 0755);
  mkdir(SPOOL, 0755);

  if (list) {
    FILE *f = fopen(path, "r");
    if (f == 0) {
      fprintf(stderr, "crontab: no table for %s\n", user);
      exit(1);
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
      fputs(line, stdout);
    fclose(f);
    exit(0);
  }
  if (remove_it) {
    if (unlink(path) < 0) {
      fprintf(stderr, "crontab: no table for %s\n", user);
      exit(1);
    }
    poke_crond();
    exit(0);
  }
  if (edit) {
    const char *editor = getenv("EDITOR");
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/crontab.%d", (int)getpid());
    if (access(path, R_OK) == 0)
      copy_file(path, tmp, 0600);
    else {
      int fd = open(tmp, O_WRONLY | O_CREAT, 0600);
      if (fd >= 0) {
        const char *help = "# minute hour day-of-month month day-of-week command\n";
        write(fd, help, strlen(help));
        close(fd);
      }
    }
    if (editor == 0) {
      fprintf(stderr, "crontab: set EDITOR, or give a file: crontab FILE\n");
      unlink(tmp);
      exit(1);
    }
    pid_t pid = fork();
    if (pid == 0) {
      char *a[] = {(char *)editor, tmp, 0};
      execvp(editor, a);
      exit(1);
    }
    int st;
    waitpid(pid, &st, 0);
    if (copy_file(tmp, path, 0600) < 0) {
      unlink(tmp);
      exit(1);
    }
    unlink(tmp);
    poke_crond();
    exit(0);
  }

  if (i >= argc) {
    fprintf(stderr, "usage: crontab [-u USER] [-l|-r|-e|FILE]\n");
    exit(1);
  }
  if (copy_file(argv[i], path, 0600) < 0)
    exit(1);
  poke_crond();
  printf("crontab: table for %s installed\n", user);
  exit(0);
}
