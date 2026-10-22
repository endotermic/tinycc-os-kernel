/* SPDX-License-Identifier: BSD-2-Clause */
/* linker script lexer and parser (GNU ld subset) */
#include "script.h"
#include <ctype.h>

typedef struct {
    const char *src, *p, *path;
    int line;
    /* current token */
    int tok;            /* T_* or a character */
    char *text;         /* identifier / string / number text */
    uint64_t num;
} Lexer;


static void lex_error(Lexer *lx, const char *msg)
{
    fatal("%s:%d: %s (near '%s')", lx->path, lx->line, msg, lx->text ? lx->text : "");
}

static int is_sym_char(int c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '/' || c == '\\' || c == '~' ||
           c == '[' || c == ']' || c == '-' || c == '+' || c == '*' || c == '?' || c == ':' || c == '!' || c == '^';
}

static int is_sym_start(const char *p)
{
    int c = (unsigned char)p[0], n = (unsigned char)p[1];
    if (isalpha(c) || c == '_' || c == '.' || c == '$' || c == '\\') return 1;
    if (c == '/') return isalpha(n) || n == '_' || n == '.';       /* /DISCARD/, file paths */
    if (c == '*') return n == '(' || n == '.' || n == '_' || isalpha(n) || n == '*' ; /* *(...), *crt*.o */
    if (c == '[' || c == '?') return 1;
    return 0;
}

static void next_tok(Lexer *lx)
{
    const char *p = lx->p;
    free(lx->text); lx->text = NULL;
again:
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { if (*p == '\n') lx->line++; p++; }
    if (p[0] == '/' && p[1] == '*') {
        p += 2;
        while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') lx->line++; p++; }
        if (*p) p += 2;
        goto again;
    }
    if (p[0] == '#' ) { /* cpp line markers or comments */
        while (*p && *p != '\n') p++;
        goto again;
    }
    lx->p = p;
    if (!*p) { lx->tok = T_EOF; return; }
    if (*p == '"') {
        const char *s = ++p;
        while (*p && *p != '"') p++;
        lx->text = xstrndup(s, p - s);
        if (*p) p++;
        lx->tok = T_STR; lx->p = p; return;
    }
    if (isdigit((unsigned char)*p)) {
        /* number: 0x.., decimal, octal(0..)? with K/M suffix; but also things like "1.." are not expected */
        const char *s = p; char *end;
        uint64_t v;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) v = strtoull(p, &end, 16);
        else v = strtoull(p, &end, 10);
        p = end;
        if (*p == 'K' || *p == 'k') { v <<= 10; p++; }
        else if (*p == 'M' || *p == 'm') { v <<= 20; p++; }
        else if (*p == 'G' || *p == 'g') { v <<= 30; p++; }
        /* a name like 0foo? treat as identifier if symbol chars follow */
        if (is_sym_char((unsigned char)*p) && *p != '-' && *p != '+' && *p != '*' && *p != '/' && *p != '!' && *p != ':') {
            p = s;
            while (is_sym_char((unsigned char)*p)) p++;
            lx->text = xstrndup(s, p - s); lx->tok = T_IDENT; lx->p = p; return;
        }
        lx->num = v; lx->text = xstrndup(s, p - s); lx->tok = T_NUM; lx->p = p; return;
    }
    if (is_sym_start(p)) {
        const char *s = p;
        /* glob patterns may include [...] with any chars inside */
        while (is_sym_char((unsigned char)*p)) {
            if (*p == '[') { while (*p && *p != ']') p++; if (*p) p++; continue; }
            /* "/DISCARD/" and paths: allow */
            p++;
        }
        /* a trailing ':' belongs to the syntax ("name :" has a space, but ".text:" could occur) — strip it */
        if (p - s > 1 && p[-1] == ':') p--;
        lx->text = xstrndup(s, p - s); lx->tok = T_IDENT; lx->p = p; return;
    }
    /* operators */
