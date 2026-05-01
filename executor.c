/*
 * executor.c — command execution engine for xvsh
 *
 * Responsibilities:
 *   - Walk the CmdList AST
 *   - Expand words (variables, tilde, command substitution)
 *   - Expand aliases
 *   - Apply && / || short-circuit logic
 *   - Set up pipes between commands in a pipeline
 *   - Apply redirections with dup2
 *   - Fork and exec external commands
 *   - Dispatch built-ins without forking
 */
#include "xvsh.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "builtins.h"
#include "lua_config.h"

/* ── Word expansion ───────────────────────────────────────────── */

/*
 * Execute a sub-command and capture its stdout.
 * Used for $(...) command substitution.
 * Caller must free() the returned string.
 */
static char *cmd_subst(ShellState *sh, const char *cmd_str);

char *expand_word(ShellState *sh, const char *raw)
{
    char       *result = malloc(EXPAND_MAX);
    int         rlen   = 0;
    const char *p      = raw;
    int         in_sq  = 0, in_dq = 0;

    if (!result) { perror("malloc"); exit(1); }

    while (*p && rlen < EXPAND_MAX - 1) {

        /* ── Quote state tracking ─────────────────────────────── */
        if (!in_dq && *p == '\'') { p++; in_sq = !in_sq; continue; }
        if (!in_sq && *p == '"') { p++; in_dq = !in_dq; continue; }

        /* Inside single quotes: everything is literal */
        if (in_sq) { result[rlen++] = *p++; continue; }

        /* Backslash outside double quotes */
        if (!in_dq && *p == '\\' && *(p + 1)) {
            p++;
            result[rlen++] = *p++;
            continue;
        }

        /* Backslash inside double quotes: only escape \, ", $, ` */
        if (in_dq && *p == '\\' && *(p + 1) && strchr("\"\\$`", *(p + 1))) {
            p++;
            result[rlen++] = *p++;
            continue;
        }

        /* ── Tilde expansion (only at start of an unquoted word) ─ */
        if (*p == '~' && rlen == 0 && !in_dq) {
            const char *home = getenv("HOME");
            if (home) {
                int hlen = (int)strlen(home);
                if (rlen + hlen < EXPAND_MAX - 1) {
                    memcpy(result + rlen, home, (size_t)hlen);
                    rlen += hlen;
                }
            } else {
                result[rlen++] = '~';
            }
            p++;
            continue;
        }

        /* ── Variable / command substitution ─────────────────── */
        if (*p == '$') {
            p++; /* consume '$' */

            if (*p == '(') {
                /* $(...) command substitution */
                p++; /* consume '(' */
                int   depth  = 1;
                char  cmdbuf[4096];
                int   clen   = 0;

                while (*p && depth > 0) {
                    if      (*p == '(') depth++;
                    else if (*p == ')') { if (--depth == 0) { p++; break; } }
                    if (depth > 0 && clen < (int)sizeof(cmdbuf) - 1)
                        cmdbuf[clen++] = *p;
                    p++;
                }
                cmdbuf[clen] = '\0';

                char *out = cmd_subst(sh, cmdbuf);
                if (out) {
                    int olen = (int)strlen(out);
                    if (rlen + olen < EXPAND_MAX - 1) {
                        memcpy(result + rlen, out, (size_t)olen);
                        rlen += olen;
                    }
                    free(out);
                }

            } else if (*p == '?') {
                /* $? — last exit code */
                rlen += snprintf(result + rlen, (size_t)(EXPAND_MAX - rlen),
                                 "%d", sh->last_exit);
                p++;

            } else if (*p == '$') {
                /* $$ — shell PID */
                rlen += snprintf(result + rlen, (size_t)(EXPAND_MAX - rlen),
                                 "%d", (int)sh->pid);
                p++;

            } else if (*p == '{') {
                /* ${VAR} */
                p++;
                char name[256]; int nlen = 0;
                while (*p && *p != '}' && nlen < 255) name[nlen++] = *p++;
                name[nlen] = '\0';
                if (*p == '}') p++;
                const char *val = getenv(name);
                if (val) rlen += snprintf(result + rlen,
                                          (size_t)(EXPAND_MAX - rlen), "%s", val);

            } else if (isalpha((unsigned char)*p) || *p == '_') {
                /* $VAR */
                char name[256]; int nlen = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && nlen < 255)
                    name[nlen++] = *p++;
                name[nlen] = '\0';
                const char *val = getenv(name);
                if (val) rlen += snprintf(result + rlen,
                                          (size_t)(EXPAND_MAX - rlen), "%s", val);

            } else {
                /* Bare $ with no recognised expansion */
                result[rlen++] = '$';
            }
            continue;
        }

        result[rlen++] = *p++;
    }

    result[rlen] = '\0';
    return result;
}

