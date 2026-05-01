/*
 * lexer.c — tokeniser for xvsh
 *
 * Walks the input string once, producing a TokenList.
 * Quoting is tracked for word-boundary detection (spaces inside quotes
 * do not split words), and the raw text including quote characters is
 * stored so expand_word() can handle variable expansion correctly.
 */
#include "xvsh.h"
#include "lexer.h"

/* ── TokenList growth ─────────────────────────────────────────── */
static void tl_push(TokenList *tl, TokenType type, const char *value)
{
    if (tl->count >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 16;
        tl->tokens = realloc(tl->tokens, sizeof(Token) * (size_t)tl->cap);
        if (!tl->tokens) { perror("realloc"); exit(1); }
    }
    Token *t = &tl->tokens[tl->count++];
    t->type  = type;
    t->value = value ? strdup(value) : NULL;
}

/* ── Helper: is char a metacharacter that ends an unquoted word? ── */
static int is_meta(char c)
{
    return c == ' ' || c == '\t' || c == '\n'
        || c == '|' || c == '&'  || c == ';'
        || c == '<' || c == '>'  || c == '\0';
}

/* ── Main lexer ───────────────────────────────────────────────── */
TokenList *lex(const char *input)
{
    TokenList *tl = calloc(1, sizeof(TokenList));
    if (!tl) { perror("calloc"); exit(1); }

    const char *p = input;

    while (*p) {
        /* Skip horizontal whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* Comment: rest of line is discarded */
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Newline */
        if (*p == '\n') {
            tl_push(tl, TOK_NEWLINE, NULL);
            p++;
            continue;
        }

        /* Semicolon */
        if (*p == ';') {
            tl_push(tl, TOK_SEMI, NULL);
            p++;
            continue;
        }

        /* & or && */
        if (*p == '&') {
            if (*(p + 1) == '&') { tl_push(tl, TOK_AND, NULL); p += 2; }
            else                  { tl_push(tl, TOK_BG,  NULL); p++;   }
            continue;
        }

        /* | or || */
        if (*p == '|') {
            if (*(p + 1) == '|') { tl_push(tl, TOK_OR,   NULL); p += 2; }
            else                  { tl_push(tl, TOK_PIPE, NULL); p++;   }
            continue;
        }

        /* < */
        if (*p == '<') {
            tl_push(tl, TOK_REDIR_IN, NULL);
            p++;
            continue;
        }

        /* > or >> */
        if (*p == '>') {
            if (*(p + 1) == '>') { tl_push(tl, TOK_REDIR_APP, NULL); p += 2; }
            else                  { tl_push(tl, TOK_REDIR_OUT, NULL); p++;   }
            continue;
        }

        /* 2>, 2>>, 2>&1  — only when followed by '>' to avoid eating "2" as a word */
        if (*p == '2' && *(p + 1) == '>') {
            if (*(p + 2) == '>' ) {
                tl_push(tl, TOK_REDIR_EAPP, NULL); p += 3;
            } else if (*(p + 2) == '&' && *(p + 3) == '1') {
                tl_push(tl, TOK_REDIR_EOUT, NULL); p += 4;
            } else {
                tl_push(tl, TOK_REDIR_ERR, NULL); p += 2;
            }
            continue;
        }

        /* ── Word collection ──────────────────────────────────── */
        /*
         * We accumulate characters into `buf`, tracking quote state
         * so spaces inside quotes don't terminate the word.
         * The raw text (including quote characters) is stored in the
         * token so expand_word() can handle quoting properly.
         */
        {
            char buf[EXPAND_MAX];
            int  blen     = 0;
            int  in_sq    = 0; /* inside single quotes */
            int  in_dq    = 0; /* inside double quotes */

            while (*p) {
                /* ── Single-quote toggle ──────────────────────── */
                if (!in_dq && *p == '\'') {
                    if (blen < EXPAND_MAX - 1) buf[blen++] = *p;
                    p++;
                    in_sq = !in_sq;
                    continue;
                }

                /* ── Double-quote toggle ──────────────────────── */
                if (!in_sq && *p == '"') {
                    if (blen < EXPAND_MAX - 1) buf[blen++] = *p;
                    p++;
                    in_dq = !in_dq;
                    continue;
                }

                /* ── Inside single quotes: everything literal ──── */
                if (in_sq) {
                    if (blen < EXPAND_MAX - 1) buf[blen++] = *p++;
                    continue;
                }

                /* ── Backslash outside quotes: escape next char ── */
                if (!in_dq && *p == '\\' && *(p + 1)) {
                    if (blen < EXPAND_MAX - 2) {
                        buf[blen++] = *p++;
                        buf[blen++] = *p++;
                    } else p += 2;
                    continue;
                }

                /* ── Inside double quotes ─────────────────────── */
                if (in_dq) {
                    /* \", \\, \$, \` — only these backslash escapes */
                    if (*p == '\\' && *(p + 1) && strchr("\"\\$`", *(p + 1))) {
                        if (blen < EXPAND_MAX - 2) { buf[blen++] = *p++; buf[blen++] = *p++; }
                        else p += 2;
                    } else {
                        if (blen < EXPAND_MAX - 1) buf[blen++] = *p++;
                        else p++;
                    }
                    continue;
                }

                /* $(...) command substitution — group entire span as one token */
                if (*p == '$' && *(p + 1) == '(') {
                    int depth = 1;
                    if (blen < EXPAND_MAX - 2) { buf[blen++] = *p++; buf[blen++] = *p++; }
                    else { p += 2; }
                    while (*p && depth > 0) {
                        if      (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        if (depth >= 0 && blen < EXPAND_MAX - 1) buf[blen++] = *p;
                        p++;
                    }
                    continue;
                }

                /* ── Outside all quotes: stop at metacharacters ── */
                /* Check for 2> which must become its own token */
                if (*p == '2' && *(p + 1) == '>') break;
                if (is_meta(*p)) break;

                if (blen < EXPAND_MAX - 1) buf[blen++] = *p++;
                else p++;
            }

            buf[blen] = '\0';
            if (blen > 0) tl_push(tl, TOK_WORD, buf);
        }
    }

    tl_push(tl, TOK_EOF, NULL);
    return tl;
}

void tokenlist_free(TokenList *tl)
{
    if (!tl) return;
    for (int i = 0; i < tl->count; i++) free(tl->tokens[i].value);
    free(tl->tokens);
    free(tl);
}
