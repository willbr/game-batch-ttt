/*
 * cmd.c -- minimal cmd.exe emulator for game-batch-ttt
 *
 * Runs the batch subset used by bin/main.cmd and friends.
 * reply.com / ping / start are faked; everything else is
 * interpreted from the .cmd source.
 *
 *   cc -O2 -o cmd cmd.c
 *   ./cmd tictactoe.cmd /test
 *   ./cmd tictactoe.cmd
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------- */
/* utilities                                                       */
/* --------------------------------------------------------------- */

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("cmd: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) die("oom"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n); if (!p) die("oom"); return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s)+1; char *p = xmalloc(n); memcpy(p, s, n); return p; }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n+1); memcpy(p, s, n); p[n] = 0; return p; }

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int strieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = lc((unsigned char)*a), cb = lc((unsigned char)*b);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static int strnieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = lc((unsigned char)a[i]), cb = lc((unsigned char)b[i]);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

static char *trim_left(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
}

/* Growable byte buffer. */
typedef struct { char *s; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->cap = 64; b->len = 0; b->s = xmalloc(b->cap); b->s[0] = 0; }
static void buf_free(Buf *b) { free(b->s); b->s = NULL; }
static void buf_reset(Buf *b) { b->len = 0; if (b->s) b->s[0] = 0; }
static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
}
static void buf_putc(Buf *b, char c) { buf_reserve(b, 1); b->s[b->len++] = c; b->s[b->len] = 0; }
static void buf_puts(Buf *b, const char *s) { size_t n = strlen(s); buf_reserve(b, n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
static void buf_putn(Buf *b, const char *s, size_t n) { buf_reserve(b, n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
static char *buf_take(Buf *b) { char *r = b->s; b->s = NULL; b->len = b->cap = 0; return r; }

/* --------------------------------------------------------------- */
/* variables                                                       */
/* --------------------------------------------------------------- */

typedef struct Var { char *name_lc; char *value; struct Var *next; } Var;
static Var *g_vars = NULL;

static void name_lower(const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    if (n + 1 > cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = lc((unsigned char)name[i]);
    out[n] = 0;
}

static const char *var_get(const char *name) {
    char key[128]; name_lower(name, key, sizeof key);
    for (Var *v = g_vars; v; v = v->next)
        if (strcmp(v->name_lc, key) == 0) return v->value;
    return NULL;
}

static void var_unset(const char *name) {
    char key[128]; name_lower(name, key, sizeof key);
    Var **pp = &g_vars;
    while (*pp) {
        if (strcmp((*pp)->name_lc, key) == 0) {
            Var *v = *pp;
            *pp = v->next;
            free(v->name_lc); free(v->value); free(v);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void var_set(const char *name, const char *value) {
    if (!value || !*value) { var_unset(name); return; }
    char key[128]; name_lower(name, key, sizeof key);
    for (Var *v = g_vars; v; v = v->next) {
        if (strcmp(v->name_lc, key) == 0) {
            free(v->value); v->value = xstrdup(value); return;
        }
    }
    Var *v = xmalloc(sizeof *v);
    v->name_lc = xstrdup(key);
    v->value = xstrdup(value);
    v->next = g_vars;
    g_vars = v;
}

/* --------------------------------------------------------------- */
/* frames                                                          */
/* --------------------------------------------------------------- */

struct Script;

typedef struct Frame {
    struct Script *script;
    int pc;
    char **args;         /* args[0] = %0, args[1] = %1, ... */
    int argc;
    int returning;       /* exit /b fired in this frame */
    int return_code;
    struct Frame *parent;
} Frame;

static Frame *g_frame = NULL;
static int g_errorlevel = 0;
static volatile sig_atomic_t g_sigint = 0;

/* --------------------------------------------------------------- */
/* script loading                                                  */
/* --------------------------------------------------------------- */

typedef struct Script {
    char *path;
    char **stmts;        /* each stmt is one logical statement (may span \n) */
    int nstmts;
    char **lbl_name;     /* lowercased labels */
    int *lbl_stmt;       /* stmt index of the label line */
    int nlabels;
} Script;

/* Count paren depth change, ignoring quoted strings and ^-escapes. */
static int paren_delta(const char *s) {
    int d = 0, q = 0;
    while (*s) {
        if (*s == '^' && s[1]) { s += 2; continue; }
        if (*s == '"') { q = !q; s++; continue; }
        if (!q) {
            if (*s == '(') d++;
            else if (*s == ')') d--;
        }
        s++;
    }
    return d;
}

static char *slurp(const char *path, size_t *olen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = xmalloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    if (olen) *olen = n;
    return buf;
}

static Script *script_load(const char *path) {
    size_t len;
    char *text = slurp(path, &len);
    if (!text) return NULL;

    Script *s = xmalloc(sizeof *s);
    s->path = xstrdup(path);
    s->stmts = NULL; s->nstmts = 0;
    s->lbl_name = NULL; s->lbl_stmt = NULL; s->nlabels = 0;

    int scap = 0, lcap = 0;
    Buf cur; buf_init(&cur);
    int depth = 0;

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t end = i;
        if (end > start && text[end-1] == '\r') end--;
        if (i < len) i++;

        char *line = xstrndup(text + start, end - start);

        if (depth == 0) {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == ':' && t[1] != ':' ) {
                /* label */
                char *q = t + 1;
                char *e = q;
                while (*e && !isspace((unsigned char)*e)) e++;
                size_t nlen = (size_t)(e - q);
                char *name = xstrndup(q, nlen);
                for (char *p = name; *p; p++) *p = lc((unsigned char)*p);
                if (s->nlabels >= lcap) { lcap = lcap ? lcap*2 : 16; s->lbl_name = xrealloc(s->lbl_name, lcap * sizeof *s->lbl_name); s->lbl_stmt = xrealloc(s->lbl_stmt, lcap * sizeof *s->lbl_stmt); }
                s->lbl_name[s->nlabels] = name;
                s->lbl_stmt[s->nlabels] = s->nstmts;
                s->nlabels++;
                if (s->nstmts >= scap) { scap = scap ? scap*2 : 64; s->stmts = xrealloc(s->stmts, scap * sizeof *s->stmts); }
                s->stmts[s->nstmts++] = xstrdup(line);
                free(line);
                continue;
            }
        }

        if (cur.len > 0) buf_putc(&cur, '\n');
        buf_puts(&cur, line);
        depth += paren_delta(line);
        if (depth < 0) depth = 0;

        if (depth == 0) {
            if (s->nstmts >= scap) { scap = scap ? scap*2 : 64; s->stmts = xrealloc(s->stmts, scap * sizeof *s->stmts); }
            s->stmts[s->nstmts++] = xstrdup(cur.s);
            buf_reset(&cur);
        }
        free(line);
    }
    if (cur.len > 0) {
        if (s->nstmts >= scap) { scap = scap ? scap*2 : 64; s->stmts = xrealloc(s->stmts, scap * sizeof *s->stmts); }
        s->stmts[s->nstmts++] = xstrdup(cur.s);
    }
    buf_free(&cur);
    free(text);
    return s;
}

static int script_find_label(Script *s, const char *lbl) {
    char key[128]; name_lower(lbl, key, sizeof key);
    if (strcmp(key, "eof") == 0) return s->nstmts;
    for (int i = 0; i < s->nlabels; i++)
        if (strcmp(s->lbl_name[i], key) == 0) return s->lbl_stmt[i];
    return -1;
}

static void script_free(Script *s) {
    if (!s) return;
    for (int i = 0; i < s->nstmts; i++) free(s->stmts[i]);
    free(s->stmts);
    for (int i = 0; i < s->nlabels; i++) free(s->lbl_name[i]);
    free(s->lbl_name); free(s->lbl_stmt);
    free(s->path);
    free(s);
}

/* --------------------------------------------------------------- */
/* variable expansion                                              */
/* --------------------------------------------------------------- */

static const char *get_special(const char *name_lc, char *buf, size_t cap) {
    if (strcmp(name_lc, "errorlevel") == 0) { snprintf(buf, cap, "%d", g_errorlevel); return buf; }
    if (strcmp(name_lc, "random") == 0) { snprintf(buf, cap, "%d", rand() & 0x7fff); return buf; }
    if (strcmp(name_lc, "cd") == 0) { if (getcwd(buf, cap)) return buf; return ""; }
    return NULL;
}

static const char *var_lookup_any(const char *name, char *scratch, size_t cap) {
    char key[128]; name_lower(name, key, sizeof key);
    const char *sp = get_special(key, scratch, cap);
    if (sp) return sp;
    for (Var *v = g_vars; v; v = v->next)
        if (strcmp(v->name_lc, key) == 0) return v->value;
    return NULL;
}

static void emit_sub(Buf *b, const char *v, long start, long len, int has_len) {
    long n = (long)strlen(v);
    if (start < 0) { start = n + start; if (start < 0) start = 0; }
    if (start > n) start = n;
    long rem = n - start;
    long take = has_len ? len : rem;
    if (has_len && len < 0) take = rem + len;
    if (take < 0) take = 0;
    if (take > rem) take = rem;
    buf_putn(b, v + start, (size_t)take);
}

/* Expand %var% / !var! / %N / %* / %~N / %% / ^ escapes. */
static char *expand(const char *in) {
    Buf out; buf_init(&out);
    char scratch[64];

    for (const char *p = in; *p; ) {
        if (*p == '^' && p[1]) { buf_putc(&out, p[1]); p += 2; continue; }

        /* %...% : check %* / %~N / %N first, then scan greedily for closing %.
         * An empty name between two adjacent %'s means the literal %% escape. */
        if (*p == '%') {
            if (p[1] == '*') {
                if (g_frame) {
                    for (int i = 1; i < g_frame->argc; i++) {
                        if (i > 1) buf_putc(&out, ' ');
                        buf_puts(&out, g_frame->args[i]);
                    }
                }
                p += 2; continue;
            }
            if (p[1] == '~' && p[2] >= '0' && p[2] <= '9') {
                int n = p[2] - '0';
                p += 3;
                if (g_frame && n < g_frame->argc) {
                    const char *a = g_frame->args[n];
                    size_t al = strlen(a);
                    if (al >= 2 && a[0] == '"' && a[al-1] == '"') buf_putn(&out, a+1, al-2);
                    else buf_puts(&out, a);
                }
                continue;
            }
            if (p[1] >= '0' && p[1] <= '9') {
                int n = p[1] - '0';
                p += 2;
                if (g_frame && n < g_frame->argc) buf_puts(&out, g_frame->args[n]);
                continue;
            }
            const char *q = p + 1;
            const char *end = NULL;
            while (*q) { if (*q == '%') { end = q; break; } q++; }
            if (!end) { buf_putc(&out, '%'); p++; continue; }
            size_t raw_nlen = (size_t)(end - (p + 1));
            if (raw_nlen == 0) {
                /* %% -- escape to single literal % */
                buf_putc(&out, '%');
                p = end + 1;
                continue;
            }
            const char *colon = NULL;
            for (const char *r = p + 1; r < end; r++) if (*r == ':') { colon = r; break; }
            size_t nlen = (colon ? colon : end) - (p + 1);
            if (nlen >= 128) { p = end + 1; continue; }
            char name[128]; memcpy(name, p + 1, nlen); name[nlen] = 0;
            const char *val = var_lookup_any(name, scratch, sizeof scratch);
            if (val && colon) {
                const char *m = colon + 1;
                if (*m == '~') {
                    m++;
                    char *me;
                    long sv = strtol(m, &me, 10); m = me;
                    int hl = 0; long lv = 0;
                    if (*m == ',') { m++; lv = strtol(m, &me, 10); hl = 1; }
                    emit_sub(&out, val, sv, lv, hl);
                } else {
                    buf_puts(&out, val);
                }
            } else if (val) {
                buf_puts(&out, val);
            }
            p = end + 1;
            continue;
        }

        /* !...! (delayed) */
        if (*p == '!') {
            const char *q = p + 1;
            const char *end = NULL;
            while (*q) { if (*q == '!') { end = q; break; } q++; }
            if (!end) { buf_putc(&out, '!'); p++; continue; }
            const char *colon = NULL;
            for (const char *r = p + 1; r < end; r++) if (*r == ':') { colon = r; break; }
            size_t nlen = (colon ? colon : end) - (p + 1);
            if (nlen == 0 || nlen >= 128) { p = end + 1; continue; }
            char name[128]; memcpy(name, p + 1, nlen); name[nlen] = 0;
            const char *val = var_lookup_any(name, scratch, sizeof scratch);
            if (val && colon) {
                const char *m = colon + 1;
                if (*m == '~') {
                    m++;
                    char *me;
                    long sv = strtol(m, &me, 10); m = me;
                    int hl = 0; long lv = 0;
                    if (*m == ',') { m++; lv = strtol(m, &me, 10); hl = 1; }
                    emit_sub(&out, val, sv, lv, hl);
                } else {
                    buf_puts(&out, val);
                }
            } else if (val) {
                buf_puts(&out, val);
            }
            p = end + 1;
            continue;
        }

        buf_putc(&out, *p);
        p++;
    }
    return buf_take(&out);
}

/* --------------------------------------------------------------- */
/* set /a expression evaluator                                     */
/* --------------------------------------------------------------- */

typedef struct { const char *p; } Lex;
static void lex_ws(Lex *L) { while (*L->p == ' ' || *L->p == '\t') L->p++; }
static long parse_expr(Lex *L);

static long ident_val(const char *name) {
    const char *v = var_get(name);
    if (!v) return 0;
    return strtol(v, NULL, 10);
}

static long parse_primary(Lex *L) {
    lex_ws(L);
    if (*L->p == '(') {
        L->p++;
        long x = parse_expr(L);
        lex_ws(L);
        if (*L->p == ')') L->p++;
        return x;
    }
    if (*L->p == '-') { L->p++; return -parse_primary(L); }
    if (*L->p == '+') { L->p++; return parse_primary(L); }
    if (*L->p == '!') { L->p++; return !parse_primary(L); }
    if (*L->p == '~') { L->p++; return ~parse_primary(L); }
    if (isdigit((unsigned char)*L->p)) {
        char *e;
        long x = strtol(L->p, &e, 0);
        L->p = e;
        return x;
    }
    if (isalpha((unsigned char)*L->p) || *L->p == '_') {
        char name[128]; size_t n = 0;
        while (*L->p && (isalnum((unsigned char)*L->p) || *L->p == '_')) {
            if (n + 1 < sizeof name) name[n++] = *L->p;
            L->p++;
        }
        name[n] = 0;
        lex_ws(L);
        if (*L->p == '=' && L->p[1] != '=') {
            L->p++;
            long v = parse_expr(L);
            char tmp[32]; snprintf(tmp, sizeof tmp, "%ld", v);
            var_set(name, tmp);
            return v;
        }
        if ((L->p[0] == '+' || L->p[0] == '-' || L->p[0] == '*' ||
             L->p[0] == '/' || L->p[0] == '%') && L->p[1] == '=') {
            int op = L->p[0];
            L->p += 2;
            long a = ident_val(name), b = parse_expr(L), r = a;
            switch (op) {
                case '+': r = a + b; break;
                case '-': r = a - b; break;
                case '*': r = a * b; break;
                case '/': r = b ? a / b : 0; break;
                case '%': r = b ? a % b : 0; break;
            }
            char tmp[32]; snprintf(tmp, sizeof tmp, "%ld", r);
            var_set(name, tmp);
            return r;
        }
        return ident_val(name);
    }
    return 0;
}

static long parse_muldiv(Lex *L) {
    long a = parse_primary(L);
    for (;;) {
        lex_ws(L);
        if (*L->p == '*' && L->p[1] != '=') { L->p++; a *= parse_primary(L); }
        else if (*L->p == '/' && L->p[1] != '=') { L->p++; long b = parse_primary(L); a = b ? a / b : 0; }
        else if (*L->p == '%' && L->p[1] != '=') { L->p++; long b = parse_primary(L); a = b ? a % b : 0; }
        else break;
    }
    return a;
}

static long parse_addsub(Lex *L) {
    long a = parse_muldiv(L);
    for (;;) {
        lex_ws(L);
        if (*L->p == '+' && L->p[1] != '=') { L->p++; a += parse_muldiv(L); }
        else if (*L->p == '-' && L->p[1] != '=') { L->p++; a -= parse_muldiv(L); }
        else break;
    }
    return a;
}

static long parse_expr(Lex *L) {
    long x = parse_addsub(L);
    lex_ws(L);
    while (*L->p == ',') { L->p++; x = parse_addsub(L); lex_ws(L); }
    return x;
}

static long eval_expr(const char *e) { Lex L = { e }; return parse_expr(&L); }

/* --------------------------------------------------------------- */
/* command-line helpers                                            */
/* --------------------------------------------------------------- */

/* Split s on top-level '&' or '\n'. Fills out[] with owned strdup'd strings.
 * Skips '&' when it's part of a redirection like `>&N` or `2>&N`. */
static int split_seq(const char *s, char ***out) {
    int cap = 4, n = 0;
    char **v = xmalloc(cap * sizeof *v);
    const char *start = s;
    int d = 0, q = 0;
    for (const char *p = s; ; p++) {
        int at_end = (*p == 0);
        int split_here = 0;
        if (!at_end) {
            if (*p == '^' && p[1]) { p++; continue; }
            if (*p == '"') { q = !q; continue; }
            if (!q) {
                if (*p == '(') d++;
                else if (*p == ')') { if (d > 0) d--; }
                else if (d == 0 && *p == '\n') {
                    split_here = 1;
                }
                else if (d == 0 && *p == '&' && p[1] != '&') {
                    /* &&  OR  preceding char is '>' (redir like >&N, 2>&1) */
                    const char *prev = p;
                    while (prev > s && (prev[-1] == ' ' || prev[-1] == '\t')) prev--;
                    if (!(prev > s && prev[-1] == '>')) split_here = 1;
                }
            }
        }
        if (at_end || split_here) {
            size_t L = (size_t)(p - start);
            char *t = xstrndup(start, L);
            if (n >= cap) { cap *= 2; v = xrealloc(v, cap * sizeof *v); }
            v[n++] = t;
            if (at_end) break;
            start = p + 1;
        }
    }
    *out = v;
    return n;
}

/* Peel off a leading parenthesized block "(...)" from s.
 * On success, returns newly allocated body string (without outer parens),
 * and writes rest pointer into *rest_out. Returns NULL if s doesn't start with '('. */
static char *peel_paren(const char *s, const char **rest_out) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '(') return NULL;
    const char *p = s + 1;
    int d = 1, q = 0;
    const char *body_start = p;
    while (*p) {
        if (*p == '^' && p[1]) { p += 2; continue; }
        if (*p == '"') { q = !q; p++; continue; }
        if (!q) {
            if (*p == '(') d++;
            else if (*p == ')') { d--; if (d == 0) break; }
        }
        p++;
    }
    if (d != 0) return NULL;
    char *body = xstrndup(body_start, (size_t)(p - body_start));
    *rest_out = (*p == ')') ? p + 1 : p;
    return body;
}

/* --------------------------------------------------------------- */
/* redirection parsing                                             */
/* --------------------------------------------------------------- */

static char *path_norm(const char *p);
static void ensure_parent_dir(const char *path);

typedef struct {
    int saved_stdout;
    int saved_stderr;
} RedirSaved;

/* Parse trailing redirects off *line (mutates by NUL-termination).
 * Applies them and returns the saved fds so caller can restore. */
static int apply_redir(char *line, RedirSaved *saved) {
    saved->saved_stdout = -1;
    saved->saved_stderr = -1;

    /* scan from left, find redir operators at top level */
    int dup_err_to_out = 0; /* 2>&1 */
    int dup_out_to_err = 0; /* 1>&2 or >&2 */
    char *out_file = NULL; int out_append = 0;
    char *err_file = NULL; int err_append = 0;

    /* we'll do a left-to-right scan, cutting tokens as we go */
    char *p = line;
    int q = 0, d = 0;
    while (*p) {
        if (*p == '^' && p[1]) { p += 2; continue; }
        if (*p == '"') { q = !q; p++; continue; }
        if (q) { p++; continue; }
        if (*p == '(') d++;
        else if (*p == ')') { if (d) d--; }

        if (d == 0) {
            /* match: 2>&1 */
            if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
                dup_err_to_out = 1;
                memmove(p, p + 4, strlen(p + 4) + 1);
                continue;
            }
            if ((p[0] == '1' && p[1] == '>' && p[2] == '&' && p[3] == '2')) {
                dup_out_to_err = 1;
                memmove(p, p + 4, strlen(p + 4) + 1);
                continue;
            }
            if (p[0] == '>' && p[1] == '&' && p[2] == '2') {
                dup_out_to_err = 1;
                memmove(p, p + 3, strlen(p + 3) + 1);
                continue;
            }
            if (p[0] == '2' && p[1] == '>') {
                int app = (p[2] == '>');
                char *q2 = p + (app ? 3 : 2);
                while (*q2 == ' ' || *q2 == '\t') q2++;
                char *fe = q2;
                int inq = 0;
                while (*fe && (inq || (*fe != ' ' && *fe != '\t' && *fe != '>' && *fe != '<' && *fe != '&'))) {
                    if (*fe == '"') inq = !inq;
                    fe++;
                }
                size_t fl = (size_t)(fe - q2);
                free(err_file);
                err_file = xstrndup(q2, fl);
                /* strip quotes */
                if (fl >= 2 && err_file[0] == '"' && err_file[fl-1] == '"') {
                    memmove(err_file, err_file + 1, fl - 2);
                    err_file[fl - 2] = 0;
                }
                err_append = app;
                memmove(p, fe, strlen(fe) + 1);
                continue;
            }
            if (p[0] == '>') {
                int app = (p[1] == '>');
                char *q2 = p + (app ? 2 : 1);
                while (*q2 == ' ' || *q2 == '\t') q2++;
                char *fe = q2;
                int inq = 0;
                while (*fe && (inq || (*fe != ' ' && *fe != '\t' && *fe != '>' && *fe != '<' && *fe != '&'))) {
                    if (*fe == '"') inq = !inq;
                    fe++;
                }
                size_t fl = (size_t)(fe - q2);
                free(out_file);
                out_file = xstrndup(q2, fl);
                if (fl >= 2 && out_file[0] == '"' && out_file[fl-1] == '"') {
                    memmove(out_file, out_file + 1, fl - 2);
                    out_file[fl - 2] = 0;
                }
                out_append = app;
                memmove(p, fe, strlen(fe) + 1);
                continue;
            }
            /* input redir: we just drop it (games never need stdin from files here) */
            if (p[0] == '<') {
                char *q2 = p + 1;
                while (*q2 == ' ' || *q2 == '\t') q2++;
                char *fe = q2;
                int inq = 0;
                while (*fe && (inq || (*fe != ' ' && *fe != '\t' && *fe != '>' && *fe != '<' && *fe != '&'))) {
                    if (*fe == '"') inq = !inq;
                    fe++;
                }
                memmove(p, fe, strlen(fe) + 1);
                continue;
            }
        }
        p++;
    }

    /* apply in order: out, err, dups */
    fflush(stdout); fflush(stderr);

    if (out_file) {
        int nul = strieq(out_file, "nul");
        char *np = nul ? xstrdup("/dev/null") : path_norm(out_file);
        if (!nul) ensure_parent_dir(np);
        const char *path = np;
        int fd = open(path, O_WRONLY | O_CREAT | (out_append ? O_APPEND : O_TRUNC), 0644);
        if (fd >= 0) {
            saved->saved_stdout = dup(1);
            dup2(fd, 1);
            close(fd);
        }
        free(np);
        free(out_file);
    }
    if (err_file) {
        int nul = strieq(err_file, "nul");
        char *np = nul ? xstrdup("/dev/null") : path_norm(err_file);
        if (!nul) ensure_parent_dir(np);
        int fd = open(np, O_WRONLY | O_CREAT | (err_append ? O_APPEND : O_TRUNC), 0644);
        if (fd >= 0) {
            saved->saved_stderr = dup(2);
            dup2(fd, 2);
            close(fd);
        }
        free(np);
        free(err_file);
    }
    if (dup_err_to_out) {
        if (saved->saved_stderr < 0) saved->saved_stderr = dup(2);
        dup2(1, 2);
    }
    if (dup_out_to_err) {
        if (saved->saved_stdout < 0) saved->saved_stdout = dup(1);
        dup2(2, 1);
    }
    return 0;
}

static void restore_redir(RedirSaved *saved) {
    fflush(stdout); fflush(stderr);
    if (saved->saved_stdout >= 0) { dup2(saved->saved_stdout, 1); close(saved->saved_stdout); }
    if (saved->saved_stderr >= 0) { dup2(saved->saved_stderr, 2); close(saved->saved_stderr); }
}

/* --------------------------------------------------------------- */
/* path helpers                                                    */
/* --------------------------------------------------------------- */

/* Normalize backslashes in path -> forward slashes (copy). */
static char *path_norm(const char *p) {
    char *r = xstrdup(p);
    for (char *q = r; *q; q++) if (*q == '\\') *q = '/';
    return r;
}

static int file_exists(const char *path) {
    struct stat st;
    char *np = path_norm(path);
    int ok = (stat(np, &st) == 0);
    free(np);
    return ok;
}

/* mkdir -p for the parent directory of path. Ignores errors; best-effort. */
static void ensure_parent_dir(const char *path) {
    char *p = xstrdup(path);
    /* strip trailing slashes */
    size_t n = strlen(p);
    while (n && (p[n-1] == '/' || p[n-1] == '\\')) p[--n] = 0;
    /* walk forward creating each prefix */
    for (char *q = p + 1; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char c = *q; *q = 0;
            mkdir(p, 0755);
            *q = c;
        }
    }
    free(p);
}

/* Try find script file by name: name, name.cmd, name.bat in cwd and in
 * dirname(argv0-equivalent). Returns newly-allocated path or NULL. */
static char *find_script(const char *name) {
    char *norm = path_norm(name);
    const char *exts[] = { "", ".cmd", ".bat", NULL };
    for (int i = 0; exts[i]; i++) {
        Buf b; buf_init(&b);
        buf_puts(&b, norm); buf_puts(&b, exts[i]);
        if (file_exists(b.s)) { free(norm); return buf_take(&b); }
        buf_free(&b);
    }
    free(norm);
    return NULL;
}

/* --------------------------------------------------------------- */
/* forward declarations                                            */
/* --------------------------------------------------------------- */

static int execute_stmt(const char *raw);
static int execute_seq(const char *raw);
static int execute_script(Script *s, char **args, int argc);

/* --------------------------------------------------------------- */
/* fake externals                                                  */
/* --------------------------------------------------------------- */

static struct termios g_tio_save;
static int g_tio_saved = 0;

static void tty_cbreak_on(void) {
    if (!isatty(0)) return;
    if (tcgetattr(0, &g_tio_save) != 0) return;
    g_tio_saved = 1;
    struct termios t = g_tio_save;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void tty_cbreak_off(void) {
    if (g_tio_saved) { tcsetattr(0, TCSANOW, &g_tio_save); g_tio_saved = 0; }
}

static void on_sigint(int sig) { (void)sig; g_sigint = 1; }

/* fake `reply` -- read one key, errorlevel = key code (0 = ctrl-c) */
static int fake_reply(void) {
    tty_cbreak_on();
    unsigned char c;
    g_sigint = 0;
    ssize_t n = read(0, &c, 1);
    tty_cbreak_off();
    if (g_sigint) return 0;
    if (n <= 0) return 0;
    if (c == 3) return 0;  /* ctrl-c */
    return (int)c;
}

/* fake `ping ... -w MS ...` -- if -w MS is present, sleep MS ms. */
static int fake_ping(int argc, char **argv) {
    long ms = 0;
    for (int i = 1; i < argc; i++) {
        if (strieq(argv[i], "-w") && i + 1 < argc) {
            ms = strtol(argv[i + 1], NULL, 10);
        }
    }
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return 0;
}

/* --------------------------------------------------------------- */
/* tokenize a command into argv (space/tab separated, "" quotes)   */
/* --------------------------------------------------------------- */

static int tok_argv(const char *s, char ***out) {
    int cap = 4, n = 0;
    char **v = xmalloc(cap * sizeof *v);
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        Buf b; buf_init(&b);
        int q = 0;
        while (*s) {
            if (*s == '^' && s[1]) { buf_putc(&b, s[1]); s += 2; continue; }
            if (*s == '"') { q = !q; buf_putc(&b, '"'); s++; continue; }
            if (!q && (*s == ' ' || *s == '\t')) break;
            buf_putc(&b, *s);
            s++;
        }
        if (n >= cap) { cap *= 2; v = xrealloc(v, cap * sizeof *v); }
        v[n++] = buf_take(&b);
    }
    *out = v;
    return n;
}

static void free_argv(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Strip surrounding "..." from an arg (in-place copy). */
static char *unquote(const char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') return xstrndup(s + 1, n - 2);
    return xstrdup(s);
}

/* --------------------------------------------------------------- */
/* echo                                                            */
/* --------------------------------------------------------------- */

/* cmd_echo receives the text AFTER "echo" (which may start with a punctuation
 * like '.', ':', ';', etc. -- those mean "just print the rest as a line").
 * If rest is empty or "on"/"off", we treat it as a state change (ignored). */
static int cmd_echo(const char *rest) {
    if (*rest == 0) return 0; /* `echo` alone would show state; just no-op */
    if (strieq(rest, "off") || strieq(rest, "on")) return 0;
    /* cmd: `echo.<anything>` prints <anything> on its own line (incl. blank). */
    /* rest may begin with the punctuation that was glued to "echo". */
    if (*rest == ' ') rest++;
    printf("%s\n", rest);
    return 0;
}

/* --------------------------------------------------------------- */
/* set                                                             */
/* --------------------------------------------------------------- */

static int cmd_set(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) return 0; /* ignore: would list all vars */
    int slash_a = 0;
    if (rest[0] == '/' && (rest[1] == 'a' || rest[1] == 'A') && (rest[2] == ' ' || rest[2] == '\t' || rest[2] == 0)) {
        slash_a = 1;
        rest += 2;
        while (*rest == ' ' || *rest == '\t') rest++;
    }
    if (rest[0] == '/' && (rest[1] == 'p' || rest[1] == 'P') && (rest[2] == ' ' || rest[2] == '\t' || rest[2] == 0)) {
        /* set /p var=prompt  (we skip actually prompting; write.cmd uses it with <nul) */
        rest += 2;
        while (*rest == ' ' || *rest == '\t') rest++;
        const char *eq = strchr(rest, '=');
        if (!eq) return 0;
        char *name = xstrndup(rest, (size_t)(eq - rest));
        /* trim name */
        trim_right(name);
        char *nm = trim_left(name);
        /* print prompt if any, no newline */
        const char *prompt = eq + 1;
        char *up = unquote(prompt);
        fputs(up, stdout); fflush(stdout);
        free(up);
        /* don't read -- stdin is assumed piped to nul */
        var_set(nm, "");
        free(name);
        return 0;
    }
    if (slash_a) {
        long v = eval_expr(rest);
        /* ECHO result only when invoked from prompt; when in a script, stay silent. */
        (void)v;
        return 0;
    }
    /* set NAME=VALUE */
    const char *eq = strchr(rest, '=');
    if (!eq) {
        /* `set NAME` -- list matching. ignore. */
        return 0;
    }
    const char *name_end = eq;
    /* trim trailing ws before '=' */
    while (name_end > rest && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
    if (name_end == rest) return 0;
    char *name = xstrndup(rest, (size_t)(name_end - rest));
    const char *val = eq + 1;
    /* cmd does NOT trim leading whitespace of value. */
    var_set(name, val);
    free(name);
    return 0;
}

/* --------------------------------------------------------------- */
/* if                                                              */
/* --------------------------------------------------------------- */

/* Consume one term of an if condition. Returns the operand string (owned)
 * and advances *pp past it. A term is either a "quoted string" or a bareword. */
static char *if_term(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return xstrdup(""); }
    if (*p == '"') {
        const char *q = p + 1;
        while (*q && *q != '"') q++;
        char *r;
        if (*q == '"') { r = xstrndup(p, (size_t)(q - p + 1)); *pp = q + 1; }
        else { r = xstrdup(p); *pp = p + strlen(p); }
        return r;
    }
    const char *q = p;
    while (*q && *q != ' ' && *q != '\t') q++;
    char *r = xstrndup(p, (size_t)(q - p));
    *pp = q;
    return r;
}

static int both_numeric(const char *a, const char *b, long *ai, long *bi) {
    /* strip surrounding quotes */
    char *ua = unquote(a), *ub = unquote(b);
    char *ea, *eb;
    long va = strtol(ua, &ea, 10);
    long vb = strtol(ub, &eb, 10);
    int ok = (*ua && *ea == 0 && *ub && *eb == 0);
    if (ok) { *ai = va; *bi = vb; }
    free(ua); free(ub);
    return ok;
}

static int str_eq_cmd(const char *a, const char *b) {
    /* cmd's "==" is a literal string comparison; we compare as-given
     * but with surrounding quotes unified. */
    char *ua = unquote(a), *ub = unquote(b);
    int r = strcmp(ua, ub) == 0;
    free(ua); free(ub);
    return r;
}

static int str_cmp_cmd(const char *a, const char *b) {
    char *ua = unquote(a), *ub = unquote(b);
    int r = strcmp(ua, ub);
    free(ua); free(ub);
    return r;
}

/* Evaluate an `if ...` statement. `s` is RAW text (no prior var expansion)
 * after the leading `if`. We expand operands as we parse them so delayed
 * !var! expansion resolves at execution time; the body is left raw so its
 * sub-commands expand at their own execution time. */
static int eval_if(const char *s) {
    while (*s == ' ' || *s == '\t') s++;

    int negate = 0;
    if (strnieq(s, "not ", 4)) { negate = 1; s += 4; while (*s == ' ' || *s == '\t') s++; }

    int cond = 0;
    const char *body_start = NULL;

    if (strnieq(s, "errorlevel ", 11)) {
        const char *p = s + 11;
        /* N may be an expanded variable, expand just this number */
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') e++;
        char *numraw = xstrndup(p, (size_t)(e - p));
        char *num = expand(numraw);
        long n = strtol(num, NULL, 10);
        free(num); free(numraw);
        cond = (g_errorlevel >= n);
        body_start = e;
    }
    else if (strnieq(s, "exist ", 6)) {
        const char *p = s + 6;
        char *term = if_term(&p);
        char *exp1 = expand(term);
        char *uq = unquote(exp1);
        cond = file_exists(uq);
        free(uq); free(exp1); free(term);
        body_start = p;
    }
    else if (strnieq(s, "defined ", 8)) {
        const char *p = s + 8;
        char *term = if_term(&p);
        char *exp1 = expand(term);
        cond = (var_get(exp1) != NULL);
        free(exp1); free(term);
        body_start = p;
    }
    else {
        const char *p = s;
        char *a = if_term(&p);
        char *aexp = expand(a);
        free(a);
        while (*p == ' ' || *p == '\t') p++;
        int handled = 0;
        if (p[0] == '=' && p[1] == '=') {
            p += 2;
            char *b = if_term(&p);
            char *bexp = expand(b);
            free(b);
            cond = str_eq_cmd(aexp, bexp);
            free(bexp); handled = 1;
        } else {
            const char *ops[] = { "EQU", "NEQ", "LSS", "LEQ", "GTR", "GEQ", NULL };
            int which = -1;
            for (int i = 0; ops[i]; i++) {
                size_t L = strlen(ops[i]);
                if (strnieq(p, ops[i], L) && (p[L] == ' ' || p[L] == '\t')) {
                    which = i; p += L; break;
                }
            }
            if (which >= 0) {
                char *b = if_term(&p);
                char *bexp = expand(b);
                free(b);
                long ai, bi; int r;
                if (both_numeric(aexp, bexp, &ai, &bi)) {
                    long d = ai - bi;
                    switch (which) {
                        case 0: r = (d == 0); break;
                        case 1: r = (d != 0); break;
                        case 2: r = (d <  0); break;
                        case 3: r = (d <= 0); break;
                        case 4: r = (d >  0); break;
                        case 5: r = (d >= 0); break;
                        default: r = 0;
                    }
                } else {
                    int d = str_cmp_cmd(aexp, bexp);
                    switch (which) {
                        case 0: r = (d == 0); break;
                        case 1: r = (d != 0); break;
                        case 2: r = (d <  0); break;
                        case 3: r = (d <= 0); break;
                        case 4: r = (d >  0); break;
                        case 5: r = (d >= 0); break;
                        default: r = 0;
                    }
                }
                cond = r;
                free(bexp); handled = 1;
            }
        }
        free(aexp);
        if (!handled) return 0;
        body_start = p;
    }

    if (negate) cond = !cond;

    while (*body_start == ' ' || *body_start == '\t' || *body_start == '\n') body_start++;

    const char *rest = NULL;
    char *true_body = NULL, *false_body = NULL;

    if (*body_start == '(') {
        true_body = peel_paren(body_start, &rest);
        if (!true_body) true_body = xstrdup("");
    } else {
        const char *p = body_start;
        while (*p) p++;
        true_body = xstrndup(body_start, (size_t)(p - body_start));
        rest = p;
    }
    while (*rest == ' ' || *rest == '\t' || *rest == '\n') rest++;
    if (strnieq(rest, "else", 4) && (rest[4] == ' ' || rest[4] == '\t' || rest[4] == '(' || rest[4] == '\n')) {
        rest += 4;
        while (*rest == ' ' || *rest == '\t' || *rest == '\n') rest++;
        if (*rest == '(') {
            const char *r2;
            false_body = peel_paren(rest, &r2);
        } else {
            false_body = xstrdup(rest);
        }
    }

    int rc = 0;
    if (cond) rc = execute_seq(true_body);
    else if (false_body) rc = execute_seq(false_body);
    free(true_body); free(false_body);
    return rc;
}

/* --------------------------------------------------------------- */
/* for                                                             */
/* --------------------------------------------------------------- */

/* Escape paren-tracking / statement-splitting specials with '^' so the value
 * survives split_seq and peel_paren intact. expand() will consume the ^ later. */
static void emit_escaped(Buf *out, const char *value) {
    for (const char *v = value; *v; v++) {
        char c = *v;
        if (c == '(' || c == ')' || c == '&' || c == '|' ||
            c == '<' || c == '>' || c == '^') {
            buf_putc(out, '^');
        }
        buf_putc(out, c);
    }
}

/* Replace every %%X and %X in body with value (textual, escaped). */
static char *for_subst(const char *body, char var, const char *value) {
    Buf out; buf_init(&out);
    for (const char *p = body; *p; ) {
        if (*p == '%' && p[1] == '%' && p[2] == var) {
            emit_escaped(&out, value);
            p += 3;
            continue;
        }
        if (*p == '%' && p[1] == var && !isalnum((unsigned char)p[2])) {
            emit_escaped(&out, value);
            p += 2;
            continue;
        }
        buf_putc(&out, *p);
        p++;
    }
    return buf_take(&out);
}

/* Execute a FOR statement. `rest` is the text after the leading `for`. */
static int do_for(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    int mode = 0; /* 0 = plain, 1 = /l numeric, 2 = /f file */
    const char *fopts = NULL;
    size_t foptslen = 0;
    if (rest[0] == '/' && (rest[1] == 'L' || rest[1] == 'l')) { mode = 1; rest += 2; }
    else if (rest[0] == '/' && (rest[1] == 'F' || rest[1] == 'f')) {
        mode = 2; rest += 2;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest == '"') {
            const char *q = rest + 1;
            while (*q && *q != '"') q++;
            fopts = rest + 1;
            foptslen = (size_t)(q - fopts);
            if (*q == '"') rest = q + 1; else rest = q;
        }
    } else if (rest[0] == '/' && (rest[1] == 'D' || rest[1] == 'd' || rest[1] == 'R' || rest[1] == 'r')) {
        rest += 2;
    }
    while (*rest == ' ' || *rest == '\t') rest++;
    /* loop var: %%X */
    if (!(rest[0] == '%' && rest[1] == '%')) return 0;
    char var = rest[2];
    rest += 3;
    while (*rest == ' ' || *rest == '\t') rest++;
    /* in (...) do BODY */
    if (!strnieq(rest, "in", 2)) return 0;
    rest += 2;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest != '(') return 0;
    const char *after;
    char *items = peel_paren(rest, &after);
    if (!items) return 0;
    while (*after == ' ' || *after == '\t' || *after == '\n') after++;
    if (!strnieq(after, "do", 2)) { free(items); return 0; }
    after += 2;
    while (*after == ' ' || *after == '\t' || *after == '\n') after++;
    char *body;
    if (*after == '(') {
        const char *r2;
        body = peel_paren(after, &r2);
        if (!body) body = xstrdup("");
    } else {
        body = xstrdup(after);
    }

    /* Expand vars in the items spec (for /l and /f need expanded path, etc.) */
    char *items_exp = expand(items);
    free(items);

    int rc = 0;

    if (mode == 1) {
        /* /l  %%X in (start,step,end) */
        long start = 0, step = 1, end = 0;
        char *tok = items_exp;
        char *comma = strchr(tok, ','); if (comma) *comma = 0;
        start = strtol(tok, NULL, 10);
        if (comma) {
            tok = comma + 1;
            char *c2 = strchr(tok, ','); if (c2) *c2 = 0;
            step = strtol(tok, NULL, 10);
            if (c2) end = strtol(c2 + 1, NULL, 10);
        }
        if (step == 0) step = 1;
        for (long i = start; (step > 0 ? i <= end : i >= end); i += step) {
            char val[32]; snprintf(val, sizeof val, "%ld", i);
            char *body2 = for_subst(body, var, val);
            rc = execute_seq(body2);
            free(body2);
            if (g_frame && g_frame->returning) break;
        }
    } else if (mode == 2) {
        /* /f %%X in (filename) do ... -- simple: one token per line (whitespace split first token) */
        /* supports "usebackq" (quoted file path) */
        char *path = items_exp;
        /* strip one level of quotes if present */
        size_t pl = strlen(path);
        if (pl >= 2 && path[0] == '"' && path[pl-1] == '"') { path[pl-1] = 0; path++; }
        char *np = path_norm(path);
        FILE *f = fopen(np, "rb");
        free(np);
        /* parse tokens=1,2 etc.  default: tokens=1 */
        int tokens_max = 1;
        if (fopts) {
            char o[256];
            size_t L = foptslen < sizeof o - 1 ? foptslen : sizeof o - 1;
            memcpy(o, fopts, L); o[L] = 0;
            char *t = strstr(o, "tokens=");
            if (t) {
                t += 7;
                /* find max token number referenced */
                int m = 0;
                while (*t && *t != ' ' && *t != '\t') {
                    if (isdigit((unsigned char)*t)) {
                        char *e;
                        long n = strtol(t, &e, 10);
                        if (n > m) m = (int)n;
                        t = e;
                    } else t++;
                }
                if (m > 0) tokens_max = m;
            }
        }
        if (f) {
            char line[4096];
            while (fgets(line, sizeof line, f)) {
                size_t L = strlen(line);
                while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
                /* skip blank */
                char *tp = line;
                while (*tp == ' ' || *tp == '\t') tp++;
                if (!*tp) continue;
                /* assign tokens_max tokens to %var, %var+1, ... */
                char *body2 = xstrdup(body);
                int k = 0;
                while (*tp && k < tokens_max) {
                    char *te = tp;
                    while (*te && *te != ' ' && *te != '\t') te++;
                    char *tok = xstrndup(tp, (size_t)(te - tp));
                    char *next = for_subst(body2, var + k, tok);
                    free(body2); body2 = next;
                    free(tok);
                    while (*te == ' ' || *te == '\t') te++;
                    tp = te;
                    k++;
                }
                rc = execute_seq(body2);
                free(body2);
                if (g_frame && g_frame->returning) break;
            }
            fclose(f);
        }
    } else {
        /* plain: iterate space-separated items */
        const char *p = items_exp;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
            if (!*p) break;
            const char *e = p;
            while (*e && *e != ' ' && *e != '\t' && *e != ',' && *e != ';') e++;
            char *val = xstrndup(p, (size_t)(e - p));
            char *body2 = for_subst(body, var, val);
            rc = execute_seq(body2);
            free(body2); free(val);
            p = e;
            if (g_frame && g_frame->returning) break;
        }
    }

    free(items_exp);
    free(body);
    return rc;
}

