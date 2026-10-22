/* SPDX-License-Identifier: BSD-2-Clause */
/* linker script AST */
#ifndef SCRIPT_H
#define SCRIPT_H
#include "tld.h"

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct OutSecDef OutSecDef;
typedef struct InputSpec InputSpec;
typedef struct Script Script;
typedef struct PhdrDef PhdrDef;

/* lexer tokens (binary operators in Expr.op use these or plain characters) */
enum { T_EOF = 256, T_IDENT, T_NUM, T_STR, T_LSH, T_RSH, T_LE, T_GE, T_EQ, T_NE, T_ANDAND, T_OROR,
       T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_LSHEQ, T_RSHEQ, T_ANDEQ, T_OREQ };

enum { E_NUM, E_SYM, E_DOT, E_UNARY, E_BINARY, E_TERNARY, E_FUNC, E_ASSERT };
enum { F_ABSOLUTE, F_ADDR, F_LOADADDR, F_SIZEOF, F_SIZEOF_HEADERS, F_ALIGN, F_DEFINED, F_MAX, F_MIN,
       F_DATA_SEGMENT_ALIGN, F_DATA_SEGMENT_END, F_DATA_SEGMENT_RELRO_END, F_ALIGNOF, F_LENGTH, F_ORIGIN,
       F_CONSTANT, F_LOG2CEIL, F_SEGMENT_START, F_BLOCK };

struct Expr {
    int kind;
    int op;                 /* unary/binary operator token, or function id */
    uint64_t num;
    char *name;             /* E_SYM / section name for ADDR etc. / ASSERT message */
    Expr *a, *b, *c;        /* operands; for E_FUNC: a, b (args) */
    int line;
};

enum { S_ASSIGN, S_DOT_ASSIGN, S_OUTSEC, S_ASSERT, S_INPUT, S_DATA, S_FILL, S_ENTRY, S_NOP };
enum { OP_SET = 0, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_LSH, OP_RSH, OP_AND, OP_OR };

struct InputSpec {
    char *filepat;          /* file name pattern, "*" normally */
    int keep;
    int sort;               /* 0 none, 1 by name, 2 by alignment, 3 by init priority */
    char **secpats; int nsecpats;
    int common;             /* *(COMMON) */
    int kind_filter, kind_nobits, kind_alloc;  /* internal: match only this section kind */
    int file_excl;          /* EXCLUDE_FILE not supported, kept 0 */
};

struct Stmt {
    int kind;
    int line;
    /* S_ASSIGN */
    char *name; int op; Expr *expr; int provide, hidden;
    /* S_OUTSEC */
    OutSecDef *os;
    /* S_INPUT */
    InputSpec *spec;
    /* S_DATA: size 1/2/4/8 and expr; S_FILL: expr */
    int size;
    /* S_ASSERT: expr + name (message) */
    Stmt *next;
};

enum { OST_NORMAL, OST_NOLOAD, OST_INFO, OST_DSECT, OST_COPY, OST_OVERLAY };

struct OutSecDef {
    char *name;
    Expr *addr;             /* NULL: use dot */
    int sectype;
    Expr *at;               /* AT(expr) or NULL */
    Expr *align;            /* ALIGN(expr) attribute */
    Expr *subalign;
    Stmt *children;
    char **phdrs; int nphdrs;
    Expr *fill;             /* =fill */
    int discard;            /* /DISCARD/ */
    char *region, *lma_region;
};

struct PhdrDef {
    char *name; uint32_t type; int has_flags; uint32_t flags; int filehdr, phdrs; Expr *at;
};

typedef struct VersionNode {
    char *name;
    char **globals; int nglobals;   /* patterns */
    char **locals; int nlocals;
    char *parent;
    uint32_t dynstr_off;
} VersionNode;

struct Script {
    Stmt *stmts;            /* top-level statements (assignments, SECTIONS children flattened, ASSERTs) */
    int has_sections;
    PhdrDef **phdrs; int nphdrs;
    char *entry;
    VersionNode **versions; int nversions;
    char **inputs; int ninputs;  /* INPUT()/GROUP() files */
    char *output_format;
};

Script *script_parse_file(const char *path);
void script_parse_into(Script *sc, const char *path);
Script *script_new(void);

#endif