#define TWO(a, b, t) if (p[0] == a && p[1] == b) { lx->tok = t; lx->p = p + 2; lx->text = xstrndup(p, 2); return; }
    if (p[0] == '<' && p[1] == '<' && p[2] == '=') { lx->tok = T_LSHEQ; lx->p = p + 3; lx->text = xstrdup("<<="); return; }
    if (p[0] == '>' && p[1] == '>' && p[2] == '=') { lx->tok = T_RSHEQ; lx->p = p + 3; lx->text = xstrdup(">>="); return; }
    TWO('<', '<', T_LSH) TWO('>', '>', T_RSH) TWO('<', '=', T_LE) TWO('>', '=', T_GE) TWO('=', '=', T_EQ)
    TWO('!', '=', T_NE) TWO('&', '&', T_ANDAND) TWO('|', '|', T_OROR) TWO('+', '=', T_ADDEQ) TWO('-', '=', T_SUBEQ)
    TWO('*', '=', T_MULEQ) TWO('/', '=', T_DIVEQ) TWO('&', '=', T_ANDEQ) TWO('|', '=', T_OREQ)
#undef TWO
    lx->tok = (unsigned char)*p; lx->text = xstrndup(p, 1); lx->p = p + 1;
}

static int accept(Lexer *lx, int t) { if (lx->tok == t) { next_tok(lx); return 1; } return 0; }
static void expect(Lexer *lx, int t)
{
    if (lx->tok != t) { char m[64]; snprintf(m, sizeof m, "expected '%c'", t < 256 ? t : '?'); lex_error(lx, m); }
    next_tok(lx);
}
static int accept_kw(Lexer *lx, const char *kw) { if (lx->tok == T_IDENT && !strcmp(lx->text, kw)) { next_tok(lx); return 1; } return 0; }
static int is_kw(Lexer *lx, const char *kw) { return lx->tok == T_IDENT && !strcmp(lx->text, kw); }
static char *take_ident(Lexer *lx)
{
    char *s;
    if (lx->tok != T_IDENT && lx->tok != T_STR) lex_error(lx, "expected a name");
    s = lx->text; lx->text = NULL; next_tok(lx); return s;
}

/* ---------------------------------------------------------------- expressions */
static Expr *new_expr(Lexer *lx, int kind) { Expr *e = xcalloc(1, sizeof *e); e->kind = kind; e->line = lx->line; return e; }
static Expr *parse_expr(Lexer *lx);

static int func_id(const char *s)
{
    static const struct { const char *n; int id; } tab[] = {
        { "ABSOLUTE", F_ABSOLUTE }, { "ADDR", F_ADDR }, { "LOADADDR", F_LOADADDR }, { "SIZEOF", F_SIZEOF },
        { "SIZEOF_HEADERS", F_SIZEOF_HEADERS }, { "sizeof_headers", F_SIZEOF_HEADERS }, { "ALIGN", F_ALIGN }, { "DEFINED", F_DEFINED },
        { "MAX", F_MAX }, { "MIN", F_MIN }, { "DATA_SEGMENT_ALIGN", F_DATA_SEGMENT_ALIGN },
        { "DATA_SEGMENT_END", F_DATA_SEGMENT_END }, { "DATA_SEGMENT_RELRO_END", F_DATA_SEGMENT_RELRO_END },
        { "ALIGNOF", F_ALIGNOF }, { "LENGTH", F_LENGTH }, { "ORIGIN", F_ORIGIN }, { "CONSTANT", F_CONSTANT },
        { "LOG2CEIL", F_LOG2CEIL }, { "SEGMENT_START", F_SEGMENT_START }, { "BLOCK", F_BLOCK },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].n, s)) return tab[i].id;
    return -1;
}

