//
// shell variables, functions and aliases.
//
// a variable lives in one list whether it is exported or not; the
// environment a child gets is built from the exported ones when it is
// needed, so nothing has to be kept in step with environ.
//
#include "sh.h"

static struct var *vars;
static struct func *funcs;

struct alias {
  char *name;
  char *value;
  struct alias *next;
};
static struct alias *aliases;

static struct var *
find(const char *name)
{
  for (struct var *v = vars; v; v = v->next)
    if (strcmp(v->name, name) == 0)
      return v;
  return 0;
}

void
var_init(char **envp)
{
  for (char **e = envp; e && *e; e++) {
    char *eq = strchr(*e, '=');
    if (eq == 0)
      continue;
    int n = eq - *e;
    char *name = xmalloc(n + 1);
    memmove(name, *e, n);
    name[n] = 0;
    var_set(name, eq + 1, 1);
    free(name);
  }
  // the shell sets these itself if the environment did not
  if (var_get("PS1") == 0)
    var_set("PS1", "$ ", 0);
  if (var_get("PS2") == 0)
    var_set("PS2", "> ", 0);
  if (var_get("IFS") == 0)
    var_set("IFS", " \t\n", 0);
  if (var_get("PATH") == 0)
    var_set("PATH", "/:/bin:/usr/bin", 1);
}

const char *
var_get(const char *name)
{
  struct var *v = find(name);

  return v ? v->value : 0;
}

void
var_set(const char *name, const char *value, int exported)
{
  struct var *v = find(name);

  if (v && v->readonly) {
    sh_error("%s: is read only", name);
    return;
  }
  if (v == 0) {
    v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->name = xstrdup(name);
    v->next = vars;
    vars = v;
  } else {
    free(v->value);
  }
  v->value = xstrdup(value ? value : "");
  if (exported)
    v->exported = 1;
}

void
var_set_int(const char *name, long value)
{
  char buf[32];

  snprintf(buf, sizeof(buf), "%ld", value);
  var_set(name, buf, 0);
}

void
var_unset(const char *name)
{
  struct var **pp = &vars;

  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      struct var *v = *pp;
      if (v->readonly) {
        sh_error("%s: is read only", name);
        return;
      }
      *pp = v->next;
      free(v->name);
      free(v->value);
      free(v);
      return;
    }
    pp = &(*pp)->next;
  }
}

int
var_export(const char *name, int on)
{
  struct var *v = find(name);

  if (v == 0) {
    if (!on)
      return 0;
    var_set(name, "", 1);
    return 0;
  }
  v->exported = on;
  return 0;
}

int
var_readonly(const char *name, int on)
{
  struct var *v = find(name);

  if (v == 0) {
    var_set(name, "", 0);
    v = find(name);
  }
  if (v)
    v->readonly = on;
  return 0;
}

// the environment for a child: every exported variable, as NAME=value
char **
var_environ(void)
{
  int n = 0;

  for (struct var *v = vars; v; v = v->next)
    if (v->exported)
      n++;
  char **env = xmalloc((n + 1) * sizeof(char *));
  int i = 0;
  for (struct var *v = vars; v; v = v->next) {
    if (!v->exported)
      continue;
    int len = strlen(v->name) + strlen(v->value) + 2;
    env[i] = xmalloc(len);
    snprintf(env[i], len, "%s=%s", v->name, v->value);
    i++;
  }
  env[i] = 0;
  return env;
}

// the quoting `set` and `export -p` use, so that the output can be
// read back by the shell
static void
print_quoted(const char *s)
{
  int needs = (*s == 0);

  for (const char *p = s; *p; p++)
    if (!isalnum((unsigned char)*p) && strchr("_-./:=@%+,", *p) == 0)
      needs = 1;
  if (!needs) {
    printf("%s", s);
    return;
  }
  printf("'");
  for (const char *p = s; *p; p++) {
    if (*p == '\'')
      printf("'\\''");
    else
      putchar(*p);
  }
  printf("'");
}

void
var_dump(int exported_only)
{
  // sorted, so that the output is the same from one run to the next
  int n = 0;
  for (struct var *v = vars; v; v = v->next)
    if (!exported_only || v->exported)
      n++;
  struct var **list = xmalloc((n + 1) * sizeof(*list));
  int i = 0;
  for (struct var *v = vars; v; v = v->next)
    if (!exported_only || v->exported)
      list[i++] = v;
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (strcmp(list[a]->name, list[b]->name) > 0) {
        struct var *t = list[a];
        list[a] = list[b];
        list[b] = t;
      }
  for (int k = 0; k < n; k++) {
    if (exported_only)
      printf("export ");
    printf("%s=", list[k]->name);
    print_quoted(list[k]->value);
    printf("\n");
  }
  free(list);
}

// ---- functions ----

struct func *
func_find(const char *name)
{
  for (struct func *f = funcs; f; f = f->next)
    if (strcmp(f->name, name) == 0)
      return f;
  return 0;
}

void
func_define(const char *name, struct cmd *body)
{
  struct func *f = func_find(name);

  if (f) {
    cmd_free(f->body);
    f->body = body;
    return;
  }
  f = xmalloc(sizeof(*f));
  f->name = xstrdup(name);
  f->body = body;
  f->next = funcs;
  funcs = f;
}

int
func_undefine(const char *name)
{
  struct func **pp = &funcs;

  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      struct func *f = *pp;
      *pp = f->next;
      free(f->name);
      cmd_free(f->body);
      free(f);
      return 0;
    }
    pp = &(*pp)->next;
  }
  return -1;
}

void
func_list(void)
{
  for (struct func *f = funcs; f; f = f->next)
    printf("%s()\n", f->name);
}

// ---- aliases ----

const char *
alias_get(const char *name)
{
  for (struct alias *a = aliases; a; a = a->next)
    if (strcmp(a->name, name) == 0)
      return a->value;
  return 0;
}

void
alias_set(const char *name, const char *value)
{
  for (struct alias *a = aliases; a; a = a->next)
    if (strcmp(a->name, name) == 0) {
      free(a->value);
      a->value = xstrdup(value);
      return;
    }
  struct alias *a = xmalloc(sizeof(*a));
  a->name = xstrdup(name);
  a->value = xstrdup(value);
  a->next = aliases;
  aliases = a;
}

int
alias_unset(const char *name)
{
  struct alias **pp = &aliases;

  while (*pp) {
    if (strcmp((*pp)->name, name) == 0) {
      struct alias *a = *pp;
      *pp = a->next;
      free(a->name);
      free(a->value);
      free(a);
      return 0;
    }
    pp = &(*pp)->next;
  }
  return -1;
}

void
alias_list(void)
{
  for (struct alias *a = aliases; a; a = a->next) {
    printf("alias %s=", a->name);
    print_quoted(a->value);
    printf("\n");
  }
}
