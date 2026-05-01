/*
 * builtins.c — built-in commands for xvsh
 *
 * Built-ins:
 *   cd, exit, export, unset, alias, unalias,
 *   echo, pwd, history, source (.), true, false,
 *   type, env
 *
 * Lua-registered builtins are also dispatched here.
 */
#include "xvsh.h"
#include "builtins.h"
#include "lua_config.h"

#include <readline/history.h>

/* ── Forward declarations ─────────────────────────────────────── */
static int builtin_cd(ShellState *sh, int argc, char **argv);
static int builtin_exit(ShellState *sh, int argc, char **argv);
static int builtin_export(ShellState *sh, int argc, char **argv);
static int builtin_unset(ShellState *sh, int argc, char **argv);
static int builtin_alias(ShellState *sh, int argc, char **argv);
static int builtin_unalias(ShellState *sh, int argc, char **argv);
static int builtin_echo(ShellState *sh, int argc, char **argv);
static int builtin_pwd(ShellState *sh, int argc, char **argv);
static int builtin_history(ShellState *sh, int argc, char **argv);
static int builtin_source(ShellState *sh, int argc, char **argv);
static int builtin_true(ShellState *sh, int argc, char **argv);
static int builtin_false(ShellState *sh, int argc, char **argv);
static int builtin_type(ShellState *sh, int argc, char **argv);
static int builtin_env(ShellState *sh, int argc, char **argv);

typedef int (*BuiltinFn)(ShellState *, int, char **);
typedef struct { const char *name; BuiltinFn fn; } Builtin;

static const Builtin BUILTINS[] = {
    { "cd",      builtin_cd      },
    { "exit",    builtin_exit    },
    { "export",  builtin_export  },
    { "unset",   builtin_unset   },
    { "alias",   builtin_alias   },
    { "unalias", builtin_unalias },
    { "echo",    builtin_echo    },
    { "pwd",     builtin_pwd     },
    { "history", builtin_history },
    { "source",  builtin_source  },
    { ".",       builtin_source  },
    { "true",    builtin_true    },
    { "false",   builtin_false   },
    { "type",    builtin_type    },
    { "env",     builtin_env     },
    { NULL,      NULL            },
};

/* ─────────────────────────────────────────────────────────────── */

int is_builtin(ShellState *sh, const char *name)
{
    for (int i = 0; BUILTINS[i].name; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return 1;
    return lua_config_is_builtin(sh, name);
}

int run_builtin(ShellState *sh, int argc, char **argv)
{
    if (argc < 1) return 0;
    for (int i = 0; BUILTINS[i].name; i++)
        if (strcmp(BUILTINS[i].name, argv[0]) == 0)
            return BUILTINS[i].fn(sh, argc, argv);
    return lua_config_run_builtin(sh, argc, argv);
}

/* ── cd ───────────────────────────────────────────────────────── */
static int builtin_cd(ShellState *sh, int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : getenv("HOME");
    if (!dir) { fprintf(stderr, "cd: HOME not set\n"); return 1; }

    if (chdir(dir) < 0) {
        fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (getcwd(sh->cwd, sizeof(sh->cwd)))
        setenv("PWD", sh->cwd, 1);
    return 0;
}

/* ── exit ─────────────────────────────────────────────────────── */
static int builtin_exit(ShellState *sh, int argc, char **argv)
{
    int code = (argc > 1) ? atoi(argv[1]) : sh->last_exit;
    shell_cleanup(sh);
    exit(code);
}

/* ── export ───────────────────────────────────────────────────── */
static int builtin_export(ShellState *sh, int argc, char **argv)
{
    (void)sh;
    if (argc == 1) {
        /* print all exported vars — use env instead */
        extern char **environ;
        for (char **e = environ; *e; e++) printf("export %s\n", *e);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char name[256];
            size_t nlen = (size_t)(eq - argv[i]);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            strncpy(name, argv[i], nlen);
            name[nlen] = '\0';
            setenv(name, eq + 1, 1);
        } else {
            /* export without value: mark for export (already in env) */
            const char *val = getenv(argv[i]);
            if (val) setenv(argv[i], val, 1);
        }
    }
    return 0;
}

/* ── unset ────────────────────────────────────────────────────── */
static int builtin_unset(ShellState *sh, int argc, char **argv)
{
    (void)sh;
    for (int i = 1; i < argc; i++) unsetenv(argv[i]);
    return 0;
}

/* ── alias ────────────────────────────────────────────────────── */
static int builtin_alias(ShellState *sh, int argc, char **argv)
{
    if (argc == 1) {
        /* print all aliases */
        for (int i = 0; i < sh->alias_count; i++)
            printf("alias %s='%s'\n", sh->aliases[i].name, sh->aliases[i].value);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = alias_lookup(sh, argv[i]);
            if (v) printf("alias %s='%s'\n", argv[i], v);
            else fprintf(stderr, "alias: %s: not found\n", argv[i]);
        } else {
            char name[256];
            size_t nlen = (size_t)(eq - argv[i]);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            strncpy(name, argv[i], nlen);
            name[nlen] = '\0';
            alias_set(sh, name, eq + 1);
        }
    }
    return 0;
}

