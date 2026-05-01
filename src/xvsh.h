/*
 * xvsh.h — shared types, shell state, and forward declarations
 *
 * Xvsh: a POSIX-compliant, Lua-configurable shell
 */
#ifndef XVSH_H
#define XVSH_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>

/* ── Constants ────────────────────────────────────────────────── */
#define XVSH_VERSION      "0.1.0"
#define MAX_ALIASES       256
#define MAX_LUA_BUILTINS  64
#define XVSHRC_FILENAME   "/.xvshrc"
#define EXPAND_MAX        65536   /* max expanded word length */

/* ── Alias table entry ────────────────────────────────────────── */
typedef struct {
    char *name;
    char *value;
} Alias;

/* ── Shell state (global singleton) ──────────────────────────── */
typedef struct ShellState {
    char      name[PATH_MAX];   /* argv[0]                     */
    char      cwd[PATH_MAX];    /* current working dir         */
    int       last_exit;        /* $? — last exit code         */
    pid_t     pid;              /* $$ — shell PID              */
    int       interactive;      /* 1 if connected to a tty     */

    /* Alias table */
    Alias     aliases[MAX_ALIASES];
    int       alias_count;

    /* Lua interpreter */
    lua_State *L;
    int        has_preexec;     /* preexec hook registered?    */
    int        has_postexec;    /* postexec hook registered?   */
} ShellState;

extern ShellState shell;

/* ── shell.c / main helpers ───────────────────────────────────── */
void        shell_init(ShellState *sh, const char *argv0);
void        shell_cleanup(ShellState *sh);
int         execute_line(ShellState *sh, const char *line);
int         run_file(ShellState *sh, const char *path);

/* ── Alias helpers ────────────────────────────────────────────── */
const char *alias_lookup(ShellState *sh, const char *name);
void        alias_set(ShellState *sh, const char *name, const char *value);
void        alias_remove(ShellState *sh, const char *name);

/* ── Word expansion (variable/tilde/command substitution) ─────── */
char       *expand_word(ShellState *sh, const char *raw);

#endif /* XVSH_H */
