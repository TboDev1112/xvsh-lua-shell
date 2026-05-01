/*
 * completion.c — fish-style inline ghost-text predictions for xvsh
 *
 * How it works
 * ────────────
 * Every command the user runs is appended as a plain line to:
 *
 *     ~/xvsh/command.db
 *
 * While the user is typing, we hook readline's redisplay function.
 * After readline draws the prompt + current input we:
 *   1. Search the db for the most-frequent command that starts with
 *      whatever is currently in the input buffer.
 *   2. Print the *remainder* of that command in dim grey (ghost text).
 *   3. Move the cursor back left so readline still thinks it's at the
 *      end of the typed text.
 *
 * Pressing → (Right arrow) or End accepts the ghost, filling the
 * rest of the command into the input buffer.
 *
 * A simple prefix + mtime cache means we don't re-scan the db on
 * every single redisplay call — only when the typed prefix changes
 * or the db file has been modified.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <time.h>
#include <errno.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "completion.h"

/* ── Config ───────────────────────────────────────────────────── */
#define GHOST_COLOR  "\033[2;37m"   /* dim grey                  */
#define GHOST_RESET  "\033[0m"
#define MAX_GHOST    72             /* max ghost chars to display */
#define MIN_PREFIX   2              /* don't predict for 1 char   */

/* ── Paths ────────────────────────────────────────────────────── */
static char db_path[PATH_MAX];     /* ~/xvsh/command.db          */

/* ── Prediction cache ─────────────────────────────────────────── */
/*
 * We cache the last result so repeated redraws with the same
 * prefix (e.g. cursor blink, terminal resize) don't re-scan the db.
 * The cache is invalidated when the prefix changes OR the db file's
 * mtime advances (i.e. a new command was just recorded).
 */
static char  cache_prefix[4096] = {0};
static char *cache_result        = NULL;   /* heap-alloc'd or NULL */
static time_t cache_mtime        = 0;

/* ── db_find_best ─────────────────────────────────────────────────
 * Scan the db and return the highest-frequency command that starts
 * with `prefix`.  Caller must free() the result.  Returns NULL if
 * nothing matches.
 */
static char *db_find_best(const char *prefix)
{
    FILE *f = fopen(db_path, "r");
    if (!f) return NULL;

    int plen = (int)strlen(prefix);

    /* We track unique matching commands + occurrence counts inline. */
    typedef struct { char *cmd; int count; } Entry;
    Entry  *entries   = NULL;
    int     ecount    = 0;
    int     ecap      = 0;
    char    line[4096];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline / CR */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len <= plen) continue;

        /* Does this command start with our prefix? */
        if (strncmp(line, prefix, (size_t)plen) != 0) continue;

        /* Skip exact matches (nothing to ghost) */
        if (len == plen) continue;

        /* Find or create an entry */
        int found = 0;
        for (int i = 0; i < ecount; i++) {
            if (strcmp(entries[i].cmd, line) == 0) {
                entries[i].count++;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (ecount >= ecap) {
                ecap  = ecap ? ecap * 2 : 32;
                entries = realloc(entries, sizeof(Entry) * (size_t)ecap);
            }
            entries[ecount].cmd   = strdup(line);
            entries[ecount].count = 1;
            ecount++;
        }
    }
    fclose(f);

    if (ecount == 0) { free(entries); return NULL; }

    /* Pick highest count */
    int best = 0;
    for (int i = 1; i < ecount; i++)
        if (entries[i].count > entries[best].count) best = i;

    char *result = strdup(entries[best].cmd);
    for (int i = 0; i < ecount; i++) free(entries[i].cmd);
    free(entries);
    return result;
}

/* ── db_get_mtime ─────────────────────────────────────────────── */
static time_t db_get_mtime(void)
{
    struct stat st;
    if (stat(db_path, &st) < 0) return 0;
    return st.st_mtime;
}

/* ── get_completion ───────────────────────────────────────────────
 * Returns cached or freshly-computed completion for `prefix`.
 * The returned pointer is owned by the cache — do NOT free it.
 * Returns NULL if no completion exists.
 */