/* ── unalias ──────────────────────────────────────────────────── */
static int builtin_unalias(ShellState *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) alias_remove(sh, argv[i]);
    return 0;
}

/* ── echo ─────────────────────────────────────────────────────── */
static int builtin_echo(ShellState *sh, int argc, char **argv)
{
    (void)sh;
    int newline = 1;
    int start   = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline) putchar('\n');
    return 0;
}

/* ── pwd ──────────────────────────────────────────────────────── */
static int builtin_pwd(ShellState *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    puts(sh->cwd);
    return 0;
}

/* ── history ──────────────────────────────────────────────────── */
static int builtin_history(ShellState *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    HIST_ENTRY **hist = history_list();
    if (!hist) return 0;
    int offset = history_base;
    for (int i = 0; hist[i]; i++)
        printf("%5d  %s\n", offset + i, hist[i]->line);
    return 0;
}

/* ── source / . ───────────────────────────────────────────────── */
static int builtin_source(ShellState *sh, int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "source: missing filename\n"); return 1; }
    return run_file(sh, argv[1]);
}

/* ── true / false ─────────────────────────────────────────────── */
static int builtin_true(ShellState *sh, int argc, char **argv)  { (void)sh;(void)argc;(void)argv; return 0; }
static int builtin_false(ShellState *sh, int argc, char **argv) { (void)sh;(void)argc;(void)argv; return 1; }

/* ── type ─────────────────────────────────────────────────────── */
static int builtin_type(ShellState *sh, int argc, char **argv)
{
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        const char *n = argv[i];
        if (alias_lookup(sh, n)) {
            printf("%s is aliased to '%s'\n", n, alias_lookup(sh, n));
        } else if (is_builtin(sh, n)) {
            printf("%s is a shell builtin\n", n);
        } else {
            /* search PATH */
            const char *path_env = getenv("PATH");
            char found = 0;
            if (path_env) {
                char pbuf[4096];
                strncpy(pbuf, path_env, sizeof(pbuf)-1);
                pbuf[sizeof(pbuf)-1] = '\0';
                for (char *dir = strtok(pbuf, ":"); dir; dir = strtok(NULL, ":")) {
                    char full[PATH_MAX];
                    snprintf(full, sizeof(full), "%s/%s", dir, n);
                    if (access(full, X_OK) == 0) {
                        printf("%s is %s\n", n, full);
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) { fprintf(stderr, "type: %s: not found\n", n); ret = 1; }
        }
    }
    return ret;
}

/* ── env ──────────────────────────────────────────────────────── */
static int builtin_env(ShellState *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    extern char **environ;
    for (char **e = environ; *e; e++) puts(*e);
    return 0;
}
