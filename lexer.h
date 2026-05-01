/*
 * lexer.h — tokeniser for xvsh
 *
 * Produces a flat list of Token values from a shell input string.
 * Quote context is preserved in the token's raw text so that
 * expand_word() can later handle '$' expansion correctly.
 */
#ifndef LEXER_H
#define LEXER_H

/* ── Token types ──────────────────────────────────────────────── */
typedef enum {
    TOK_WORD,       /* any word (possibly quoted)       */
    TOK_PIPE,       /* |                                */
    TOK_AND,        /* &&                               */
    TOK_OR,         /* ||                               */
    TOK_SEMI,       /* ;                                */
    TOK_BG,         /* &                                */
    TOK_REDIR_IN,   /* <                                */
    TOK_REDIR_OUT,  /* >                                */
    TOK_REDIR_APP,  /* >>                               */
    TOK_REDIR_ERR,  /* 2>                               */
    TOK_REDIR_EAPP, /* 2>>                              */
    TOK_REDIR_EOUT, /* 2>&1                             */
    TOK_NEWLINE,
    TOK_EOF,
} TokenType;

typedef struct {
    TokenType  type;
    char      *value; /* raw text for TOK_WORD (heap-alloc'd); NULL otherwise */
} Token;

typedef struct {
    Token *tokens;
    int    count;
    int    cap;
} TokenList;

/* Tokenise `input` and return a new TokenList (caller frees with tokenlist_free). */
TokenList *lex(const char *input);
void       tokenlist_free(TokenList *tl);

#endif /* LEXER_H */