/* --------------------------------------------------------------- */
/* goto                                                            */
/* --------------------------------------------------------------- */

static int cmd_goto(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    const char *p = rest;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
    char *label = xstrndup(rest, (size_t)(p - rest));
    if (label[0] == ':') memmove(label, label + 1, strlen(label)); /* tolerate :label */
    if (!g_frame) { free(label); return 0; }
    if (strieq(label, "eof")) {
        g_frame->returning = 1;
        free(label);
        return 0;
    }
    int idx = script_find_label(g_frame->script, label);
    if (idx < 0) {
        fprintf(stderr, "cmd: label not found: %s\n", label);
        g_frame->returning = 1;
        g_errorlevel = 1;
        free(label);
        return 0;
    }
    g_frame->pc = idx;
    free(label);
    return 0;
}

/* --------------------------------------------------------------- */
/* call                                                            */
/* --------------------------------------------------------------- */

static int call_internal_or_external(const char *target, char **argv, int argc);

static int cmd_call(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) return 0;

    /* `call` triggers a second expansion pass (that's the whole point of
     * writing `call set x=%%game:~%1,1%%` in get_cell-style indirection). */
    char *exp2 = expand(rest);
    const char *p = exp2;

    char *target;
    if (*p == '"') {
        const char *q = p + 1;
        while (*q && *q != '"') q++;
        target = xstrndup(p + 1, (size_t)(q - (p + 1)));
        p = (*q == '"') ? q + 1 : q;
    } else {
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        target = xstrndup(p, (size_t)(q - p));
        p = q;
    }

    char **argv = NULL;
    int argn = tok_argv(p, &argv);
    int rc = call_internal_or_external(target, argv, argn);
    free_argv(argv, argn);
    free(target);
    free(exp2);
    return rc;
}