static Expr *parse_primary(Lexer *lx)
{
    Expr *e;
    if (lx->tok == T_NUM) { e = new_expr(lx, E_NUM); e->num = lx->num; next_tok(lx); return e; }
    if (lx->tok == '(') { next_tok(lx); e = parse_expr(lx); expect(lx, ')'); return e; }
    if (lx->tok == '-' || lx->tok == '~' || lx->tok == '!' || lx->tok == '+') {
        int op = lx->tok; next_tok(lx);
        e = new_expr(lx, E_UNARY); e->op = op; e->a = parse_primary(lx); return e;
    }
    if (lx->tok == T_IDENT) {
        int fid;
        if (!strcmp(lx->text, ".")) { next_tok(lx); return new_expr(lx, E_DOT); }
        if (!strcmp(lx->text, "ASSERT")) {
            next_tok(lx); expect(lx, '(');
            e = new_expr(lx, E_ASSERT); e->a = parse_expr(lx); expect(lx, ',');
            if (lx->tok != T_STR) lex_error(lx, "ASSERT needs a message");
            e->name = take_ident(lx); expect(lx, ')');
            return e;
        }
        fid = func_id(lx->text);
        if (fid >= 0) {
            const char *p = lx->p;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '(' || fid == F_SIZEOF_HEADERS) {
                next_tok(lx);
                e = new_expr(lx, E_FUNC); e->op = fid;
                if (fid == F_SIZEOF_HEADERS) {
                    if (accept(lx, '(')) expect(lx, ')');
                    return e;
                }
                expect(lx, '(');
                if (fid == F_ADDR || fid == F_LOADADDR || fid == F_SIZEOF || fid == F_ALIGNOF || fid == F_LENGTH || fid == F_ORIGIN) {
                    e->name = take_ident(lx);
                } else if (fid == F_DEFINED) {
                    e->name = take_ident(lx);
                } else if (fid == F_CONSTANT) {
                    e->name = take_ident(lx);
                } else if (fid == F_SEGMENT_START) {
                    e->name = take_ident(lx); expect(lx, ','); e->a = parse_expr(lx);
                } else {
                    e->a = parse_expr(lx);
                    if (accept(lx, ',')) e->b = parse_expr(lx);
                    if (fid == F_DATA_SEGMENT_RELRO_END || fid == F_DATA_SEGMENT_ALIGN) { /* 2 args */ }
                }
                expect(lx, ')');
                return e;
            }
        }
        e = new_expr(lx, E_SYM); e->name = take_ident(lx); return e;
    }
    lex_error(lx, "bad expression");
    return NULL;
}

static int binprec(int t)
{
    switch (t) {
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case T_LSH: case T_RSH: return 8;
    case '<': case '>': case T_LE: case T_GE: return 7;
    case T_EQ: case T_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    case T_ANDAND: return 2;
    case T_OROR: return 1;
    }
    return 0;
}

static Expr *parse_binary(Lexer *lx, int minprec)
{
    Expr *l = parse_primary(lx);
    for (;;) {
        int prec = binprec(lx->tok), op;
        Expr *r, *e;
        if (!prec || prec < minprec) return l;
        op = lx->tok; next_tok(lx);
        r = parse_binary(lx, prec + 1);
        e = new_expr(lx, E_BINARY); e->op = op; e->a = l; e->b = r;
        l = e;
    }
}

static Expr *parse_expr(Lexer *lx)
{
    Expr *c = parse_binary(lx, 1);
    if (lx->tok == '?') {
        Expr *e = new_expr(lx, E_TERNARY);
        next_tok(lx);
        e->a = c; e->b = parse_expr(lx); expect(lx, ':'); e->c = parse_expr(lx);
        return e;
    }
    return c;
}

/* ---------------------------------------------------------------- statements */
static Stmt *new_stmt(Lexer *lx, int kind) { Stmt *s = xcalloc(1, sizeof *s); s->kind = kind; s->line = lx->line; return s; }

static int assign_op(int t)
{
    switch (t) {
    case '=': return OP_SET; case T_ADDEQ: return OP_ADD; case T_SUBEQ: return OP_SUB; case T_MULEQ: return OP_MUL;
    case T_DIVEQ: return OP_DIV; case T_LSHEQ: return OP_LSH; case T_RSHEQ: return OP_RSH; case T_ANDEQ: return OP_AND; case T_OREQ: return OP_OR;
    }
    return -1;
}

