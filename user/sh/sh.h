#ifndef XV6_SH_H
#define XV6_SH_H
//
// the shell: what its parts share.
//
// the shape follows the POSIX grammar. a script is a list of
// and-or lists; each is a sequence of pipelines joined by && and ||;
// each pipeline is a sequence of commands joined by |; and a command
// is either a simple command (words and redirections) or a compound
// one (if, while, until, for, case, a brace group, a subshell) or a
// function definition.
//
#include "user/user.h"
#include <termios.h>

#define MAXWORD 4096   // the longest single word
#define MAXARGS 256    // arguments to one command
#define MAXHEREDOC 16  // here-documents queued on one line

// ---- words ----
//
// a word is kept as text with a mark on every character saying whether
// it was quoted, because that decides what happens during expansion:
// quoted text is not split into fields and does not become a pattern.

// the mark beside each character of a word. quoting has two separate
// effects and a single bit cannot hold both: double quotes stop field
// splitting and pathname expansion but not parameter expansion, while
// single quotes and a backslash stop all three.
#define Q_QUOTED  1  // not split into fields, not a pattern
#define Q_LITERAL 2  // not expanded either

struct word {
  char *text;
  char *quoted;     // one byte of Q_* flags per character of text
  int len;
  struct word *next;
};

// ---- redirections ----

enum redir_kind {
  R_IN,       // < file
  R_OUT,      // > file
  R_APPEND,   // >> file
  R_CLOBBER,  // >| file
  R_RDWR,     // <> file
  R_DUPIN,    // <& n or <& word
  R_DUPOUT,   // >& n or >& word
  R_HEREDOC,  // << word
  R_HERESTR,  // <<< word (an extension, and a convenient one)
};

struct redir {
  enum redir_kind kind;
  int fd;             // the descriptor the redirection acts on
  struct word *word;  // the file name, or the here-document's delimiter
  char *heredoc;      // the text of a here-document, once it is read
  int heredoc_quoted; // whether the delimiter was quoted: no expansion then
  struct redir *next;
};

// ---- commands ----

enum cmd_kind {
  C_SIMPLE,
  C_PIPE,
  C_LIST,      // ; or & or a newline
  C_AND,       // &&
  C_OR,        // ||
  C_SUBSHELL,  // ( ... )
  C_GROUP,     // { ... }
  C_IF,
  C_WHILE,
  C_UNTIL,
  C_FOR,
  C_CASE,
  C_FUNCTION,
  C_NOT,       // ! pipeline
};

struct case_item {
  struct word *patterns;
  struct cmd *body;
  struct case_item *next;
};

struct cmd {
  enum cmd_kind kind;
  struct redir *redirs;

  // C_SIMPLE
  struct word *words;        // the command and its arguments, before expansion
  struct word *assigns;      // NAME=value pairs in front of the command

  // C_PIPE, C_LIST, C_AND, C_OR
  struct cmd *left, *right;
  int background;            // the list's last command ended with &

  // C_IF
  struct cmd *cond, *then_part, *else_part;

  // C_WHILE, C_UNTIL, C_FOR, C_SUBSHELL, C_GROUP, C_NOT
  struct cmd *body;

  // C_FOR
  char *var;
  struct word *items;
  int has_items;             // "for x" with no "in" walks the arguments

  // C_CASE
  struct word *subject;
  struct case_item *items_case;

  // C_FUNCTION
  char *fname;
};

// ---- variables ----

struct var {
  char *name;
  char *value;
  int exported;
  int readonly;
  struct var *next;
};

// ---- functions ----

struct func {
  char *name;
  struct cmd *body;
  struct func *next;
};

// ---- jobs ----

#define MAXPROCS 32

struct job {
  int id;                    // the number jobs(1) shows
  int pgid;
  char *command;             // the text, for the jobs listing
  int npids;
  int pids[MAXPROCS];
  int done[MAXPROCS];
  int status[MAXPROCS];
  int stopped;
  int notified;
  int background;
  struct job *next;
};

// ---- the shell's state ----

extern int last_status;       // $?
extern int shell_pid;         // $$
extern int last_bg_pid;       // $!
extern int interactive;
extern int shell_tty;
extern struct termios saved_tty;
extern int shell_pgid;
extern char **script_args;    // $1, $2, ...
extern int nscript_args;      // $#
extern char *script_name;     // $0
extern int opt_errexit, opt_xtrace, opt_noexec, opt_nounset, opt_verbose, opt_noglob, opt_monitor;

// sh.c
void sh_error(const char *fmt, ...);
void *xmalloc(size_t n);
char *xstrdup(const char *s);
int run_string(const char *text, int report_syntax);
int run_file(const char *path);

// lex.c and parse.c
struct parser;
struct parser *parser_new(const char *text, int interactive_input);
void parser_free(struct parser *p);
struct cmd *parse_command(struct parser *p, int *eof);
int parser_error(struct parser *p);
void cmd_free(struct cmd *c);
struct word *word_new(const char *text, const char *quoted, int len);
void word_free(struct word *w);
char *word_text(struct word *w);

// expand.c
struct wordlist {
  char **words;
  int n, cap;
};
void wl_init(struct wordlist *wl);
void wl_add(struct wordlist *wl, char *s);
void wl_free(struct wordlist *wl);
void expand_word_list(struct word *w, struct wordlist *out, int for_assign);
char *expand_to_string(struct word *w, int quoted_context);
char *expand_here(const char *text);
int pattern_match(const char *pattern, const char *string);

// vars.c
void var_init(char **envp);
const char *var_get(const char *name);
void var_set(const char *name, const char *value, int exported);
void var_unset(const char *name);
int var_export(const char *name, int on);
int var_readonly(const char *name, int on);
char **var_environ(void);
void var_dump(int exported_only);
void var_set_int(const char *name, long v);

// func.c
struct func *func_find(const char *name);
void func_define(const char *name, struct cmd *body);
int func_undefine(const char *name);
void func_list(void);

// exec.c
int run_cmd(struct cmd *c);
int run_cmd_status(struct cmd *c);
extern int loop_break, loop_continue, function_return, exiting, exit_code;

// builtin.c
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn builtin_lookup(const char *name);
int builtin_special(const char *name);

// jobs.c
void jobs_init(void);
struct job *job_new(int pgid, const char *command, int background);
void job_add_pid(struct job *j, int pid);
void job_wait(struct job *j);
void job_reap(int notify);
void job_list(int mode);
struct job *job_find(const char *spec);
int job_fg(struct job *j);
int job_bg(struct job *j);
void job_free_all(void);
void give_terminal(int pgid);

// trap.c
void trap_set(int sig, const char *action);
const char *trap_get(int sig);
void trap_run_pending(void);
void trap_on_exit(void);
int trap_signal_number(const char *name);
void trap_list(void);
void trap_reset_for_child(void);

// alias.c
const char *alias_get(const char *name);
void alias_set(const char *name, const char *value);
int alias_unset(const char *name);
void alias_list(void);

#endif