/* Call an internal-label subroutine (target starts with ':'). */
static int run_subroutine(const char *label, char **argv, int argc) {
    if (!g_frame) return 1;
    Script *s = g_frame->script;
    int idx = script_find_label(s, label);
    if (idx < 0) {
        fprintf(stderr, "cmd: label not found: :%s\n", label);
        g_errorlevel = 1;
        return 1;
    }
    Frame f;
    f.script = s;
    f.pc = idx;
    f.returning = 0;
    f.return_code = 0;
    f.parent = g_frame;
    /* args[0] = label string (preserve original case), args[1..] = call args */
    char **args = xmalloc((size_t)(argc + 1) * sizeof *args);
    Buf b0; buf_init(&b0); buf_putc(&b0, ':'); buf_puts(&b0, label);
    args[0] = buf_take(&b0);
    for (int i = 0; i < argc; i++) args[i + 1] = xstrdup(argv[i]);
    f.args = args;
    f.argc = argc + 1;

    g_frame = &f;
    while (f.pc < s->nstmts && !f.returning) {
        const char *stmt = s->stmts[f.pc];
        f.pc++;
        execute_stmt(stmt);
    }
    g_frame = f.parent;
    for (int i = 0; i < f.argc; i++) free(args[i]);
    free(args);
    return f.return_code;
}