/* name [op]= expr ; */
static Stmt *parse_assignment(Lexer *lx, char *name, int provide, int hidden)
{
    Stmt *s = new_stmt(lx, S_ASSIGN);
    int op = assign_op(lx->tok);
    if (op < 0) lex_error(lx, "expected '='");
    next_tok(lx);
    s->name = name; s->op = op; s->provide = provide; s->hidden = hidden;
    s->expr = parse_expr(lx);
    if (!strcmp(name, ".")) s->kind = S_DOT_ASSIGN;
    return s;
}

static Stmt *parse_provide(Lexer *lx, int hidden)
{
    Stmt *s;
    char *name;
    expect(lx, '(');
    name = take_ident(lx);
    s = parse_assignment(lx, name, 1, hidden);
    expect(lx, ')');
    return s;
}

/* input section description after the file pattern:  "( patterns )" */
static InputSpec *parse_input_spec(Lexer *lx, char *filepat, int keep)
{
    InputSpec *sp = xcalloc(1, sizeof *sp);
    PArr pats = { 0 };
    sp->filepat = filepat; sp->keep = keep;
    expect(lx, '(');
    while (lx->tok != ')') {
        int sort = 0;
        if (is_kw(lx, "SORT") || is_kw(lx, "SORT_BY_NAME")) sort = 1;
        else if (is_kw(lx, "SORT_BY_ALIGNMENT")) sort = 2;
        else if (is_kw(lx, "SORT_BY_INIT_PRIORITY")) sort = 3;
        else if (is_kw(lx, "SORT_NONE")) sort = 4;
        if (sort) {
            int depth = 1;
            next_tok(lx); expect(lx, '(');
            /* nested SORT(SORT_BY_ALIGNMENT(..)) just take innermost names */
            while (lx->tok == T_IDENT && (is_kw(lx, "SORT") || is_kw(lx, "SORT_BY_NAME") || is_kw(lx, "SORT_BY_ALIGNMENT") || is_kw(lx, "SORT_BY_INIT_PRIORITY"))) {
                next_tok(lx); expect(lx, '('); depth++;
            }
            while (lx->tok != ')') parr_add(&pats, take_ident(lx));
            while (depth-- > 0) expect(lx, ')');
            if (sort != 4) sp->sort = sort;
            continue;
        }
        if (is_kw(lx, "EXCLUDE_FILE")) {
            next_tok(lx); expect(lx, '(');
            while (lx->tok != ')') { free(take_ident(lx)); }
            expect(lx, ')');
            warning("EXCLUDE_FILE is ignored");
            continue;
        }
        if (is_kw(lx, "COMMON")) { sp->common = 1; next_tok(lx); continue; }
        parr_add(&pats, take_ident(lx));
        accept(lx, ',');
    }
    expect(lx, ')');
    sp->secpats = (char **)pats.p; sp->nsecpats = pats.n;
    return sp;
}

static int is_datacmd(Lexer *lx)
{
    return is_kw(lx, "BYTE") || is_kw(lx, "SHORT") || is_kw(lx, "LONG") || is_kw(lx, "QUAD") || is_kw(lx, "SQUAD");
}

