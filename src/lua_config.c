/*
 * lua_config.c — Lua configuration system for xvsh
 *
 * Exposes the following Lua API under the global table `xvsh`:
 *
 *   xvsh.version        string   — shell version
 *   xvsh.prompt         string|function — prompt (default "$ ")
 *   xvsh.alias(n, v)    function — define alias  n → v
 *   xvsh.unalias(n)     function — remove alias n
 *   xvsh.setenv(k, v)   function — set environment variable
 *   xvsh.getenv(k)      function — get environment variable
 *   xvsh.cwd()          function — return current working directory
 *   xvsh.exit_code()    function — return last exit code ($?)
 *   xvsh.on_preexec(f)  function — register pre-command hook
 *   xvsh.on_postexec(f) function — register post-command hook
 *   xvsh.register_builtin(n,f) function — register a Lua builtin
 *
 * ~/.xvshrc format:
 *   /absolute/path/to/config.lua   # one path per line; # = comment
 */
#include "xvsh.h"
#include "lua_config.h"

/* Registry keys */
#define REG_PREEXEC   "xvsh_preexec"
#define REG_POSTEXEC  "xvsh_postexec"
#define REG_BUILTINS  "xvsh_builtins"
#define REG_SHELL     "xvsh_shell_ptr"

/* ── Retrieve the ShellState stored in the Lua registry ───────── */
static ShellState *get_shell(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_SHELL);
    ShellState *sh = (ShellState *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return sh;
}

/* ── xvsh.alias(name, value) ──────────────────────────────────── */
static int l_alias(lua_State *L)
{
    const char *name  = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);
    alias_set(get_shell(L), name, value);
    return 0;
}

/* ── xvsh.unalias(name) ───────────────────────────────────────── */
static int l_unalias(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    alias_remove(get_shell(L), name);
    return 0;
}

/* ── xvsh.setenv(key, value) ──────────────────────────────────── */
static int l_setenv(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *val = luaL_checkstring(L, 2);
    setenv(key, val, 1);
    return 0;
}

/* ── xvsh.getenv(key) → string|nil ───────────────────────────── */
static int l_getenv(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *val = getenv(key);
    if (val) lua_pushstring(L, val);
    else     lua_pushnil(L);
    return 1;
}

/* ── xvsh.cwd() → string ─────────────────────────────────────── */
static int l_cwd(lua_State *L)
{
    lua_pushstring(L, get_shell(L)->cwd);
    return 1;
}

/* ── xvsh.exit_code() → integer ──────────────────────────────── */
static int l_exit_code(lua_State *L)
{
    lua_pushinteger(L, get_shell(L)->last_exit);
    return 1;
}

/* ── xvsh.on_preexec(fn) ──────────────────────────────────────── */
static int l_on_preexec(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_PREEXEC);
    get_shell(L)->has_preexec = 1;
    return 0;
}

/* ── xvsh.on_postexec(fn) ─────────────────────────────────────── */
static int l_on_postexec(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_POSTEXEC);
    get_shell(L)->has_postexec = 1;
    return 0;
}

/* ── xvsh.register_builtin(name, fn) ─────────────────────────── */
static int l_register_builtin(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* xvsh_builtins[name] = fn */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_BUILTINS);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

/* ── Register the `xvsh` global table ────────────────────────── */
static void register_xvsh_table(lua_State *L, ShellState *sh)
{
    lua_newtable(L);

    /* Version */
    lua_pushstring(L, XVSH_VERSION);
    lua_setfield(L, -2, "version");

    /* Default prompt */
    lua_pushstring(L, "$ ");
    lua_setfield(L, -2, "prompt");

    /* Functions */
    lua_pushcfunction(L, l_alias);             lua_setfield(L, -2, "alias");
    lua_pushcfunction(L, l_unalias);           lua_setfield(L, -2, "unalias");
    lua_pushcfunction(L, l_setenv);            lua_setfield(L, -2, "setenv");
    lua_pushcfunction(L, l_getenv);            lua_setfield(L, -2, "getenv");
    lua_pushcfunction(L, l_cwd);               lua_setfield(L, -2, "cwd");
    lua_pushcfunction(L, l_exit_code);         lua_setfield(L, -2, "exit_code");
    lua_pushcfunction(L, l_on_preexec);        lua_setfield(L, -2, "on_preexec");
    lua_pushcfunction(L, l_on_postexec);       lua_setfield(L, -2, "on_postexec");
    lua_pushcfunction(L, l_register_builtin);  lua_setfield(L, -2, "register_builtin");

    lua_setglobal(L, "xvsh");

    /* Store shell pointer in registry */
    lua_pushlightuserdata(L, (void *)sh);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_SHELL);

    /* Initialise builtins registry table */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_BUILTINS);
}