/* Call an external script (other .cmd file). */
static int run_script_file(const char *path, char **argv, int argc) {
    Script *s = script_load(path);
    if (!s) {
        fprintf(stderr, "cmd: cannot load: %s\n", path);
        g_errorlevel = 1;
        return 1;
    }
    char **args = xmalloc((size_t)(argc + 1) * sizeof *args);
    args[0] = xstrdup(path);
    for (int i = 0; i < argc; i++) args[i + 1] = xstrdup(argv[i]);
    int rc = execute_script(s, args, argc + 1);
    for (int i = 0; i < argc + 1; i++) free(args[i]);
    free(args);
    script_free(s);
    return rc;
}

static int call_internal_or_external(const char *target, char **argv, int argc) {
    /* subroutine */
    if (target[0] == ':') return run_subroutine(target + 1, argv, argc);

    /* faked externals */
    if (strieq(target, "reply")) { g_errorlevel = fake_reply(); return g_errorlevel; }
    if (strieq(target, "ping")) {
        /* build argv with "ping" as argv[0] */
        char **a = xmalloc((size_t)(argc + 1) * sizeof *a);
        a[0] = xstrdup("ping");
        for (int i = 0; i < argc; i++) a[i + 1] = xstrdup(argv[i]);
        int rc = fake_ping(argc + 1, a);
        for (int i = 0; i < argc + 1; i++) free(a[i]);
        free(a);
        g_errorlevel = rc;
        return rc;
    }
    if (strieq(target, "start") || strieq(target, "debug") ||
        strieq(target, "sndrec32")) { g_errorlevel = 0; return 0; }
    if (strieq(target, "cmd") || strieq(target, "cmd.exe")) {
        /* cmd /c script args */
        int i = 0;
        while (i < argc && argv[i][0] == '/') i++; /* skip /c /k /q etc */
        if (i >= argc) return 0;
        const char *name = argv[i];
        char **rest = argv + i + 1;
        int rn = argc - i - 1;
        char *path = find_script(name);
        if (!path) { fprintf(stderr, "cmd: not found: %s\n", name); g_errorlevel = 1; return 1; }
        int rc = run_script_file(path, rest, rn);
        free(path);
        g_errorlevel = rc;
        return rc;
    }

    /* single-command internals reachable via call */
    if (strieq(target, "echo")) {
        Buf b; buf_init(&b);
        for (int i = 0; i < argc; i++) {
            if (i) buf_putc(&b, ' ');
            buf_puts(&b, argv[i]);
        }
        int rc = cmd_echo(b.s);
        buf_free(&b);
        return rc;
    }
    if (strieq(target, "set")) {
        Buf b; buf_init(&b);
        for (int i = 0; i < argc; i++) {
            if (i) buf_putc(&b, ' ');
            buf_puts(&b, argv[i]);
        }
        int rc = cmd_set(b.s);
        buf_free(&b);
        return rc;
    }

    /* fall back: find .cmd/.bat and run */
    char *path = find_script(target);
    if (path) {
        int rc = run_script_file(path, argv, argc);
        free(path);
        g_errorlevel = rc;
        return rc;
    }

    fprintf(stderr, "cmd: command not found: %s\n", target);
    (void)argv; (void)argc;
    g_errorlevel = 9009;
    return 9009;
}