/* ── Command substitution helper ─────────────────────────────── */
static char *cmd_subst(ShellState *sh, const char *cmd_str)
{
    int pfd[2];
    if (pipe(pfd) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return NULL; }

    if (pid == 0) {
        /* Child: redirect stdout → pipe write end */
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) exit(1);
        close(pfd[1]);
        int ret = execute_line(sh, cmd_str);
        exit(ret);
    }

    /* Parent: read from pipe */
    close(pfd[1]);
    char *buf  = malloc(EXPAND_MAX);
    int   len  = 0, n;
    while ((n = (int)read(pfd[0], buf + len, (size_t)(EXPAND_MAX - len - 1))) > 0)
        len += n;
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    buf[len] = '\0';
    /* Strip trailing newlines (POSIX behaviour) */
    while (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    return buf;
}

/* ── Apply redirections in a child process ────────────────────── */
static int apply_redirections(ShellState *sh, Redir *r)
{
    for (; r; r = r->next) {
        int fd = -1;
        switch (r->type) {
        case REDIR_IN: {
            char *f = expand_word(sh, r->file);
            fd = open(f, O_RDONLY);
            free(f);
            if (fd < 0) { perror(r->file); return -1; }
            dup2(fd, STDIN_FILENO);
            close(fd);
            break;
        }
        case REDIR_OUT: {
            char *f = expand_word(sh, r->file);
            fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            free(f);
            if (fd < 0) { perror(r->file); return -1; }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            break;
        }
        case REDIR_APP: {
            char *f = expand_word(sh, r->file);
            fd = open(f, O_WRONLY | O_CREAT | O_APPEND, 0644);
            free(f);
            if (fd < 0) { perror(r->file); return -1; }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            break;
        }
        case REDIR_ERR: {
            char *f = expand_word(sh, r->file);
            fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            free(f);
            if (fd < 0) { perror(r->file); return -1; }
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;
        }
        case REDIR_EAPP: {
            char *f = expand_word(sh, r->file);
            fd = open(f, O_WRONLY | O_CREAT | O_APPEND, 0644);
            free(f);
            if (fd < 0) { perror(r->file); return -1; }
            dup2(fd, STDERR_FILENO);
            close(fd);
            break;
        }
        case REDIR_EOUT:
            dup2(STDOUT_FILENO, STDERR_FILENO);
            break;
        }
    }
    return 0;
}

/* ── Build expanded argv[] for a simple command ─────────────────
 * Returns heap-allocated NULL-terminated array; caller frees each
 * element and the array itself.
 */
static char **build_argv(ShellState *sh, SimpleCmd *sc)
{
    char **ev = malloc(sizeof(char *) * (size_t)(sc->argc + 1));
    for (int i = 0; i < sc->argc; i++)
        ev[i] = expand_word(sh, sc->argv[i]);
    ev[sc->argc] = NULL;
    return ev;
}

static void free_argv(char **ev)
{
    if (!ev) return;
    for (int i = 0; ev[i]; i++) free(ev[i]);
    free(ev);
}