static Stmt *parse_outsec_body(Lexer *lx, OutSecDef *os)
{
    Stmt *head = NULL, **tail = &head;
    expect(lx, '{');
    while (lx->tok != '}') {
        Stmt *s = NULL;
        if (lx->tok == ';') { next_tok(lx); continue; }
        if (is_kw(lx, "PROVIDE")) { next_tok(lx); s = parse_provide(lx, 0); accept(lx, ';'); }
        else if (is_kw(lx, "PROVIDE_HIDDEN")) { next_tok(lx); s = parse_provide(lx, 1); accept(lx, ';'); }
        else if (is_kw(lx, "HIDDEN")) { next_tok(lx); expect(lx, '('); { char *n = take_ident(lx); s = parse_assignment(lx, n, 0, 1); } expect(lx, ')'); accept(lx, ';'); }
        else if (is_kw(lx, "ASSERT")) {
            next_tok(lx); expect(lx, '(');
            s = new_stmt(lx, S_ASSERT); s->expr = parse_expr(lx); expect(lx, ',');
            s->name = take_ident(lx); expect(lx, ')'); accept(lx, ';');
        }
        else if (is_kw(lx, "KEEP")) {
            char *fp;
            next_tok(lx); expect(lx, '(');
            fp = take_ident(lx);
            s = new_stmt(lx, S_INPUT); s->spec = parse_input_spec(lx, fp, 1);
            expect(lx, ')');
        }
        else if (is_kw(lx, "INPUT_SECTION_FLAGS")) {
            char *fp;
            next_tok(lx); expect(lx, '('); while (lx->tok != ')') next_tok(lx); expect(lx, ')');
            fp = take_ident(lx);
            s = new_stmt(lx, S_INPUT); s->spec = parse_input_spec(lx, fp, 0);
        }
        else if (is_datacmd(lx)) {
            s = new_stmt(lx, S_DATA);
            s->size = is_kw(lx, "BYTE") ? 1 : is_kw(lx, "SHORT") ? 2 : is_kw(lx, "LONG") ? 4 : 8;
            next_tok(lx); expect(lx, '('); s->expr = parse_expr(lx); expect(lx, ')'); accept(lx, ';');
        }
        else if (is_kw(lx, "FILL")) {
            s = new_stmt(lx, S_FILL); next_tok(lx); expect(lx, '('); s->expr = parse_expr(lx); expect(lx, ')'); accept(lx, ';');
        }
        else if (is_kw(lx, "CONSTRUCTORS")) { next_tok(lx); accept(lx, ';'); continue; }
        else if (is_kw(lx, "SORT") || is_kw(lx, "SORT_BY_NAME") || is_kw(lx, "SORT_BY_ALIGNMENT")) {
            /* SORT(file pattern)(sections) — treat the file pattern as * */
            next_tok(lx); expect(lx, '('); free(take_ident(lx)); expect(lx, ')');
            s = new_stmt(lx, S_INPUT); s->spec = parse_input_spec(lx, xstrdup("*"), 0);
        }
        else if (lx->tok == T_IDENT || lx->tok == T_STR) {
            char *name = take_ident(lx);
            if (lx->tok == '(') { s = new_stmt(lx, S_INPUT); s->spec = parse_input_spec(lx, name, 0); }
            else if (assign_op(lx->tok) >= 0) { s = parse_assignment(lx, name, 0, 0); accept(lx, ';'); }
            else lex_error(lx, "unexpected token in output section");
        }
        else lex_error(lx, "unexpected token in output section body");
        if (s) { *tail = s; tail = &s->next; }
    }
    expect(lx, '}');
    return head;
}