/* --------------------------------------------------------------- */
/* exit                                                            */
/* --------------------------------------------------------------- */

static int cmd_exit(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    int slash_b = 0;
    if (rest[0] == '/' && (rest[1] == 'b' || rest[1] == 'B')) { slash_b = 1; rest += 2; while (*rest == ' ' || *rest == '\t') rest++; }
    int code = g_errorlevel;
    if (*rest) code = (int)strtol(rest, NULL, 10);
    g_errorlevel = code;
    if (slash_b) {
        if (g_frame) { g_frame->returning = 1; g_frame->return_code = code; }
        return code;
    }
    /* non-/b exits the whole process */
    exit(code);
}

/* --------------------------------------------------------------- */
/* misc built-ins                                                  */
/* --------------------------------------------------------------- */

typedef struct DirStack { char *path; struct DirStack *next; } DirStack;
static DirStack *g_dirs = NULL;

static int cmd_pushd(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return 1;
    DirStack *d = xmalloc(sizeof *d);
    d->path = xstrdup(cwd);
    d->next = g_dirs;
    g_dirs = d;
    char *p = unquote(rest);
    char *np = path_norm(p);
    int rc = chdir(np);
    free(np); free(p);
    if (rc != 0) { g_errorlevel = 1; return 1; }
    return 0;
}