static const char *get_completion(const char *prefix)
{
    if (!prefix || (int)strlen(prefix) < MIN_PREFIX) return NULL;

    time_t mtime = db_get_mtime();

    /* Cache hit: same prefix and db hasn't changed */
    if (strcmp(prefix, cache_prefix) == 0 && mtime == cache_mtime)
        return cache_result; /* may be NULL (no match cached) */

    /* Cache miss: recompute */
    free(cache_result);
    cache_result = NULL;

    strncpy(cache_prefix, prefix, sizeof(cache_prefix) - 1);
    cache_prefix[sizeof(cache_prefix) - 1] = '\0';
    cache_mtime  = mtime;
    cache_result = db_find_best(prefix);
    return cache_result;
}

/* ── Ghost redisplay ──────────────────────────────────────────────
 * Called by readline instead of its own rl_redisplay().
 * We let readline draw normally, then overlay the dim ghost text
 * and move the cursor back so readline stays in sync.
 */
static void (*orig_redisplay)(void) = NULL;

static void ghost_redisplay(void)
{
    /* Normal readline draw */
    if (orig_redisplay) orig_redisplay();
    else                rl_redisplay();

    /* Only decorate when the cursor is at the end of the buffer */
    if (!rl_line_buffer || rl_end == 0 || rl_point != rl_end) return;

    const char *full = get_completion(rl_line_buffer);
    if (!full) return;

    const char *suffix = full + rl_end;
    int slen = (int)strlen(suffix);
    if (slen <= 0) return;

    /* Cap the ghost length so it never wraps past the terminal edge */
    if (slen > MAX_GHOST) slen = MAX_GHOST;

    /* Print ghost then retreat cursor with ANSI CUB (cursor back) */
    fprintf(rl_outstream, "%s%.*s%s\033[%dD",
            GHOST_COLOR, slen, suffix, GHOST_RESET, slen);
    fflush(rl_outstream);
}

/* ── accept_ghost ─────────────────────────────────────────────────
 * Bound to → (Right) and End.
 * If there is a ghost, fill it in; otherwise fall back to the
 * normal right-arrow / end-of-line behaviour.
 */
static int accept_ghost(int count, int key)
{
    (void)count;
    if (!rl_line_buffer || rl_end == 0) {
        /* Nothing typed: behave normally */
        return key == '\033' ? 0 : rl_forward_char(1, key);
    }

    const char *full = get_completion(rl_line_buffer);
    if (!full) {
        /* No suggestion: right-arrow moves forward, End goes to EOL */
        return rl_forward_char(1, key);
    }

    /* Accept the ghost: replace line buffer with the full command */
    rl_replace_line(full, 0);
    rl_point = rl_end;

    /* Invalidate cache so the next keystroke re-computes */
    cache_prefix[0] = '\0';
    free(cache_result);
    cache_result = NULL;

    return 0;
}

/* ── Public API ───────────────────────────────────────────────── */

void completion_init(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    /* Create ~/xvsh/ directory (ignore error if it already exists) */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/xvsh", home);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "xvsh: could not create %s: %s\n", dir, strerror(errno));

    snprintf(db_path, sizeof(db_path), "%s/xvsh/command.db", home);
}

void completion_setup(void)
{
    /* Hook redisplay */
    orig_redisplay      = rl_redisplay_function;
    rl_redisplay_function = ghost_redisplay;

    /* Right arrow → accept ghost (or move forward if no ghost) */
    rl_bind_keyseq("\033[C", accept_ghost);   /* VT100/xterm  */
    rl_bind_keyseq("\033OC", accept_ghost);   /* some terms   */

    /* End key → accept ghost (or go to EOL) */
    rl_bind_keyseq("\033[F", accept_ghost);   /* xterm End    */
    rl_bind_keyseq("\033[4~", accept_ghost);  /* linux cons.  */
}

void db_record(const char *cmd)
{
    if (!cmd || !*cmd || !db_path[0]) return;

    /* Trim leading/trailing whitespace before storing */
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) return;

    FILE *f = fopen(db_path, "a");
    if (!f) return;
    fprintf(f, "%s\n", cmd);
    fclose(f);

    /* Invalidate the cache so the next prediction is fresh */
    cache_prefix[0] = '\0';
    free(cache_result);
    cache_result = NULL;
    cache_mtime  = 0;
}