/* ── Execute a single Lua config file ────────────────────────── */
static void exec_lua_file(lua_State *L, const char *path)
{
    if (luaL_loadfile(L, path) != LUA_OK) {
        fprintf(stderr, "xvsh: lua load error (%s): %s\n",
                path, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "xvsh: lua error (%s): %s\n",
                path, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

/* ── Read ~/.xvshrc and execute each listed config file ──────── */
static void load_xvshrc(lua_State *L)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char rc_path[PATH_MAX];
    snprintf(rc_path, sizeof(rc_path), "%s%s", home, XVSHRC_FILENAME);

    FILE *fp = fopen(rc_path, "r");
    if (!fp) return; /* ~/.xvshrc is optional */

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        /* Trim trailing whitespace */
        char path[PATH_MAX];
        strncpy(path, p, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        size_t plen = strlen(path);
        while (plen > 0 && (path[plen-1] == ' ' || path[plen-1] == '\t'))
            path[--plen] = '\0';

        exec_lua_file(L, path);
    }

    fclose(fp);
}

/* ── Public API ───────────────────────────────────────────────── */

void lua_config_load(ShellState *sh)
{
    sh->L = luaL_newstate();
    if (!sh->L) { fprintf(stderr, "xvsh: failed to create Lua state\n"); return; }
    luaL_openlibs(sh->L);
    register_xvsh_table(sh->L, sh);
    load_xvshrc(sh->L);
}

char *lua_config_get_prompt(ShellState *sh)
{
    lua_State *L = sh->L;
    if (!L) return strdup("$ ");

    lua_getglobal(L, "xvsh");
    lua_getfield(L, -1, "prompt");

    char *result = NULL;

    if (lua_isfunction(L, -1)) {
        /* Call prompt function with no args, expect one string return */
        if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
            const char *s = lua_tostring(L, -1);
            result = strdup(s ? s : "$ ");
            lua_pop(L, 1);
        } else {
            fprintf(stderr, "xvsh: prompt error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            result = strdup("$ ");
        }
    } else if (lua_isstring(L, -1)) {
        result = strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        result = strdup("$ ");
    }

    lua_pop(L, 1); /* pop xvsh table */
    return result;
}

void lua_config_call_preexec(ShellState *sh, const char *cmd)
{
    if (!sh->L || !sh->has_preexec) return;
    lua_getfield(sh->L, LUA_REGISTRYINDEX, REG_PREEXEC);
    if (!lua_isfunction(sh->L, -1)) { lua_pop(sh->L, 1); return; }
    lua_pushstring(sh->L, cmd);
    if (lua_pcall(sh->L, 1, 0, 0) != LUA_OK) {
        fprintf(stderr, "xvsh: preexec hook error: %s\n",
                lua_tostring(sh->L, -1));
        lua_pop(sh->L, 1);
    }
}

void lua_config_call_postexec(ShellState *sh, const char *cmd, int exit_code)
{
    if (!sh->L || !sh->has_postexec) return;
    lua_getfield(sh->L, LUA_REGISTRYINDEX, REG_POSTEXEC);
    if (!lua_isfunction(sh->L, -1)) { lua_pop(sh->L, 1); return; }
    lua_pushstring(sh->L, cmd);
    lua_pushinteger(sh->L, exit_code);
    if (lua_pcall(sh->L, 2, 0, 0) != LUA_OK) {
        fprintf(stderr, "xvsh: postexec hook error: %s\n",
                lua_tostring(sh->L, -1));
        lua_pop(sh->L, 1);
    }
}

int lua_config_is_builtin(ShellState *sh, const char *name)
{
    if (!sh->L) return 0;
    lua_getfield(sh->L, LUA_REGISTRYINDEX, REG_BUILTINS);
    lua_getfield(sh->L, -1, name);
    int found = lua_isfunction(sh->L, -1);
    lua_pop(sh->L, 2);
    return found;
}

int lua_config_run_builtin(ShellState *sh, int argc, char **argv)
{
    if (!sh->L || argc < 1) return -1;
    lua_State *L = sh->L;

    lua_getfield(L, LUA_REGISTRYINDEX, REG_BUILTINS);
    lua_getfield(L, -1, argv[0]);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return -1;
    }

    /* Build args table: args[0]=cmd, args[1]=first arg, ... */
    lua_newtable(L);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fprintf(stderr, "xvsh: builtin '%s' error: %s\n",
                argv[0], lua_tostring(L, -1));
        lua_pop(L, 2);
        return 1;
    }

    int ret = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 2); /* result + builtins table */
    return ret;
}