static int cmd_popd(const char *rest) {
    (void)rest;
    if (!g_dirs) return 1;
    DirStack *d = g_dirs; g_dirs = d->next;
    chdir(d->path);
    free(d->path); free(d);
    return 0;
}

static int cmd_cd(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) return 0;
    char *p = unquote(rest);
    char *np = path_norm(p);
    int rc = chdir(np);
    free(np); free(p);
    g_errorlevel = rc ? 1 : 0;
    return rc;
}

static int cmd_type(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    char *p = unquote(rest);
    char *np = path_norm(p);
    FILE *f = fopen(np, "rb");
    free(np); free(p);
    if (!f) { g_errorlevel = 1; return 1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

static int cmd_setlocal(const char *rest) { (void)rest; return 0; }
static int cmd_endlocal(const char *rest) { (void)rest; return 0; }

/* --------------------------------------------------------------- */
/* single statement                                                */
/* --------------------------------------------------------------- */

/* Execute a single command (already expanded, no leading whitespace). */
static int run_simple(const char *s) {
    /* identify first word (command name) */
    const char *p = s;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t nlen = (size_t)(p - s);
    if (nlen == 0) return 0;
    char name[64];
    if (nlen >= sizeof name) nlen = sizeof name - 1;
    memcpy(name, s, nlen); name[nlen] = 0;
    const char *rest = p;
    while (*rest == ' ' || *rest == '\t') rest++;

    /* `echo.` / `echo:` / `echo;` / `echo,` / `echo/` / `echo\` :
     * cmd treats any punctuation glued to echo as part of the body. */
    if (nlen >= 4 && strnieq(name, "echo", 4) && nlen > 4) {
        char c = name[4];
        if (c == '.' || c == ':' || c == ';' || c == ',' || c == '/' ||
            c == '\\' || c == '+' || c == '=' || c == '[' || c == ']') {
            /* print the text after "echo<c>" plus whatever came after */
            Buf b; buf_init(&b);
            buf_puts(&b, name + 5);
            if (*rest) { buf_putc(&b, ' '); buf_puts(&b, rest); }
            printf("%s\n", b.s);
            buf_free(&b);
            return 0;
        }
    }

    if (strieq(name, "rem")) return 0;
    if (strieq(name, "echo")) return cmd_echo(rest);
    if (strieq(name, "set")) return cmd_set(rest);
    if (strieq(name, "if")) return eval_if(rest);
    if (strieq(name, "for")) return do_for(rest);
    if (strieq(name, "goto")) return cmd_goto(rest);
    if (strieq(name, "call")) return cmd_call(rest);
    if (strieq(name, "exit")) return cmd_exit(rest);
    if (strieq(name, "pushd")) return cmd_pushd(rest);
    if (strieq(name, "popd")) return cmd_popd(rest);
    if (strieq(name, "cd") || strieq(name, "chdir")) return cmd_cd(rest);
    if (strieq(name, "type")) return cmd_type(rest);
    if (strieq(name, "setlocal")) return cmd_setlocal(rest);
    if (strieq(name, "endlocal")) return cmd_endlocal(rest);
    if (strieq(name, "pause")) { (void)getchar(); return 0; }
    if (strieq(name, "cls")) { printf("\x1b[2J\x1b[H"); return 0; }
    if (strieq(name, "title")) return 0;
    if (strieq(name, "color")) return 0;
    if (strieq(name, "ver")) { printf("cmd.c fake shell\n"); return 0; }

    /* treat as implicit call */
    char **argv = NULL;
    int argn = tok_argv(rest, &argv);
    int rc = call_internal_or_external(name, argv, argn);
    free_argv(argv, argn);
    g_errorlevel = rc;
    return rc;
}

/* Peek first word of s (skipping leading ws/@) into out (lowercase, nul-term).
 * Returns pointer just after the word (and any trailing whitespace). */
static const char *peek_word(const char *s, char *out, size_t cap) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '@') { s++; while (*s == ' ' || *s == '\t') s++; }
    const char *p = s;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '(') p++;
    size_t n = (size_t)(p - s);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = lc((unsigned char)s[i]);
    out[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int execute_stmt(const char *raw) {
    if (g_frame && g_frame->returning) return 0;

    const char *s = raw;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (!*s) return 0;

    /* labels: no-op during execution (already indexed) */
    if (*s == ':' && s[1] != ':') return 0;
    if (s[0] == ':' && s[1] == ':') return 0;

    if (*s == '@') { s++; while (*s == ' ' || *s == '\t') s++; if (!*s) return 0; }

    /* leading parenthesized block: execute inner as a sequence */
    if (*s == '(') {
        const char *rest;
        char *body = peel_paren(s, &rest);
        if (body) {
            int rc = execute_seq(body);
            free(body);
            return rc;
        }
    }

    /* `rem ...` shortcut (before any expansion) */
    if ((s[0] == 'r' || s[0] == 'R') && (s[1] == 'e' || s[1] == 'E') &&
        (s[2] == 'm' || s[2] == 'M') && (s[3] == 0 || s[3] == ' ' || s[3] == '\t'))
        return 0;

    /* Peek the first word -- for `for` and `if` we keep the body raw and let
     * per-sub-command expansion happen at execute time.  For everything else
     * we expand the whole (single-line) statement. */
    char word[32];
    const char *after = peek_word(s, word, sizeof word);

    if (strcmp(word, "for") == 0) return do_for(after);
    if (strcmp(word, "if") == 0) return eval_if(after);

    char *copy = xstrdup(s);
    trim_right(copy);
    char *exp = expand(copy);
    free(copy);

    RedirSaved saved;
    apply_redir(exp, &saved);
    trim_right(exp);
    char *t = trim_left(exp);
    while (*t == ' ' || *t == '\t' || *t == '\n') t++;
    if (*t == '@') { t++; while (*t == ' ' || *t == '\t') t++; }

    int rc = 0;
    if (*t) rc = run_simple(t);

    restore_redir(&saved);
    free(exp);
    g_errorlevel = rc;
    return rc;
}

static int execute_seq(const char *raw) {
    /* split on top-level `&` and `\n`, execute each */
    char **parts = NULL;
    int n = split_seq(raw, &parts);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        rc = execute_stmt(parts[i]);
        free(parts[i]);
        if (g_frame && g_frame->returning) {
            /* free remaining parts */
            for (int j = i + 1; j < n; j++) free(parts[j]);
            break;
        }
    }
    free(parts);
    return rc;
}

