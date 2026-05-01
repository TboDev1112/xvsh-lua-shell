/*
 * parser.h — AST types for xvsh
 *
 * Grammar (simplified):
 *
 *   cmdlist  := pipeline (sep pipeline)*
 *   sep      := ';' | '&&' | '||'
 *   pipeline := simple_cmd ('|' simple_cmd)*  ['&']
 *   simple_cmd := WORD+ redirect*
 *   redirect := '<' WORD | '>' WORD | '>>' WORD
 *             | '2>' WORD | '2>>' WORD | '2>&1'
 */
#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

/* ── Redirection ──────────────────────────────────────────────── */
typedef enum {
    REDIR_IN,    /* <   stdin  from file  */
    REDIR_OUT,   /* >   stdout to file    */
    REDIR_APP,   /* >>  stdout append     */
    REDIR_ERR,   /* 2>  stderr to file    */
    REDIR_EAPP,  /* 2>> stderr append     */
    REDIR_EOUT,  /* 2>&1 stderr → stdout  */
} RedirType;

typedef struct Redir {
    RedirType    type;
    char        *file;        /* NULL for REDIR_EOUT */
    struct Redir *next;
} Redir;

/* ── Simple command ───────────────────────────────────────────── */
typedef struct {
    char   **argv;  /* NULL-terminated, raw (pre-expansion) */
    int      argc;
    Redir   *redirs;
} SimpleCmd;

/* ── Pipeline ─────────────────────────────────────────────────── */
typedef struct {
    SimpleCmd **cmds;
    int         count;
    int         background; /* ends with & */
} Pipeline;

/* ── List separator ───────────────────────────────────────────── */
typedef enum { SEP_SEMI, SEP_AND, SEP_OR } Sep;

/* ── Command list ─────────────────────────────────────────────── */
typedef struct {
    Pipeline **pipelines;
    Sep       *seps;   /* seps[i] is separator AFTER pipelines[i]; last unused */
    int        count;
    int        cap;
} CmdList;

/* Parse a TokenList into a CmdList (caller frees with cmdlist_free). */
CmdList  *parse(TokenList *tl);
void      cmdlist_free(CmdList *cl);

#endif /* PARSER_H */