/* ── Execute a simple command ─────────────────────────────────── */
int exec_simple(ShellState *sh, SimpleCmd *sc)
{
    if (!sc || sc->argc == 0) return 0;

    char **ev = build_argv(sh, sc);
    if (!ev[0] || ev[0][0] == '\0') { free_argv(ev); return 0; }

    /* ── Alias expansion ─────────────────────────────────────── */
    {
        const char *aval = alias_lookup(sh, ev[0]);
        if (aval) {
            /* Build expanded command: alias_value + remaining args */
            size_t alen = strlen(aval);
            size_t extra = 0;
            for (int i = 1; ev[i]; i++) extra += strlen(ev[i]) + 1;
            char *expanded = malloc(alen + extra + 2);
            strcpy(expanded, aval);
            for (int i = 1; ev[i]; i++) {
                strcat(expanded, " ");
                strcat(expanded, ev[i]);
            }
            free_argv(ev);
            int ret = execute_line(sh, expanded);
            free(expanded);
            return ret;
        }
    }

    /* ── Built-in ────────────────────────────────────────────── */
    if (is_builtin(sh, ev[0])) {
        int argc = 0;
        while (ev[argc]) argc++;
        /* Built-ins run in-process but still need redirections applied.
         * For simplicity we fork only if there are redirections. */
        if (sc->redirs) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); free_argv(ev); return 1; }
            if (pid == 0) {
                if (apply_redirections(sh, sc->redirs) < 0) exit(1);
                exit(run_builtin(sh, argc, ev));
            }
            free_argv(ev);
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        int ret = run_builtin(sh, argc, ev);
        free_argv(ev);
        return ret;
    }

    /* ── External command ────────────────────────────────────── */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); free_argv(ev); return 1; }

    if (pid == 0) {
        /* Reset signals in child */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        if (apply_redirections(sh, sc->redirs) < 0) exit(1);
        execvp(ev[0], ev);
        fprintf(stderr, "xvsh: %s: %s\n", ev[0], strerror(errno));
        exit(127);
    }

    free_argv(ev);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ── Execute a pipeline ───────────────────────────────────────── */
int exec_pipeline(ShellState *sh, Pipeline *pl)
{
    if (pl->count == 1 && !pl->background)
        return exec_simple(sh, pl->cmds[0]);

    int  n    = pl->count;
    int *pfds = NULL; /* pipe fd pairs: pfds[i*2], pfds[i*2+1] */
    pid_t *pids = calloc((size_t)n, sizeof(pid_t));

    if (n > 1) {
        pfds = malloc(sizeof(int) * 2 * (size_t)(n - 1));
        for (int i = 0; i < n - 1; i++) {
            if (pipe(&pfds[i * 2]) < 0) {
                perror("pipe"); free(pfds); free(pids); return 1;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); break; }

        if (pids[i] == 0) {
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            /* Connect stdin from previous pipe */
            if (i > 0) {
                dup2(pfds[(i - 1) * 2], STDIN_FILENO);
            }
            /* Connect stdout to next pipe */
            if (i < n - 1) {
                dup2(pfds[i * 2 + 1], STDOUT_FILENO);
            }

            /* Close all pipe fds in child */
            if (pfds) for (int j = 0; j < (n - 1) * 2; j++) close(pfds[j]);

            if (apply_redirections(sh, pl->cmds[i]->redirs) < 0) exit(1);

            char **ev = build_argv(sh, pl->cmds[i]);
            if (!ev || !ev[0]) exit(0);

            if (is_builtin(sh, ev[0])) {
                int argc = 0; while (ev[argc]) argc++;
                exit(run_builtin(sh, argc, ev));
            }
            execvp(ev[0], ev);
            fprintf(stderr, "xvsh: %s: %s\n", ev[0], strerror(errno));
            exit(127);
        }
    }

    /* Parent: close all pipe fds */
    if (pfds) {
        for (int i = 0; i < (n - 1) * 2; i++) close(pfds[i]);
        free(pfds);
    }

    int last_status = 0;
    if (pl->background) {
        /* Don't wait — print PID of last process */
        printf("[bg] %d\n", (int)pids[n - 1]);
    } else {
        for (int i = 0; i < n; i++) {
            int status = 0;
            if (pids[i] > 0) waitpid(pids[i], &status, 0);
            if (i == n - 1) {
                if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
                else last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
        }
    }

    free(pids);
    return last_status;
}

/* ── Execute a command list ───────────────────────────────────── */
int exec_cmdlist(ShellState *sh, CmdList *cl)
{
    int ret = 0;
    for (int i = 0; i < cl->count; i++) {
        /* Apply && / || short-circuit from previous separator */
        if (i > 0) {
            Sep prev_sep = cl->seps[i - 1];
            if (prev_sep == SEP_AND && ret != 0) continue;
            if (prev_sep == SEP_OR  && ret == 0) continue;
        }
        ret = exec_pipeline(sh, cl->pipelines[i]);
    }
    return ret;
}