/* --------------------------------------------------------------- */
/* top-level script execution                                      */
/* --------------------------------------------------------------- */

static int execute_script(Script *s, char **args, int argc) {
    Frame f;
    f.script = s;
    f.pc = 0;
    f.args = args;
    f.argc = argc;
    f.returning = 0;
    f.return_code = 0;
    f.parent = g_frame;
    Frame *saved = g_frame;
    g_frame = &f;
    while (f.pc < s->nstmts && !f.returning) {
        const char *stmt = s->stmts[f.pc];
        f.pc++;
        execute_stmt(stmt);
    }
    g_frame = saved;
    return f.return_code;
}

/* --------------------------------------------------------------- */
/* main                                                            */
/* --------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s SCRIPT.cmd [args...]\n", argv[0]);
        return 2;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    signal(SIGINT, on_sigint);
    atexit(tty_cbreak_off);

    const char *path = argv[1];
    char *resolved = find_script(path);
    if (!resolved) { fprintf(stderr, "cmd: cannot find script: %s\n", path); return 1; }

    Script *s = script_load(resolved);
    if (!s) { fprintf(stderr, "cmd: failed to load: %s\n", resolved); free(resolved); return 1; }

    /* args[0] = script name (as given), args[1..] = remaining argv */
    int nargs = 1 + (argc - 2);
    char **args = xmalloc((size_t)nargs * sizeof *args);
    args[0] = xstrdup(path);
    for (int i = 2; i < argc; i++) args[i - 1] = xstrdup(argv[i]);

    int rc = execute_script(s, args, nargs);

    for (int i = 0; i < nargs; i++) free(args[i]);
    free(args);
    script_free(s);
    free(resolved);
    tty_cbreak_off();
    return rc;
}
