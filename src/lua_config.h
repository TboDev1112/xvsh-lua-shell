/*
 * lua_config.h — Lua configuration system for xvsh
 *
 * Reads ~/.xvshrc for a list of Lua config file paths,
 * executes each in order, and exposes the `xvsh` Lua table.
 */
#ifndef LUA_CONFIG_H
#define LUA_CONFIG_H

#include "xvsh.h"

/*
 * Initialise Lua state, register the `xvsh` API table, then
 * read ~/.xvshrc and execute each listed config file.
 */
void  lua_config_load(ShellState *sh);

/*
 * Return the current prompt string.
 * If xvsh.prompt is a function, call it; if a string, return it.
 * Caller must free() the returned string.
 */
char *lua_config_get_prompt(ShellState *sh);

/* Call the pre-exec hook (if registered) with the raw command string. */
void  lua_config_call_preexec(ShellState *sh, const char *cmd);

/* Call the post-exec hook (if registered) with command and exit code. */
void  lua_config_call_postexec(ShellState *sh, const char *cmd, int exit_code);

/* Returns 1 if `name` is a Lua-registered builtin. */
int   lua_config_is_builtin(ShellState *sh, const char *name);

/*
 * Run a Lua-registered builtin.  Returns exit code, or -1 if not found.
 */
int   lua_config_run_builtin(ShellState *sh, int argc, char **argv);

#endif /* LUA_CONFIG_H */