static Stmt *parse_outsec(Lexer *lx, char *name)
{
    OutSecDef *os = xcalloc(1, sizeof *os);
    Stmt *s = new_stmt(lx, S_OUTSEC);
    PArr ph = { 0 };
    s->os = os;
    os->name = name;
    if (!strcmp(name, "/DISCARD/")) os->discard = 1;
    /* [address] [(type)] : [AT(x)] [ALIGN(x)] [SUBALIGN(x)] */
    if (lx->tok != ':' && lx->tok != '(') os->addr = parse_expr(lx);
    if (lx->tok == '(') {
        next_tok(lx);
        if (accept_kw(lx, "NOLOAD")) os->sectype = OST_NOLOAD;
        else if (accept_kw(lx, "INFO")) os->sectype = OST_INFO;
        else if (accept_kw(lx, "DSECT")) os->sectype = OST_DSECT;
        else if (accept_kw(lx, "COPY")) os->sectype = OST_COPY;
        else if (accept_kw(lx, "OVERLAY")) os->sectype = OST_OVERLAY;
        else if (accept_kw(lx, "READONLY")) { if (lx->tok == '(') { next_tok(lx); accept_kw(lx, "NOLOAD"); expect(lx, ')'); } }
        else if (accept_kw(lx, "TYPE")) { expect(lx, '='); parse_expr(lx); }
        else lex_error(lx, "bad section type");
        expect(lx, ')');
    }
    expect(lx, ':');
    for (;;) {
        if (is_kw(lx, "AT")) { next_tok(lx); expect(lx, '('); os->at = parse_expr(lx); expect(lx, ')'); }
        else if (is_kw(lx, "ALIGN")) { next_tok(lx); expect(lx, '('); os->align = parse_expr(lx); expect(lx, ')'); }
        else if (is_kw(lx, "ALIGN_WITH_INPUT")) { next_tok(lx); }
        else if (is_kw(lx, "SUBALIGN")) { next_tok(lx); expect(lx, '('); os->subalign = parse_expr(lx); expect(lx, ')'); }
        else if (is_kw(lx, "ONLY_IF_RO") || is_kw(lx, "ONLY_IF_RW")) { next_tok(lx); }
        else break;
    }
    os->children = parse_outsec_body(lx, os);
    /* trailing: >region AT>region :phdr ... =fill , */
    for (;;) {
        if (lx->tok == '>') { next_tok(lx); os->region = take_ident(lx); }
        else if (is_kw(lx, "AT")) { next_tok(lx); expect(lx, '>'); os->lma_region = take_ident(lx); }
        else if (lx->tok == ':') { next_tok(lx); parr_add(&ph, take_ident(lx)); }
        else if (lx->tok == '=') { next_tok(lx); os->fill = parse_expr(lx); }
        else if (lx->tok == ',') { next_tok(lx); }
        else break;
    }
    os->phdrs = (char **)ph.p; os->nphdrs = ph.n;
    return s;
}

static void parse_sections(Lexer *lx, Script *sc, Stmt ***tail)
{
    expect(lx, '{');
    sc->has_sections = 1;
    while (lx->tok != '}') {
        Stmt *s = NULL;
        if (lx->tok == ';') { next_tok(lx); continue; }
        if (is_kw(lx, "PROVIDE")) { next_tok(lx); s = parse_provide(lx, 0); accept(lx, ';'); }
        else if (is_kw(lx, "PROVIDE_HIDDEN")) { next_tok(lx); s = parse_provide(lx, 1); accept(lx, ';'); }
        else if (is_kw(lx, "HIDDEN")) { next_tok(lx); expect(lx, '('); { char *n = take_ident(lx); s = parse_assignment(lx, n, 0, 1); } expect(lx, ')'); accept(lx, ';'); }
        else if (is_kw(lx, "ASSERT")) {
            next_tok(lx); expect(lx, '(');
            s = new_stmt(lx, S_ASSERT); s->expr = parse_expr(lx); expect(lx, ',');
            s->name = take_ident(lx); expect(lx, ')'); accept(lx, ';');
        }
        else if (is_kw(lx, "ENTRY")) { next_tok(lx); expect(lx, '('); sc->entry = take_ident(lx); expect(lx, ')'); accept(lx, ';'); continue; }
        else if (is_kw(lx, "INCLUDE")) { next_tok(lx); { char *f = take_ident(lx); script_parse_into(sc, f); free(f); } continue; }
        else if (lx->tok == T_IDENT || lx->tok == T_STR) {
            char *name = take_ident(lx);
            if (assign_op(lx->tok) >= 0) { s = parse_assignment(lx, name, 0, 0); accept(lx, ';'); }
            else s = parse_outsec(lx, name);
        }
        else lex_error(lx, "unexpected token in SECTIONS");
        if (s) { **tail = s; *tail = &s->next; }
    }
    expect(lx, '}');
}

