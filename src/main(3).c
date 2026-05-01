/*
 * main.c — entry point for xvsh
 *
 * Handles:
 *   - Shell initialisation
 *   - Lua config loading
 *   - Signal setup
 *   - Interactive REPL (with readline)
 *   - Script file execution
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "xvsh.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "builtins.h"
#include "lua_config.h"
#include "completion.h"

/* ── Global shell state ───────────────────────────────────────── */
ShellState shell;

/* ── Signal handlers ──────────────────────────────────────────── */
static volatile sig_atomic_t got_sigint = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    got_sigint = 1;
    /* write() is async-signal-safe; readline functions are not */
    (void)write(STDOUT_FILENO, "\n", 1);
}

static void handle_sigchld(int sig)
{
    (void)sig;
    /* Reap zombie background children */
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

/* ── Shell state helpers ──────────────────────────────────────── */
void shell_init(ShellState *sh, const char *argv0)
{
    memset(sh, 0, sizeof(*sh));
    strncpy(sh->name, argv0, sizeof(sh->name) - 1);
    if (!getcwd(sh->cwd, sizeof(sh->cwd)))
        strncpy(sh->cwd, "/", sizeof(sh->cwd) - 1);
    sh->pid         = getpid();
    sh->last_exit   = 0;
    sh->interactive = isatty(STDIN_FILENO);
}

void shell_cleanup(ShellState *sh)
{
    /* Free aliases */
    for (int i = 0; i < sh->alias_count; i++) {
        free(sh->aliases[i].name);
        free(sh->aliases[i].value);
    }
    sh->alias_count = 0;

    /* Close Lua */
    if (sh->L) {
        lua_close(sh->L);
        sh->L = NULL;
    }
}

/* ── Alias helpers ────────────────────────────────────────────── */
const char *alias_lookup(ShellState *sh, const char *name)
{
    for (int i = 0; i < sh->alias_count; i++)
        if (strcmp(sh->aliases[i].name, name) == 0)
            return sh->aliases[i].value;
    return NULL;
}

void alias_set(ShellState *sh, const char *name, const char *value)
{
    for (int i = 0; i < sh->alias_count; i++) {
        if (strcmp(sh->aliases[i].name, name) == 0) {
            free(sh->aliases[i].value);
            sh->aliases[i].value = strdup(value);
            return;
        }
    }
    if (sh->alias_count >= MAX_ALIASES) {
        fprintf(stderr, "xvsh: alias table full\n");
        return;
    }
    sh->aliases[sh->alias_count].name  = strdup(name);
    sh->aliases[sh->alias_count].value = strdup(value);
    sh->alias_count++;
}

void alias_remove(ShellState *sh, const char *name)
{
    for (int i = 0; i < sh->alias_count; i++) {
        if (strcmp(sh->aliases[i].name, name) == 0) {
            free(sh->aliases[i].name);
            free(sh->aliases[i].value);
            /* Shift remaining entries down */
            memmove(&sh->aliases[i], &sh->aliases[i + 1],
                    sizeof(Alias) * (size_t)(sh->alias_count - i - 1));
            sh->alias_count--;
            return;
        }
    }
}

/* ── Parse and execute a single line of input ─────────────────── */
int execute_line(ShellState *sh, const char *line)
{
    if (!line || !*line) return 0;

    /* Call pre-exec hook */
    lua_config_call_preexec(sh, line);

    TokenList *tl = lex(line);
    CmdList   *cl = parse(tl);
    int        ret = 0;

    if (cl && cl->count > 0)
        ret = exec_cmdlist(sh, cl);

    cmdlist_free(cl);
    tokenlist_free(tl);

    sh->last_exit = ret;

    /* Call post-exec hook */
    lua_config_call_postexec(sh, line, ret);

    return ret;
}

/* ── Execute a script file (source / .) ───────────────────────── */
int run_file(ShellState *sh, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "xvsh: %s: %s\n", path, strerror(errno));
        return 1;
    }

    char   line[65536];
    int    ret    = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        ret = execute_line(sh, line);
    }

    fclose(fp);
    return ret;
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    shell_init(&shell, argv[0]);

    /* Load Lua config (reads ~/.xvshrc and runs each listed .lua file) */
    lua_config_load(&shell);

    /* Signal setup */
    signal(SIGINT,  handle_sigint);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, handle_sigchld);

    /* If a script is provided as argument, run it non-interactively */
    if (argc > 1) {
        int ret = run_file(&shell, argv[1]);
        shell_cleanup(&shell);
        return ret;
    }

    /* ── Interactive REPL ───────────────────────────────────── */
    if (shell.interactive) {
        rl_initialize();
        using_history();

        /* Set up ghost-text completion (reads/writes ~/xvsh/command.db) */
        completion_init();
        completion_setup();

        /* Optional: load persistent history */
        const char *home = getenv("HOME");
        char hist_file[PATH_MAX] = {0};
        if (home) {
            snprintf(hist_file, sizeof(hist_file), "%s/.xvsh_history", home);
            read_history(hist_file);
        }

        char *line;
        while (1) {
            char *prompt = lua_config_get_prompt(&shell);
            line = readline(prompt);
            free(prompt);

            if (!line) {
                if (got_sigint) {
                    /* Ctrl-C mid-prompt: clear and re-prompt */
                    got_sigint = 0;
                    rl_on_new_line();
                    rl_replace_line("", 0);
                    continue;
                }
                /* Real EOF (Ctrl-D) */
                if (shell.interactive) printf("exit\n");
                break;
            }

            if (*line) {
                add_history(line);
                db_record(line);       /* persist to ~/xvsh/command.db */
                execute_line(&shell, line);
            }
            free(line);

            /* If Ctrl-C fired while we were executing, clear the flag */
            if (got_sigint) {
                got_sigint = 0;
                rl_on_new_line();
                rl_replace_line("", 0);
            }
        }

        /* Save history */
        if (hist_file[0]) {
            stifle_history(1000);
            write_history(hist_file);
        }

    } else {
        /* Non-interactive: read from stdin */
        char line[65536];
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            execute_line(&shell, line);
        }
    }

    shell_cleanup(&shell);
    return shell.last_exit;
}
