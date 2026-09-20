//
// jobs and the terminal.
//
// every job is a process group. the shell gives the terminal to a
// foreground job and takes it back when the job stops or finishes,
// which is what makes ctrl-C reach the job and not the shell.
//
#include "sh.h"

static struct job *jobs;
static int next_job_id = 1;

void
jobs_init(void)
{
  jobs = 0;
  next_job_id = 1;
}

struct job *
job_new(int pgid, const char *command, int background)
{
  struct job *j = xmalloc(sizeof(*j));

  memset(j, 0, sizeof(*j));
  j->pgid = pgid;
  j->command = xstrdup(command ? command : "");
  j->background = background;
  j->id = next_job_id++;
  j->next = jobs;
  jobs = j;
  return j;
}

void
job_add_pid(struct job *j, int pid)
{
  if (j->npids < MAXPROCS)
    j->pids[j->npids++] = pid;
}

static void
job_remove(struct job *j)
{
  struct job **pp = &jobs;

  while (*pp) {
    if (*pp == j) {
      *pp = j->next;
      free(j->command);
      free(j);
      if (jobs == 0)
        next_job_id = 1; // start over when nothing is left
      return;
    }
    pp = &(*pp)->next;
  }
}

static int
job_done(struct job *j)
{
  for (int i = 0; i < j->npids; i++)
    if (!j->done[i])
      return 0;
  return 1;
}

// record what happened to one process
static int
note_status(int pid, int status)
{
  for (struct job *j = jobs; j; j = j->next)
    for (int i = 0; i < j->npids; i++)
      if (j->pids[i] == pid) {
        j->status[i] = status;
        if (WIFSTOPPED(status)) {
          j->stopped = 1;
        } else {
          j->done[i] = 1;
          j->stopped = 0;
        }
        return 1;
      }
  return 0;
}

void
give_terminal(int pgid)
{
  if (!interactive || shell_tty < 0)
    return;
  signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(shell_tty, pgid);
}

// wait for a foreground job, and hand the terminal back afterwards
void
job_wait(struct job *j)
{
  int status;

  while (!job_done(j) && !j->stopped) {
    int pid = waitpid(-1, &status, WUNTRACED);
    if (pid < 0) {
      if (errno == EINTR) {
        trap_run_pending();
        continue;
      }
      break;
    }
    note_status(pid, status);
    // the job's status is the last process's, as POSIX says
    if (pid == j->pids[j->npids - 1]) {
      if (WIFEXITED(status))
        last_status = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        last_status = 128 + WTERMSIG(status);
      else if (WIFSTOPPED(status))
        last_status = 128 + WSTOPSIG(status);
    }
  }
  give_terminal(shell_pgid);

  if (j->stopped) {
    printf("[%d]+  Stopped                 %s\n", j->id, j->command);
    j->notified = 1;
  } else {
    job_remove(j);
  }
}

// collect whatever has finished in the background
void
job_reap(int notify)
{
  int status;

  for (;;) {
    int pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
    if (pid <= 0)
      break;
    if (!note_status(pid, status))
      continue;
  }
  for (struct job *j = jobs; j;) {
    struct job *next = j->next;
    if (job_done(j)) {
      if (notify && j->background) {
        int st = j->status[j->npids - 1];
        if (WIFSIGNALED(st) && WTERMSIG(st) != SIGINT)
          printf("[%d]+  %s                 %s\n", j->id, strsignal(WTERMSIG(st)), j->command);
        else
          printf("[%d]+  Done                    %s\n", j->id, j->command);
      }
      job_remove(j);
    } else if (j->stopped && !j->notified && notify) {
      printf("[%d]+  Stopped                 %s\n", j->id, j->command);
      j->notified = 1;
    }
    j = next;
  }
}

void
job_list(int mode)
{
  for (struct job *j = jobs; j; j = j->next) {
    const char *state = j->stopped ? "Stopped" : "Running";
    if (mode == 'l')
      printf("[%d]+  %d %s                 %s\n", j->id, j->pgid, state, j->command);
    else if (mode == 'p')
      printf("%d\n", j->pgid);
    else
      printf("[%d]+  %-23s %s\n", j->id, state, j->command);
  }
}

// %n, %+, %-, %string or a bare number
struct job *
job_find(const char *spec)
{
  if (spec == 0 || *spec == 0 || strcmp(spec, "%+") == 0 || strcmp(spec, "%%") == 0)
    return jobs; // the most recent
  if (strcmp(spec, "%-") == 0)
    return jobs ? jobs->next : 0;
  if (spec[0] == '%')
    spec++;
  if (isdigit((unsigned char)spec[0])) {
    int id = atoi(spec);
    for (struct job *j = jobs; j; j = j->next)
      if (j->id == id)
        return j;
    return 0;
  }
  for (struct job *j = jobs; j; j = j->next)
    if (strncmp(j->command, spec, strlen(spec)) == 0)
      return j;
  return 0;
}

int
job_fg(struct job *j)
{
  printf("%s\n", j->command);
  give_terminal(j->pgid);
  if (j->stopped) {
    kill(-j->pgid, SIGCONT);
    j->stopped = 0;
  }
  j->background = 0;
  job_wait(j);
  return last_status;
}

int
job_bg(struct job *j)
{
  printf("[%d]+ %s &\n", j->id, j->command);
  if (j->stopped) {
    kill(-j->pgid, SIGCONT);
    j->stopped = 0;
  }
  j->background = 1;
  return 0;
}

void
job_free_all(void)
{
  while (jobs)
    job_remove(jobs);
}