static void parse_phdrs(Lexer *lx, Script *sc)
{
    expect(lx, '{');
    while (lx->tok != '}') {
        PhdrDef *ph = xcalloc(1, sizeof *ph);
        ph->name = take_ident(lx);
        if (lx->tok == T_NUM) { ph->type = (uint32_t)lx->num; next_tok(lx); }
        else {
            char *t = take_ident(lx);
            static const struct { const char *n; uint32_t v; } tt[] = {
                { "PT_NULL", PT_NULL }, { "PT_LOAD", PT_LOAD }, { "PT_DYNAMIC", PT_DYNAMIC }, { "PT_INTERP", PT_INTERP },
                { "PT_NOTE", PT_NOTE }, { "PT_SHLIB", PT_SHLIB }, { "PT_PHDR", PT_PHDR }, { "PT_TLS", PT_TLS },
                { "PT_GNU_EH_FRAME", 0x6474e550 }, { "PT_GNU_STACK", 0x6474e551 }, { "PT_GNU_RELRO", 0x6474e552 }, { "PT_GNU_PROPERTY", 0x6474e553 },
            };
            size_t i;
            for (i = 0; i < sizeof tt / sizeof tt[0]; i++) if (!strcmp(tt[i].n, t)) break;
            if (i == sizeof tt / sizeof tt[0]) lex_error(lx, "unknown program header type");
            ph->type = tt[i].v;
            free(t);
        }
        for (;;) {
            if (accept_kw(lx, "FILEHDR")) ph->filehdr = 1;
            else if (accept_kw(lx, "PHDRS")) ph->phdrs = 1;
            else if (is_kw(lx, "FLAGS")) { next_tok(lx); expect(lx, '('); { Expr *e = parse_expr(lx); if (e->kind != E_NUM) lex_error(lx, "FLAGS needs a number"); ph->has_flags = 1; ph->flags = (uint32_t)e->num; } expect(lx, ')'); }
            else if (is_kw(lx, "AT")) { next_tok(lx); expect(lx, '('); ph->at = parse_expr(lx); expect(lx, ')'); }
            else break;
        }
        expect(lx, ';');
        sc->phdrs = xrealloc(sc->phdrs, (sc->nphdrs + 1) * sizeof *sc->phdrs);
        sc->phdrs[sc->nphdrs++] = ph;
    }
    expect(lx, '}');
}

static void parse_version(Lexer *lx, Script *sc)
{
    expect(lx, '{');
    while (lx->tok != '}') {
        VersionNode *vn = xcalloc(1, sizeof *vn);
        PArr g = { 0 }, l = { 0 }, *cur = &g;
        if (lx->tok == T_IDENT) vn->name = take_ident(lx);
        expect(lx, '{');
        while (lx->tok != '}') {
            if (is_kw(lx, "global")) { next_tok(lx); expect(lx, ':'); cur = &g; continue; }
            if (is_kw(lx, "local")) { next_tok(lx); expect(lx, ':'); cur = &l; continue; }
            if (is_kw(lx, "extern")) { /* extern "C++" { } */ next_tok(lx); free(take_ident(lx)); expect(lx, '{'); while (lx->tok != '}') { parr_add(cur, take_ident(lx)); accept(lx, ';'); } expect(lx, '}'); accept(lx, ';'); continue; }
            if (lx->tok == '*') { parr_add(cur, xstrdup("*")); next_tok(lx); }
            else parr_add(cur, take_ident(lx));
            accept(lx, ';');
        }
        expect(lx, '}');
        if (lx->tok == T_IDENT) vn->parent = take_ident(lx);
        expect(lx, ';');
        vn->globals = (char **)g.p; vn->nglobals = g.n; vn->locals = (char **)l.p; vn->nlocals = l.n;
        sc->versions = xrealloc(sc->versions, (sc->nversions + 1) * sizeof *sc->versions);
        sc->versions[sc->nversions++] = vn;
    }
    expect(lx, '}');
}

Script *script_new(void) { return xcalloc(1, sizeof(Script)); }

