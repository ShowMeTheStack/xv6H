#ifndef _EDITLINE_H
#define _EDITLINE_H
#include <sys/cdefs.h>

//
// editline: read one line from a terminal, with editing, history,
// completion and a redraw that touches as few characters as it can.
// the shell uses it for its prompt, and any program that asks a
// question can use it too.
//

// read a line. the prompt is written to the terminal and is not part
// of the answer. the caller owns the returned string and gives it back
// with el_free() (which is free()); a newline is never in it. 0 comes
// back at the end of the input: ctrl-D on an empty line, or a closed
// descriptor. when the standard input is not a terminal the line is
// read plainly, without editing, escape codes or a prompt, so that a
// script reads the same way a person does.
char *el_readline(const char *prompt);

// give back a line from el_readline. it is free(), named for symmetry.
void el_free(char *line);

// add a line to the history. an empty line, and a line equal to the
// one already at the end, are ignored, so holding a key down or
// repeating a command does not fill the list. returns 1 when the line
// was added, 0 when it was ignored, -1 when memory ran out. the
// history keeps its own copy.
int el_history_add(const char *line);

// read a history file, one entry per line, adding each to what is
// already there on the same terms as el_history_add. returns how many
// entries it added, or -1 if the file could not be opened.
int el_history_load(const char *path);

// write the history to a file, one entry per line, replacing whatever
// was there. returns 0, or -1 if the file could not be written.
int el_history_save(const char *path);

// forget every entry and free the memory they hold.
void el_history_clear(void);

// how many entries the history holds.
int el_history_count(void);

// entry i, where 0 is the oldest and el_history_count()-1 the newest,
// or 0 when i is out of range. the string belongs to the library and
// is only valid until the history changes; copy it to keep it.
const char *el_history_get(int i);

// keep at most n entries, dropping the oldest first. the default is
// 500; a value below 1 is ignored.
void el_history_max(int n);

// the completer the tab key calls. it is handed the line and the
// cursor's offset into it, and answers with an array of replacements
// for the word that ends at the cursor (the run of characters back to
// the nearest blank). it allocates the array and each string with
// malloc(); editline frees both with free() when it is done. setting
// *nmatches to 0 (or leaving *matches 0) means it found nothing.
typedef void (*el_completer)(const char *line, int point, char ***matches, int *nmatches);

// install the completer, or 0 to make the tab key do nothing. the
// shell supplies file and command completion.
void el_set_completer(el_completer fn);

// choose what a line wider than the terminal does: off (the default)
// scrolls it within one row, on wraps it across as many rows as it
// needs.
void el_set_multiline(int on);

// the editing engine on an arbitrary descriptor, for tests: keys are
// read from fd, nothing is drawn, and the terminal is not touched.
// the return value is owned like el_readline's.
char *__el_edit_from(int fd, const char *prompt);

#endif
