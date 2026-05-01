/*
 * builtins.h — built-in command declarations for xvsh
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "xvsh.h"

/* Returns 1 if `name` is a built-in or Lua-registered built-in, else 0. */
int is_builtin(ShellState *sh, const char *name);

/*
 * Run a built-in. Returns the exit code (0 = success).
 * Returns -1 if `name` is not a built-in (caller should exec external).
 */
int run_builtin(ShellState *sh, int argc, char **argv);

#endif /* BUILTINS_H */