void script_parse_into(Script *sc, const char *path)
{
    Lexer lx;
    size_t size;
    uint8_t *src = read_file(path, &size);
    Stmt **tail = &sc->stmts;
    while (*tail) tail = &(*tail)->next;
    src = xrealloc(src, size + 1); src[size] = 0;
    memset(&lx, 0, sizeof lx);
    lx.src = lx.p = (char *)src; lx.path = path; lx.line = 1;
    next_tok(&lx);
    while (lx.tok != T_EOF) {
        if (lx.tok == ';') { next_tok(&lx); continue; }
        if (is_kw(&lx, "SECTIONS")) { next_tok(&lx); parse_sections(&lx, sc, &tail); }
        else if (is_kw(&lx, "PHDRS")) { next_tok(&lx); parse_phdrs(&lx, sc); }
        else if (is_kw(&lx, "VERSION")) { next_tok(&lx); parse_version(&lx, sc); }
        else if (is_kw(&lx, "ENTRY")) { next_tok(&lx); expect(&lx, '('); sc->entry = take_ident(&lx); expect(&lx, ')'); accept(&lx, ';'); }
        else if (is_kw(&lx, "OUTPUT_FORMAT")) { next_tok(&lx); expect(&lx, '('); sc->output_format = take_ident(&lx); while (lx.tok == ',') { next_tok(&lx); free(take_ident(&lx)); } expect(&lx, ')'); accept(&lx, ';'); }
        else if (is_kw(&lx, "OUTPUT_ARCH") || is_kw(&lx, "OUTPUT") || is_kw(&lx, "TARGET") || is_kw(&lx, "SEARCH_DIR") || is_kw(&lx, "STARTUP")) {
            next_tok(&lx); expect(&lx, '('); while (lx.tok != ')') next_tok(&lx); expect(&lx, ')'); accept(&lx, ';');
        }
        else if (is_kw(&lx, "INPUT") || is_kw(&lx, "GROUP")) {
            next_tok(&lx); expect(&lx, '(');
            while (lx.tok != ')') { if (lx.tok == ',') { next_tok(&lx); continue; } sc->inputs = xrealloc(sc->inputs, (sc->ninputs + 1) * sizeof(char *)); sc->inputs[sc->ninputs++] = take_ident(&lx); }
            expect(&lx, ')'); accept(&lx, ';');
        }
        else if (is_kw(&lx, "INCLUDE")) { next_tok(&lx); { char *f = take_ident(&lx); script_parse_into(sc, f); free(f); } }
        else if (is_kw(&lx, "MEMORY") || is_kw(&lx, "REGION_ALIAS") || is_kw(&lx, "INSERT") || is_kw(&lx, "NOCROSSREFS") || is_kw(&lx, "EXTERN") || is_kw(&lx, "FORCE_COMMON_ALLOCATION") || is_kw(&lx, "INHIBIT_COMMON_ALLOCATION")) {
            lex_error(&lx, "unsupported linker script command");
        }
        else if (is_kw(&lx, "PROVIDE")) { next_tok(&lx); *tail = parse_provide(&lx, 0); tail = &(*tail)->next; accept(&lx, ';'); }
        else if (is_kw(&lx, "PROVIDE_HIDDEN")) { next_tok(&lx); *tail = parse_provide(&lx, 1); tail = &(*tail)->next; accept(&lx, ';'); }
        else if (is_kw(&lx, "ASSERT")) {
            Stmt *s;
            next_tok(&lx); expect(&lx, '(');
            s = new_stmt(&lx, S_ASSERT); s->expr = parse_expr(&lx); expect(&lx, ',');
            s->name = take_ident(&lx); expect(&lx, ')'); accept(&lx, ';');
            *tail = s; tail = &s->next;
        }
        else if (lx.tok == T_IDENT) {
            char *name = take_ident(&lx);
            if (assign_op(lx.tok) < 0) lex_error(&lx, "unexpected token at top level");
            *tail = parse_assignment(&lx, name, 0, 0); tail = &(*tail)->next; accept(&lx, ';');
        }
        else lex_error(&lx, "unexpected token at top level");
    }
    free(lx.text);
    free(src);
}

Script *script_parse_file(const char *path)
{
    Script *sc = script_new();
    script_parse_into(sc, path);
    return sc;
}
