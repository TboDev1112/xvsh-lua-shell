/*
 * parser.c — recursive descent parser for xvsh
 *
 * Converts a TokenList into a CmdList AST that the executor can walk.
 */
#include "xvsh.h"
#include "parser.h"

/* ── Parser cursor ────────────────────────────────────────────── */
typedef struct {
    TokenList *tl;
    int        pos;
} Parser;

static Token *peek(Parser *p)         { return &p->tl->tokens[p->pos]; }
static Token *advance(Parser *p)      { return &p->tl->tokens[p->pos++]; }
static int    check(Parser *p, TokenType t) { return peek(p)->type == t; }
static int    match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return 1; }
    return 0;
}

/* ── Memory helpers ───────────────────────────────────────────── */
static Redir *redir_new(RedirType type, const char *file)
{
    Redir *r = calloc(1, sizeof(Redir));
    r->type = type;
    r->file = file ? strdup(file) : NULL;
    return r;
}

static void redir_free(Redir *r)
{
    while (r) { Redir *next = r->next; free(r->file); free(r); r = next; }
}

static void simplecmd_free(SimpleCmd *sc)
{
    if (!sc) return;
    for (int i = 0; i < sc->argc; i++) free(sc->argv[i]);
    free(sc->argv);
    redir_free(sc->redirs);
    free(sc);
}

static void pipeline_free(Pipeline *pl)
{
    if (!pl) return;
    for (int i = 0; i < pl->count; i++) simplecmd_free(pl->cmds[i]);
    free(pl->cmds);
    free(pl);
}

/* ── Parse a redirection ──────────────────────────────────────── */
static Redir *parse_redir(Parser *p)
{
    TokenType tt = peek(p)->type;
    switch (tt) {
    case TOK_REDIR_IN:
    case TOK_REDIR_OUT:
    case TOK_REDIR_APP:
    case TOK_REDIR_ERR:
    case TOK_REDIR_EAPP: {
        advance(p);
        if (!check(p, TOK_WORD)) {
            fprintf(stderr, "xvsh: expected filename after redirection\n");
            return NULL;
        }
        const char *file = advance(p)->value;
        RedirType rt = (tt == TOK_REDIR_IN)   ? REDIR_IN  :
                       (tt == TOK_REDIR_OUT)  ? REDIR_OUT :
                       (tt == TOK_REDIR_APP)  ? REDIR_APP :
                       (tt == TOK_REDIR_ERR)  ? REDIR_ERR :
                                                REDIR_EAPP;
        return redir_new(rt, file);
    }
    case TOK_REDIR_EOUT:
        advance(p);
        return redir_new(REDIR_EOUT, NULL);
    default:
        return NULL;
    }
}

/* ── Parse a simple command ───────────────────────────────────── */
static SimpleCmd *parse_simple_cmd(Parser *p)
{
    SimpleCmd *sc  = calloc(1, sizeof(SimpleCmd));
    int        cap = 8;
    sc->argv = malloc(sizeof(char *) * (size_t)cap);

    Redir  *redir_head = NULL;
    Redir **redir_tail = &redir_head;

    while (!check(p, TOK_EOF) && !check(p, TOK_NEWLINE)
        && !check(p, TOK_PIPE) && !check(p, TOK_SEMI)
        && !check(p, TOK_AND)  && !check(p, TOK_OR)
        && !check(p, TOK_BG))
    {
        /* Try a redirection first */
        Redir *r = parse_redir(p);
        if (r) {
            *redir_tail = r;
            redir_tail  = &r->next;
            continue;
        }

        /* Otherwise a word */
        if (!check(p, TOK_WORD)) break;
        if (sc->argc >= cap - 1) {
            cap *= 2;
            sc->argv = realloc(sc->argv, sizeof(char *) * (size_t)cap);
        }
        sc->argv[sc->argc++] = strdup(advance(p)->value);
    }

    sc->argv[sc->argc] = NULL; /* NULL-terminate */
    sc->redirs = redir_head;

    if (sc->argc == 0 && sc->redirs == NULL) {
        /* empty — discard */
        simplecmd_free(sc);
        return NULL;
    }
    return sc;
}

/* ── Parse a pipeline ─────────────────────────────────────────── */
static Pipeline *parse_pipeline(Parser *p)
{
    Pipeline *pl  = calloc(1, sizeof(Pipeline));
    int       cap = 4;
    pl->cmds = malloc(sizeof(SimpleCmd *) * (size_t)cap);

    SimpleCmd *sc = parse_simple_cmd(p);
    if (!sc) { free(pl->cmds); free(pl); return NULL; }

    pl->cmds[pl->count++] = sc;

    while (check(p, TOK_PIPE)) {
        advance(p); /* consume | */
        /* Skip newlines after | */
        while (match(p, TOK_NEWLINE)) {}
        sc = parse_simple_cmd(p);
        if (!sc) break;
        if (pl->count >= cap) { cap *= 2; pl->cmds = realloc(pl->cmds, sizeof(SimpleCmd *) * (size_t)cap); }
        pl->cmds[pl->count++] = sc;
    }

    /* Background flag */
    if (check(p, TOK_BG)) {
        advance(p);
        pl->background = 1;
    }

    return pl;
}

/* ── Parse a full command list ────────────────────────────────── */
CmdList *parse(TokenList *tl)
{
    Parser   par = { tl, 0 };
    Parser  *p   = &par;
    CmdList *cl  = calloc(1, sizeof(CmdList));
    cl->cap = 8;
    cl->pipelines = malloc(sizeof(Pipeline *) * (size_t)cl->cap);
    cl->seps      = malloc(sizeof(Sep)        * (size_t)cl->cap);

    /* Skip leading newlines */
    while (match(p, TOK_NEWLINE)) {}

    while (!check(p, TOK_EOF)) {
        Pipeline *pl = parse_pipeline(p);
        if (!pl) { match(p, TOK_NEWLINE); continue; }

        if (cl->count >= cl->cap) {
            cl->cap *= 2;
            cl->pipelines = realloc(cl->pipelines, sizeof(Pipeline *) * (size_t)cl->cap);
            cl->seps      = realloc(cl->seps,      sizeof(Sep)        * (size_t)cl->cap);
        }
        cl->pipelines[cl->count] = pl;

        /* Separator after pipeline */
        if (check(p, TOK_AND)) {
            cl->seps[cl->count++] = SEP_AND;
            advance(p);
        } else if (check(p, TOK_OR)) {
            cl->seps[cl->count++] = SEP_OR;
            advance(p);
        } else {
            cl->seps[cl->count++] = SEP_SEMI;
            match(p, TOK_SEMI);
            match(p, TOK_NEWLINE);
        }

        while (match(p, TOK_NEWLINE)) {}
    }

    return cl;
}

/* ── Cleanup ──────────────────────────────────────────────────── */
void cmdlist_free(CmdList *cl)
{
    if (!cl) return;
    for (int i = 0; i < cl->count; i++) pipeline_free(cl->pipelines[i]);
    free(cl->pipelines);
    free(cl->seps);
    free(cl);
}
