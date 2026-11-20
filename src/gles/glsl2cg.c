// glsl2cg.c - GLSL ES 1.00/3.00 -> psp2 Cg translator (see glsl2cg.h)
// pipeline: lex -> small preprocessor -> recursive descent parser that types
// every expression and writes Cg text as it goes -> wrap stage IO in a
// generated main(). everything lives in a heap ctx, errors longjmp out.

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glsl2cg.h"

#if defined(__GNUC__)
#define G2C_NORET __attribute__((noreturn))
#define G2C_FMT(a, b) __attribute__((format(printf, a, b)))
#define G2C_NOINLINE __attribute__((noinline))
#else
#define G2C_NORET
#define G2C_FMT(a, b)
#define G2C_NOINLINE
#endif

#define MAX_EDEPTH 64      // expression nesting
#define MAX_SDEPTH 64      // statement nesting
#define MAX_ARGS 16
#define MAX_IO 32
#define MAX_TEXCOORD 10
#define MAX_MEMBERS 64
#define MAX_MACRO_PARAMS 32
#define MAX_IF_DEPTH 64
#define CHUNK_SIZE 65536

// ---- basic containers ----

typedef struct chunk { struct chunk *next; size_t used, cap; } chunk;
#define CHDR ((sizeof(chunk) + 15) & ~(size_t)15)
typedef struct { chunk *head; } arena;
typedef struct { chunk *c; size_t used; } amark;
typedef struct { char *p; size_t n, cap; } sb;

enum { TK_EOF, TK_ID, TK_INT, TK_FLT, TK_OP };
typedef struct { const char *s; int line; unsigned char k, bol, ws; } tok;
typedef struct { tok *v; int n, cap; } tlist;

typedef struct macro {
  struct macro *next;
  const char *name;
  const char **par;
  int np;  // -1 = object-like
  tok *body;
  int nb;
} macro;

enum { B_VOID, B_FLOAT, B_INT, B_BOOL, B_S2D, B_SCUBE, B_STRUCT };
// r = vector size (matrix rows), c = matrix columns (1 if not a matrix)
// arr = array size (0 = not an array, -1 = unsized while parsing)
typedef struct { unsigned char b, r, c; short arr, st; } gty;

typedef struct { const char *name, *cg; gty t; } member;
typedef struct { const char *name, *cg; member *m; int nm; } sdef;

enum { K_VAR, K_UNIFORM, K_IN, K_OUT };
enum { ST_NATIVE, ST_SLOT, ST_MAT, ST_MATARR };  // uniform storage
typedef struct sym {
  struct sym *next;
  const char *name, *cg;
  gty t;
  int depth;
  unsigned char kind, store, is_const, has_ival;
  long long ival;
} sym;

typedef struct fn {
  struct fn *next;
  const char *name, *cg;
  gty ret;
  int np, defined;
  gty *pt;
  unsigned char *pq;  // 0 in, 1 out, 2 inout
} fn;

typedef struct { const char *name, *cg; gty t; int loc, tc; } iovar;

enum {
  U_POS = 1, U_PSIZE = 2, U_IID = 4, U_VID = 8, U_FCOORD = 16, U_FACE = 32,
  U_PCOORD = 64, U_FCOLOR = 128, U_FDATA = 256, U_FDEPTH = 512
};

typedef struct {
  int vs;
  sb helpers, body;
  iovar in[MAX_IO], out[MAX_IO];  // vs: attribs / varyings, fs: varyings / color out
  int nin, nout;
  unsigned used;
  int have_main;
} stage;

typedef struct ctx {
  jmp_buf jb;
  char err[256];
  unsigned flags;
  arena perm, tmp;
  g2c_result *res;
  stage st[2];
  stage *S;
  sb scratch, fin[2];
  macro *macros;
  int pp_line;
  tok *tk;
  int ntk, p;
  sym *syms;
  int depth;
  sdef *structs;
  int nstructs, capstructs;
  fn *fns;
  const char **fnames;
  int nfnames, capfnames;
  const char **hkeys;
  int nhkeys, caphkeys;
  int in_fn;
  size_t fn_start;
  gty fn_ret;
  int indent, edepth, sdepth, uniq;
} ctx;

// pending forms for values that need fixing up before use (uniform slots etc)
enum { PN_NONE, PN_STORE, PN_MATU, PN_MATEL, PN_ARR, PN_INIT };
typedef struct {
  const char *s;
  gty t;
  unsigned char pn, raw4, cast, has_ival, lval;
  const sym *u;
  const char *idx;
  long long ival;
} ex;

#define STRICT (c->flags & G2C_STRICT)

// ---- errors / memory ----

static void fail(ctx *c, const char *fmt, ...) G2C_NORET G2C_FMT(2, 3);
static void fail(ctx *c, const char *fmt, ...) {
  int n = 0;
  if (c->S) n = snprintf(c->err, sizeof c->err, "%s: ", c->S->vs ? "vs" : "fs");
  if (c->tk && c->ntk) {
    int i = c->p < c->ntk ? c->p : c->ntk - 1;
    n += snprintf(c->err + n, sizeof c->err - n, "line %d: ", c->tk[i].line);
  } else if (c->pp_line) {
    n += snprintf(c->err + n, sizeof c->err - n, "line %d: ", c->pp_line);
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->err + n, sizeof c->err - n, fmt, ap);
  va_end(ap);
  longjmp(c->jb, 1);
}

static void *amalloc(ctx *c, arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  chunk *h = a->head;
  if (!h || h->used + n > h->cap) {
    size_t cap = n > CHUNK_SIZE ? n : CHUNK_SIZE;
    chunk *nc = malloc(CHDR + cap);
    if (!nc) fail(c, "out of memory");
    nc->next = h;
    nc->used = 0;
    nc->cap = cap;
    a->head = h = nc;
  }
  void *p = (char *)h + CHDR + h->used;
  h->used += n;
  return p;
}

static amark amark_get(arena *a) {
  amark m;
  m.c = a->head;
  m.used = a->head ? a->head->used : 0;
  return m;
}

static void amark_release(arena *a, amark m) {
  while (a->head != m.c) {
    chunk *n = a->head->next;
    free(a->head);
    a->head = n;
  }
  if (a->head) a->head->used = m.used;
}

static void arena_free(arena *a) {
  while (a->head) {
    chunk *n = a->head->next;
    free(a->head);
    a->head = n;
  }
}

static char *astrndup(ctx *c, arena *a, const char *s, size_t n) {
  char *d = amalloc(c, a, n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

static char *aprintf(ctx *c, arena *a, const char *fmt, ...) G2C_FMT(3, 4);
static char *aprintf(ctx *c, arena *a, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) fail(c, "format error");
  char *s = amalloc(c, a, (size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return s;
}

#define TS(...) aprintf(c, &c->tmp, __VA_ARGS__)
#define PS(...) aprintf(c, &c->perm, __VA_ARGS__)

static void sb_reserve(ctx *c, sb *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap * 2 : 4096;
  while (cap < b->n + extra + 1) cap *= 2;
  char *np = realloc(b->p, cap);
  if (!np) fail(c, "out of memory");
  b->p = np;
  b->cap = cap;
}

static void sb_putn(ctx *c, sb *b, const char *s, size_t n) {
  sb_reserve(c, b, n);
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void sb_puts(ctx *c, sb *b, const char *s) { sb_putn(c, b, s, strlen(s)); }

static void sb_printf(ctx *c, sb *b, const char *fmt, ...) G2C_FMT(3, 4);
static void sb_printf(ctx *c, sb *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) fail(c, "format error");
  sb_reserve(c, b, (size_t)n);
  vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->n += (size_t)n;
}

static void sb_insert(ctx *c, sb *b, size_t at, const char *s) {
  size_t n = strlen(s);
  sb_reserve(c, b, n);
  memmove(b->p + at + n, b->p + at, b->n - at + 1);
  memcpy(b->p + at, s, n);
  b->n += n;
}

static void sb_clear(sb *b) {
  b->n = 0;
  if (b->p) b->p[0] = 0;
}

static void sb_free(sb *b) {
  free(b->p);
  b->p = NULL;
  b->n = b->cap = 0;
}

// grow a pointer array living in the perm arena
static void push_name(ctx *c, const char ***v, int *n, int *cap, const char *s) {
  if (*n == *cap) {
    int nc = *cap ? *cap * 2 : 32;
    const char **nv = amalloc(c, &c->perm, sizeof(char *) * (size_t)nc);
    if (*n) memcpy(nv, *v, sizeof(char *) * (size_t)*n);
    *v = nv;
    *cap = nc;
  }
  (*v)[(*n)++] = s;
}

// ---- lexer ----

static int is_alpha(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int is_digit(int ch) { return ch >= '0' && ch <= '9'; }
static int is_alnum(int ch) { return is_alpha(ch) || is_digit(ch); }
static int is_xdigit(int ch) { return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

static const char *const ops3[] = { "<<=", ">>=", NULL };
static const char *const ops2[] = {
  "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", NULL
};
static const char *const ops1[] = {
  "+", "-", "*", "/", "%", "<", ">", "=", "!", "~", "&", "|", "^", "?",
  ":", ";", ",", ".", "(", ")", "{", "}", "[", "]", "#", NULL
};

static void tl_push(ctx *c, tlist *l, tok t) {
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 256;
    tok *nv = amalloc(c, &c->perm, sizeof(tok) * (size_t)cap);
    if (l->n) memcpy(nv, l->v, sizeof(tok) * (size_t)l->n);
    l->v = nv;
    l->cap = cap;
  }
  l->v[l->n++] = t;
}

static int isop(const tok *t, const char *s) { return t->k == TK_OP && !strcmp(t->s, s); }
static int isid(const tok *t, const char *s) { return t->k == TK_ID && !strcmp(t->s, s); }

static void lex(ctx *c, const char *src, tlist *out) {
  const unsigned char *p = (const unsigned char *)src;
  int line = 1, bol = 1, ws = 1;
  for (;;) {
    int ch = *p;
    if (!ch) break;
    if (ch == '\n') { line++; bol = 1; ws = 1; p++; continue; }
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') { ws = 1; p++; continue; }
    if (ch == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
      p += p[1] == '\n' ? 2 : 3;
      line++;
      ws = 1;
      continue;
    }
    if (ch == '/' && p[1] == '/') {
      while (*p && *p != '\n') p++;
      ws = 1;
      continue;
    }
    if (ch == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/')) {
        if (*p == '\n') line++;
        p++;
      }
      if (!*p) { c->pp_line = line; fail(c, "unterminated comment"); }
      p += 2;
      ws = 1;
      continue;
    }
    tok t;
    t.line = line;
    t.bol = (unsigned char)bol;
    t.ws = (unsigned char)ws;
    bol = ws = 0;
    const unsigned char *s = p;
    if (is_alpha(ch)) {
      while (is_alnum(*p)) p++;
      t.k = TK_ID;
    } else if (is_digit(ch) || (ch == '.' && is_digit(p[1]))) {
      int flt = 0;
      if (ch == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (is_xdigit(*p)) p++;
      } else {
        while (is_digit(*p)) p++;
        if (*p == '.') {
          flt = 1;
          p++;
          while (is_digit(*p)) p++;
        }
        if (*p == 'e' || *p == 'E') {
          const unsigned char *q = p + 1;
          if (*q == '+' || *q == '-') q++;
          if (is_digit(*q)) {
            flt = 1;
            p = q;
            while (is_digit(*p)) p++;
          }
        }
        if (*p == 'f' || *p == 'F') { flt = 1; p++; }
      }
      if (!flt && (*p == 'u' || *p == 'U')) p++;
      if (is_alnum(*p)) { c->pp_line = line; fail(c, "bad number"); }
      t.k = flt ? TK_FLT : TK_INT;
    } else {
      const char *op = NULL;
      for (int i = 0; !op && ops3[i]; i++) if (!strncmp((const char *)p, ops3[i], 3)) op = ops3[i];
      for (int i = 0; !op && ops2[i]; i++) if (!strncmp((const char *)p, ops2[i], 2)) op = ops2[i];
      for (int i = 0; !op && ops1[i]; i++) if (*p == (unsigned char)ops1[i][0]) op = ops1[i];
      if (!op) { c->pp_line = line; fail(c, "unexpected character '%c'", ch); }
      t.k = TK_OP;
      t.s = op;
      p += strlen(op);
      tl_push(c, out, t);
      continue;
    }
    t.s = astrndup(c, &c->perm, (const char *)s, (size_t)(p - s));
    tl_push(c, out, t);
  }
}

// ---- preprocessor ----

typedef struct act { const char *name; const struct act *next; } act;

static macro *find_macro(ctx *c, const char *name) {
  for (macro *m = c->macros; m; m = m->next)
    if (!strcmp(m->name, name)) return m;
  return NULL;
}

static void undef_macro(ctx *c, const char *name) {
  for (macro **pp = &c->macros; *pp; pp = &(*pp)->next) {
    if (!strcmp((*pp)->name, name)) {
      *pp = (*pp)->next;
      return;
    }
  }
}

static void define_int(ctx *c, const char *name, int v) {
  undef_macro(c, name);
  macro *m = amalloc(c, &c->perm, sizeof *m);
  memset(m, 0, sizeof *m);
  m->name = name;
  m->np = -1;
  m->body = amalloc(c, &c->perm, sizeof(tok));
  memset(m->body, 0, sizeof(tok));
  m->body[0].k = TK_INT;
  m->body[0].s = PS("%d", v);
  m->nb = 1;
  m->next = c->macros;
  c->macros = m;
}

static int is_active(const act *a, const char *n) {
  for (; a; a = a->next)
    if (!strcmp(a->name, n)) return 1;
  return 0;
}

static void expand_list(ctx *c, const tlist *l, tlist *out, const act *a, int depth);

static void paste_tokens(ctx *c, tlist *r) {
  int w = 0;
  for (int i = 0; i < r->n; i++) {
    if (isop(&r->v[i], "##") && w > 0 && i + 1 < r->n) {
      char *s = PS("%s%s", r->v[w - 1].s, r->v[i + 1].s);
      tlist t = { 0 };
      lex(c, s, &t);
      if (t.n != 1) fail(c, "bad token paste '%s'", s);
      t.v[0].line = r->v[w - 1].line;
      t.v[0].bol = 0;
      t.v[0].ws = r->v[w - 1].ws;
      r->v[w - 1] = t.v[0];
      i++;
      continue;
    }
    r->v[w++] = r->v[i];
  }
  r->n = w;
}

static void expand_one(ctx *c, const tlist *src, int *pos, tlist *out, const act *a, int depth) {
  tok t = src->v[*pos];
  c->pp_line = t.line;
  if (t.k != TK_ID) { tl_push(c, out, t); (*pos)++; return; }
  if (!strcmp(t.s, "__LINE__")) {
    t.k = TK_INT;
    t.s = PS("%d", t.line);
    tl_push(c, out, t);
    (*pos)++;
    return;
  }
  macro *m = find_macro(c, t.s);
  if (!m || is_active(a, t.s)) { tl_push(c, out, t); (*pos)++; return; }
  if (depth > 64) fail(c, "macro expansion too deep");
  act na = { m->name, a };
  if (m->np < 0) {
    (*pos)++;
    tlist body = { 0 };
    for (int i = 0; i < m->nb; i++) tl_push(c, &body, m->body[i]);
    paste_tokens(c, &body);
    expand_list(c, &body, out, &na, depth + 1);
    return;
  }
  int q = *pos + 1;
  if (q >= src->n || !isop(&src->v[q], "(")) { tl_push(c, out, t); (*pos)++; return; }
  int maxa = m->np > 0 ? m->np : 1;
  tlist *args = amalloc(c, &c->perm, sizeof(tlist) * (size_t)maxa);
  memset(args, 0, sizeof(tlist) * (size_t)maxa);
  int na_ = 0, lvl = 0;
  q++;
  for (;;) {
    if (q >= src->n) fail(c, "unterminated call to macro '%s'", m->name);
    const tok *x = &src->v[q];
    if (x->bol && isop(x, "#")) fail(c, "directive inside arguments of macro '%s'", m->name);
    if (isop(x, "(")) lvl++;
    else if (isop(x, ")")) {
      if (!lvl) break;
      lvl--;
    } else if (isop(x, ",") && !lvl) {
      if (++na_ >= maxa) fail(c, "too many arguments to macro '%s'", m->name);
      q++;
      continue;
    }
    tok y = *x;
    y.bol = 0;
    tl_push(c, &args[na_], y);
    q++;
  }
  int nargs = na_ + 1;
  if (m->np == 0) {
    if (args[0].n) fail(c, "macro '%s' takes no arguments", m->name);
  } else if (nargs != m->np) {
    fail(c, "macro '%s' expects %d arguments", m->name, m->np);
  }
  *pos = q + 1;
  tlist *xargs = amalloc(c, &c->perm, sizeof(tlist) * (size_t)maxa);
  memset(xargs, 0, sizeof(tlist) * (size_t)maxa);
  for (int i = 0; i < m->np; i++) expand_list(c, &args[i], &xargs[i], a, depth + 1);
  tlist r = { 0 };
  for (int i = 0; i < m->nb; i++) {
    tok b = m->body[i];
    int pi = -1;
    if (b.k == TK_ID)
      for (int k = 0; k < m->np; k++)
        if (!strcmp(m->par[k], b.s)) { pi = k; break; }
    if (pi < 0) { tl_push(c, &r, b); continue; }
    int paste = (i > 0 && isop(&m->body[i - 1], "##")) || (i + 1 < m->nb && isop(&m->body[i + 1], "##"));
    const tlist *sl = paste ? &args[pi] : &xargs[pi];
    for (int k = 0; k < sl->n; k++) tl_push(c, &r, sl->v[k]);
  }
  paste_tokens(c, &r);
  expand_list(c, &r, out, &na, depth + 1);
}

static void expand_list(ctx *c, const tlist *l, tlist *out, const act *a, int depth) {
  int i = 0;
  while (i < l->n) expand_one(c, l, &i, out, a, depth);
}

// #if expression evaluator
static long long ppx(ctx *c, const tlist *e, int *pos, int minp);

static long long ppx_un(ctx *c, const tlist *e, int *pos) {
  if (*pos >= e->n) fail(c, "bad #if expression");
  const tok *t = &e->v[(*pos)++];
  if (t->k == TK_OP) {
    if (!strcmp(t->s, "(")) {
      long long v = ppx(c, e, pos, 0);
      if (*pos >= e->n || !isop(&e->v[*pos], ")")) fail(c, "missing ')' in #if");
      (*pos)++;
      return v;
    }
    if (!strcmp(t->s, "-")) return -ppx_un(c, e, pos);
    if (!strcmp(t->s, "+")) return ppx_un(c, e, pos);
    if (!strcmp(t->s, "!")) return !ppx_un(c, e, pos);
    if (!strcmp(t->s, "~")) return ~ppx_un(c, e, pos);
  }
  if (t->k == TK_INT) return strtoll(t->s, NULL, 0);
  if (t->k == TK_ID) return 0;
  fail(c, "bad token '%s' in #if", t->s);
}

static int ppx_prec(const tok *t) {
  static const char *const tab[] = {
    "||", "1", "&&", "2", "|", "3", "^", "4", "&", "5", "==", "6", "!=", "6",
    "<", "7", ">", "7", "<=", "7", ">=", "7", "<<", "8", ">>", "8",
    "+", "9", "-", "9", "*", "10", "/", "10", "%", "10", NULL
  };
  if (t->k != TK_OP) return -1;
  for (int i = 0; tab[i]; i += 2)
    if (!strcmp(tab[i], t->s)) return atoi(tab[i + 1]);
  return -1;
}

static long long ppx(ctx *c, const tlist *e, int *pos, int minp) {
  long long l = ppx_un(c, e, pos);
  while (*pos < e->n) {
    const tok *t = &e->v[*pos];
    if (minp == 0 && isop(t, "?")) {
      (*pos)++;
      long long a = ppx(c, e, pos, 0);
      if (*pos >= e->n || !isop(&e->v[*pos], ":")) fail(c, "missing ':' in #if");
      (*pos)++;
      long long b = ppx(c, e, pos, 0);
      l = l ? a : b;
      continue;
    }
    int p = ppx_prec(t);
    if (p < 0 || p < minp) break;
    (*pos)++;
    long long r = ppx(c, e, pos, p + 1);
    const char *o = t->s;
    if (!strcmp(o, "||")) l = l || r;
    else if (!strcmp(o, "&&")) l = l && r;
    else if (!strcmp(o, "|")) l = l | r;
    else if (!strcmp(o, "^")) l = l ^ r;
    else if (!strcmp(o, "&")) l = l & r;
    else if (!strcmp(o, "==")) l = l == r;
    else if (!strcmp(o, "!=")) l = l != r;
    else if (!strcmp(o, "<")) l = l < r;
    else if (!strcmp(o, ">")) l = l > r;
    else if (!strcmp(o, "<=")) l = l <= r;
    else if (!strcmp(o, ">=")) l = l >= r;
    else if (!strcmp(o, "<<")) l = l << r;
    else if (!strcmp(o, ">>")) l = l >> r;
    else if (!strcmp(o, "+")) l = l + r;
    else if (!strcmp(o, "-")) l = l - r;
    else if (!strcmp(o, "*")) l = l * r;
    else {
      if (!r) fail(c, "division by zero in #if");
      l = !strcmp(o, "/") ? l / r : l % r;
    }
  }
  return l;
}

static long long pp_eval(ctx *c, const tok *t, int n) {
  tlist l = { 0 }, e = { 0 };
  for (int i = 0; i < n; i++) {
    if (isid(&t[i], "defined")) {
      int par = i + 1 < n && isop(&t[i + 1], "(");
      int k = i + 1 + par;
      if (k >= n || t[k].k != TK_ID) fail(c, "bad 'defined' in #if");
      if (par && (k + 1 >= n || !isop(&t[k + 1], ")"))) fail(c, "bad 'defined' in #if");
      tok r = t[k];
      r.k = TK_INT;
      r.s = find_macro(c, t[k].s) ? "1" : "0";
      tl_push(c, &l, r);
      i = k + par;
      continue;
    }
    tl_push(c, &l, t[i]);
  }
  expand_list(c, &l, &e, NULL, 0);
  if (!e.n) fail(c, "empty #if expression");
  int pos = 0;
  long long v = ppx(c, &e, &pos, 0);
  if (pos != e.n) fail(c, "bad #if expression");
  return v;
}

static void pp_define(ctx *c, const tok *d, int nd) {
  if (nd < 2 || d[1].k != TK_ID) fail(c, "bad #define");
  if (!strncmp(d[1].s, "GL_", 3) || !strncmp(d[1].s, "__", 2)) fail(c, "can't #define reserved name '%s'", d[1].s);
  macro *m = amalloc(c, &c->perm, sizeof *m);
  memset(m, 0, sizeof *m);
  m->name = d[1].s;
  m->np = -1;
  int k = 2;
  if (k < nd && isop(&d[k], "(") && !d[k].ws) {
    m->np = 0;
    m->par = amalloc(c, &c->perm, sizeof(char *) * MAX_MACRO_PARAMS);
    k++;
    if (k < nd && isop(&d[k], ")")) {
      k++;
    } else {
      for (;;) {
        if (k >= nd || d[k].k != TK_ID) fail(c, "bad macro parameter list");
        if (m->np == MAX_MACRO_PARAMS) fail(c, "too many macro parameters");
        m->par[m->np++] = d[k].s;
        k++;
        if (k < nd && isop(&d[k], ",")) { k++; continue; }
        if (k < nd && isop(&d[k], ")")) { k++; break; }
        fail(c, "bad macro parameter list");
      }
    }
  }
  m->nb = nd - k;
  m->body = amalloc(c, &c->perm, sizeof(tok) * (size_t)(m->nb ? m->nb : 1));
  for (int i = 0; i < m->nb; i++) {
    m->body[i] = d[k + i];
    m->body[i].bol = 0;
    if (isop(&d[k + i], "#") && m->np >= 0) fail(c, "stringizing (#) is not supported");
  }
  undef_macro(c, m->name);
  m->next = c->macros;
  c->macros = m;
}

typedef struct { unsigned char active, taken, seen_else, parent; } ifst;

static void preprocess(ctx *c, const tlist *in, tlist *out) {
  ifst stk[MAX_IF_DEPTH];
  int sp = 0, i = 0;
  while (i < in->n) {
    const tok *t = &in->v[i];
    int active = sp ? stk[sp - 1].active : 1;
    c->pp_line = t->line;
    if (t->bol && isop(t, "#")) {
      int j = i + 1;
      while (j < in->n && !in->v[j].bol) j++;
      const tok *d = in->v + i + 1;
      int nd = j - i - 1;
      i = j;
      if (!nd) continue;
      if (d[0].k != TK_ID) {
        if (active) fail(c, "bad preprocessor directive");
        continue;
      }
      const char *dn = d[0].s;
      if (!strcmp(dn, "if") || !strcmp(dn, "ifdef") || !strcmp(dn, "ifndef")) {
        if (sp == MAX_IF_DEPTH) fail(c, "#if nesting too deep");
        ifst s;
        memset(&s, 0, sizeof s);
        s.parent = (unsigned char)active;
        if (active) {
          int v;
          if (dn[2]) {
            if (nd < 2 || d[1].k != TK_ID) fail(c, "#%s needs a name", dn);
            v = find_macro(c, d[1].s) != NULL;
            if (dn[2] == 'n') v = !v;
          } else {
            v = pp_eval(c, d + 1, nd - 1) != 0;
          }
          s.active = s.taken = (unsigned char)v;
        } else {
          s.taken = 1;
        }
        stk[sp++] = s;
      } else if (!strcmp(dn, "elif")) {
        if (!sp) fail(c, "#elif without #if");
        ifst *s = &stk[sp - 1];
        if (s->seen_else) fail(c, "#elif after #else");
        if (s->taken || !s->parent) {
          s->active = 0;
        } else {
          int v = pp_eval(c, d + 1, nd - 1) != 0;
          s->active = s->taken = (unsigned char)v;
        }
      } else if (!strcmp(dn, "else")) {
        if (!sp) fail(c, "#else without #if");
        ifst *s = &stk[sp - 1];
        if (s->seen_else) fail(c, "duplicate #else");
        s->seen_else = 1;
        s->active = s->parent && !s->taken;
        s->taken = 1;
      } else if (!strcmp(dn, "endif")) {
        if (!sp) fail(c, "#endif without #if");
        sp--;
      } else if (!active) {
        continue;
      } else if (!strcmp(dn, "define")) {
        pp_define(c, d, nd);
      } else if (!strcmp(dn, "undef")) {
        if (nd < 2 || d[1].k != TK_ID) fail(c, "bad #undef");
        undef_macro(c, d[1].s);
      } else if (!strcmp(dn, "version")) {
        if (nd >= 2 && d[1].k == TK_INT) define_int(c, "__VERSION__", atoi(d[1].s));
      } else if (!strcmp(dn, "extension") || !strcmp(dn, "pragma") || !strcmp(dn, "line")) {
        // nothing to do
      } else if (!strcmp(dn, "error")) {
        sb *m = &c->scratch;
        sb_clear(m);
        for (int k = 1; k < nd; k++) {
          sb_puts(c, m, d[k].s);
          sb_puts(c, m, " ");
        }
        fail(c, "#error %s", m->p ? m->p : "");
      } else {
        fail(c, "unknown directive #%s", dn);
      }
      continue;
    }
    if (!active) { i++; continue; }
    expand_one(c, in, &i, out, NULL, 0);
  }
  if (sp) fail(c, "unterminated #if");
}

// ---- types / names ----

static gty ty(int b, int r, int cc) {
  gty t;
  t.b = (unsigned char)b;
  t.r = (unsigned char)r;
  t.c = (unsigned char)cc;
  t.arr = 0;
  t.st = 0;
  return t;
}

static int teq(gty a, gty b) {
  return a.b == b.b && a.r == b.r && a.c == b.c && a.arr == b.arr && (a.b != B_STRUCT || a.st == b.st);
}
static int is_num(gty t) { return !t.arr && (t.b == B_FLOAT || t.b == B_INT || t.b == B_BOOL); }
static int is_scalar(gty t) { return is_num(t) && t.r == 1 && t.c == 1; }
static int is_vec(gty t) { return is_num(t) && t.r > 1 && t.c == 1; }
static int is_mat(gty t) { return is_num(t) && t.c > 1; }
static int comps(gty t) { return t.r * t.c; }
static const char *swz_n(int n) { return n == 1 ? "x" : n == 2 ? "xy" : n == 3 ? "xyz" : "xyzw"; }

static const char *cgty(ctx *c, gty t) {
  switch (t.b) {
  case B_VOID: return "void";
  case B_S2D: return "sampler2D";
  case B_SCUBE: return "samplerCUBE";
  case B_STRUCT: return c->structs[t.st].cg;
  default: break;
  }
  const char *bn = t.b == B_FLOAT ? "float" : t.b == B_INT ? "int" : "bool";
  if (t.c > 1) return TS("%s%dx%d", bn, t.c, t.r);
  if (t.r > 1) return TS("%s%d", bn, t.r);
  return bn;
}

static const char *arrsuf(ctx *c, gty t) { return t.arr > 0 ? TS("[%d]", t.arr) : ""; }
static const char *tname(ctx *c, gty t) { return TS("%s%s", cgty(c, t), arrsuf(c, t)); }

static int is_prec(const char *s) { return !strcmp(s, "highp") || !strcmp(s, "mediump") || !strcmp(s, "lowp"); }

static int type_kw(ctx *c, const char *s, gty *t) {
  if (!strcmp(s, "void")) { *t = ty(B_VOID, 1, 1); return 1; }
  if (!strcmp(s, "float")) { *t = ty(B_FLOAT, 1, 1); return 1; }
  if (!strcmp(s, "int") || !strcmp(s, "uint")) { *t = ty(B_INT, 1, 1); return 1; }
  if (!strcmp(s, "bool")) { *t = ty(B_BOOL, 1, 1); return 1; }
  if (!strcmp(s, "sampler2D")) { *t = ty(B_S2D, 1, 1); return 1; }
  if (!strcmp(s, "samplerCube")) { *t = ty(B_SCUBE, 1, 1); return 1; }
  const char *q = s;
  int b = B_FLOAT;
  if (*q == 'i' || *q == 'u') { b = B_INT; q++; }
  else if (*q == 'b') { b = B_BOOL; q++; }
  if (!strncmp(q, "vec", 3) && q[3] >= '2' && q[3] <= '4' && !q[4]) { *t = ty(b, q[3] - '0', 1); return 1; }
  if (q == s && !strncmp(s, "mat", 3) && s[3] >= '2' && s[3] <= '4') {
    int C = s[3] - '0', R = C;
    if (s[4] == 'x' && s[5] >= '2' && s[5] <= '4' && !s[6]) R = s[5] - '0';
    else if (s[4]) return 0;
    *t = ty(B_FLOAT, R, C);
    return 1;
  }
  // other sampler types exist in GLSL but not here
  const char *x = NULL;
  if (!strncmp(s, "sampler", 7)) x = s + 7;
  else if (!strncmp(s, "isampler", 8) || !strncmp(s, "usampler", 8)) x = s + 8;
  if (x && (is_digit((unsigned char)*x) || *x == 'C' || *x == 'E'))
    fail(c, "unsupported type '%s'", s);
  return 0;
}

static g2c_type gtype_enum(gty t) {
  if (t.b == B_S2D) return G2C_SAMPLER2D;
  if (t.b == B_SCUBE) return G2C_SAMPLERCUBE;
  if (t.c > 1) {
    if (t.c != t.r) return G2C_OTHER;
    return t.c == 2 ? G2C_MAT2 : t.c == 3 ? G2C_MAT3 : G2C_MAT4;
  }
  if (t.b == B_FLOAT) return (g2c_type)(G2C_FLOAT + t.r - 1);
  if (t.b == B_INT) return (g2c_type)(G2C_INT + t.r - 1);
  if (t.b == B_BOOL) return (g2c_type)(G2C_BOOL + t.r - 1);
  return G2C_OTHER;
}

// identifiers Cg (psp2) reserves or defines that GLSL doesn't
static const char *const cg_words[] = {
  "asm", "asm_fragment", "auto", "case", "catch", "char", "class", "column_major", "compile",
  "const_cast", "decl", "default", "delete", "double", "dword", "dynamic_cast", "emit", "enum",
  "explicit", "extern", "fixed", "friend", "get", "goto", "half", "inline", "interface", "long",
  "matrix", "Matrix", "mutable", "namespace", "new", "operator", "packed", "pass", "pixelfragment",
  "pixelshader", "private", "protected", "public", "register", "reinterpret_cast", "row_major",
  "sampler", "sampler_state", "sampler1D", "sampler3D", "samplerCUBE", "samplerRECT", "shared",
  "short", "signed", "sizeof", "static", "static_cast", "string", "technique", "template",
  "texture", "texture1D", "texture2D", "texture3D", "textureCUBE", "textureRECT", "this", "throw",
  "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "vector", "vertexfragment",
  "vertexshader", "virtual", "volatile", "point", "line", "triangle", "lineadj", "triangleadj",
  "snorm", "unorm", "packoffset", "cbuffer", "tbuffer", "nointerpolation", "precise", "main",
  "uint", "switch", "in", "out", "inout", "uniform", "varying", "attribute", "const", "struct",
  // hlsl-ish object types sony's cg knows about (a game shader names a sampler "Texture")
  "Texture", "Texture1D", "Texture2D", "Texture3D", "TextureCube", "Texture1DArray",
  "Texture2DArray", "TextureCubeArray", "Texture2DMS", "Texture2DMSArray", "Buffer",
  "Sampler", "SamplerState", "SamplerComparisonState", "PixelShader", "VertexShader",
  "BlendState", "DepthStencilState", "RasterizerState", "String", "Vector",
  // stdlib
  "abs", "acos", "all", "any", "asin", "atan", "atan2", "ceil", "clamp", "clip", "cos", "cosh",
  "cross", "ddx", "ddy", "debug", "degrees", "determinant", "distance", "dot", "exp", "exp2",
  "faceforward", "floor", "fmod", "frac", "frexp", "fwidth", "isfinite", "isinf", "isnan",
  "ldexp", "length", "lerp", "lit", "log", "log10", "log2", "max", "min", "modf", "mul", "noise",
  "normalize", "pow", "radians", "reflect", "refract", "round", "rsqrt", "saturate", "sign", "sin",
  "sincos", "sinh", "smoothstep", "sqrt", "step", "tan", "tanh", "transpose", "trunc", "bit_cast",
  "f16tof32", "f32tof16", "tex1D", "tex1Dproj", "tex1Dlod", "tex1Dbias", "tex1Dgrad", "tex1Dfetch",
  "tex1Dsize", "tex2D", "tex2Dproj", "tex2Dlod", "tex2Dbias", "tex2Dgrad", "tex2Dfetch",
  "tex2Dsize", "tex3D", "tex3Dproj", "tex3Dlod", "tex3Dbias", "tex3Dgrad", "tex3Dfetch",
  "tex3Dsize", "texCUBE", "texCUBEproj", "texCUBElod", "texCUBEbias", "texCUBEgrad", "texCUBEsize",
  "texRECT", "texRECTproj", "texRECTlod", "texRECTbias", "texRECTgrad", "texRECTfetch",
  "texRECTsize", NULL
};

static int is_cg_reserved(const char *s) {
  static const char *const pre[] = {
    "float", "half", "fixed", "int", "bool", "double", "uint", "short", "long", "char",
    "unsigned", "dword", "uchar", "ushort", NULL
  };
  for (int i = 0; cg_words[i]; i++)
    if (!strcmp(cg_words[i], s)) return 1;
  for (int i = 0; pre[i]; i++) {
    size_t n = strlen(pre[i]);
    if (strncmp(s, pre[i], n)) continue;
    const char *q = s + n;
    if (*q >= '1' && *q <= '4') {
      if (!q[1]) return 1;
      if (q[1] == 'x' && q[2] >= '1' && q[2] <= '4' && !q[3]) return 1;
    }
  }
  return !strncmp(s, "_g2c_", 5) || !strncmp(s, "gl_", 3);
}

static int global_taken(ctx *c, const char *n) {
  for (sym *s = c->syms; s; s = s->next)
    if (s->depth == 0 && !strcmp(s->cg, n)) return 1;
  for (fn *f = c->fns; f; f = f->next)
    if (!strcmp(f->cg, n)) return 1;
  for (int i = 0; i < c->nstructs; i++)
    if (!strcmp(c->structs[i].cg, n)) return 1;
  return 0;
}

static int fname_taken(ctx *c, const char *n) {
  for (int i = 0; i < c->nfnames; i++)
    if (!strcmp(c->fnames[i], n)) return 1;
  return 0;
}

static const char *unique(ctx *c, const char *base) {
  for (;;) {
    const char *s = PS("g2c%d_%s", ++c->uniq, base);
    if (!global_taken(c, s) && !fname_taken(c, s)) return s;
  }
}

static const char *global_cg(ctx *c, const char *name) {
  if (is_cg_reserved(name) || global_taken(c, name)) return unique(c, name);
  return name;
}

// locals get a name that's unique in the whole function, so it works no
// matter how SceShaccCg scopes for-init / nested blocks
static const char *local_cg(ctx *c, const char *name) {
  const char *cg = name;
  if (is_cg_reserved(name) || global_taken(c, name) || fname_taken(c, name)) cg = unique(c, name);
  push_name(c, &c->fnames, &c->nfnames, &c->capfnames, cg);
  return cg;
}

// api-visible names (uniforms/attribs) keep their GLSL name unless Cg
// reserves it, then they get a prefix and the GL side looks up cg_name
static const char *api_cg_name(ctx *c, const char *name, const char *what) {
  if (strlen(name) + 5 >= GLSL2CG_NAME_LEN) fail(c, "%s name '%s' is too long", what, name);
  return is_cg_reserved(name) ? PS("g2cu_%s", name) : name;
}

// ---- symbols ----

static sym *add_sym(ctx *c, const char *name, const char *cg, gty t, int kind) {
  for (sym *s = c->syms; s && s->depth == c->depth; s = s->next)
    if (!strcmp(s->name, name)) fail(c, "redeclaration of '%s'", name);
  sym *s = amalloc(c, &c->perm, sizeof *s);
  memset(s, 0, sizeof *s);
  s->name = name;
  s->cg = cg;
  s->t = t;
  s->kind = (unsigned char)kind;
  s->depth = c->depth;
  s->next = c->syms;
  c->syms = s;
  return s;
}

static sym *lookup(ctx *c, const char *name) {
  for (sym *s = c->syms; s; s = s->next)
    if (!strcmp(s->name, name)) return s;
  return NULL;
}

static void push_scope(ctx *c) { c->depth++; }
static void pop_scope(ctx *c) {
  while (c->syms && c->syms->depth >= c->depth) c->syms = c->syms->next;
  c->depth--;
}

static int find_struct(ctx *c, const char *n) {
  for (int i = c->nstructs - 1; i >= 0; i--)
    if (!strcmp(c->structs[i].name, n)) return i;
  return -1;
}

// ---- token helpers ----

static tok *pk(ctx *c, int o) {
  int i = c->p + o;
  return i < c->ntk ? &c->tk[i] : &c->tk[c->ntk - 1];
}

static int accept(ctx *c, const char *op) {
  if (isop(pk(c, 0), op)) { c->p++; return 1; }
  return 0;
}

static void expect(ctx *c, const char *op) {
  if (!accept(c, op)) fail(c, "expected '%s' near '%s'", op, pk(c, 0)->s);
}

static const char *expect_id(ctx *c) {
  tok *t = pk(c, 0);
  if (t->k != TK_ID) fail(c, "expected an identifier near '%s'", t->s);
  c->p++;
  return t->s;
}

static void skip_to_semi(ctx *c) {
  while (!accept(c, ";")) {
    if (pk(c, 0)->k == TK_EOF) fail(c, "missing ';'");
    c->p++;
  }
}

#define OUT(...) sb_printf(c, &c->S->body, __VA_ARGS__)
static void ind(ctx *c) {
  for (int i = 0; i < c->indent; i++) sb_putn(c, &c->S->body, "\t", 1);
}

// text that belongs at global scope; inside a function it goes before it
static void emit_global_text(ctx *c, const char *s) {
  if (c->in_fn) {
    sb_insert(c, &c->S->body, c->fn_start, s);
    c->fn_start += strlen(s);
  } else {
    sb_puts(c, &c->S->body, s);
  }
}

static int helper_once(ctx *c, const char *key) {
  for (int i = 0; i < c->nhkeys; i++)
    if (!strcmp(c->hkeys[i], key)) return 0;
  push_name(c, &c->hkeys, &c->nhkeys, &c->caphkeys, PS("%s", key));
  return 1;
}

// ---- expressions ----

static ex parse_expr(ctx *c);
static ex parse_assign(ctx *c);
static ex parse_cond(ctx *c);
static gty parse_type(ctx *c, int allow_struct);

static ex mk(const char *s, gty t) {
  ex e;
  memset(&e, 0, sizeof e);
  e.s = s;
  e.t = t;
  return e;
}

static ex mk_int(ctx *c, long long v) {
  ex e = mk(TS("%lld", v), ty(B_INT, 1, 1));
  e.has_ival = 1;
  e.ival = v;
  return e;
}

// turn a pending value into plain Cg text
static G2C_NOINLINE const char *fin(ctx *c, ex *e) {
  switch (e->pn) {
  case PN_NONE:
    return e->s;
  case PN_STORE: {
    const char *s = e->s;
    if (e->raw4 && comps(e->t) < 4) s = TS("%s.%s", s, swz_n(comps(e->t)));
    if (e->cast) s = TS("((%s)(%s))", cgty(c, e->t), s);
    e->s = s;
    break;
  }
  case PN_MATU: {
    int C = e->u->t.c, R = e->u->t.r;
    const char *sw = R == 4 ? "" : R == 3 ? ".xyz" : ".xy";
    const char *l = "";
    for (int i = 0; i < C; i++) l = TS("%s%s%s[%d]%s", l, i ? ", " : "", e->u->cg, i, sw);
    e->s = TS("float%dx%d(%s)", C, R, l);
    break;
  }
  case PN_MATEL:
    e->s = TS("_g2c_mu_%s(%s)", e->u->cg, e->idx);
    break;
  case PN_ARR:
    fail(c, "uniform array '%s' can only be used with an index", e->u->name);
  case PN_INIT:
    fail(c, "array constructors are only supported as initializers");
  }
  e->pn = PN_NONE;
  return e->s;
}

static const char *bcast(ctx *c, gty t, const char *s) { return TS("((%s)(%s))", cgty(c, t), s); }

static void need_diag(ctx *c, int C, int R) {
  if (!helper_once(c, TS("diag%dx%d", C, R))) return;
  const char *l = "";
  for (int i = 0; i < C; i++)
    for (int j = 0; j < R; j++) l = TS("%s%s%s", l, (i || j) ? ", " : "", i == j ? "s" : "0.0");
  sb_printf(c, &c->S->helpers, "float%dx%d _g2c_diag%dx%d(float s) { return float%dx%d(%s); }\n",
            C, R, C, R, C, R, l);
}

static void need_resize(ctx *c, int C, int R, int C2, int R2) {
  if (!helper_once(c, TS("rs%dx%d_%dx%d", C, R, C2, R2))) return;
  const char *l = "";
  for (int i = 0; i < C; i++)
    for (int j = 0; j < R; j++) {
      const char *v = (i < C2 && j < R2) ? TS("m[%d][%d]", i, j) : i == j ? "1.0" : "0.0";
      l = TS("%s%s%s", l, (i || j) ? ", " : "", v);
    }
  sb_printf(c, &c->S->helpers, "float%dx%d _g2c_m%dx%d_from_%dx%d(float%dx%d m) { return float%dx%d(%s); }\n",
            C, R, C, R, C2, R2, C2, R2, C, R, l);
}

static void need_mod(ctx *c, gty t) {
  if (!helper_once(c, TS("mod%d%d", t.b, t.r))) return;
  const char *n = cgty(c, t);
  sb_printf(c, &c->S->helpers, "%s _g2c_mod(%s x, %s y) { return x - y * floor(x / y); }\n", n, n, n);
}

static void need_projlod(ctx *c, int n) {
  if (!helper_once(c, TS("projlod%d", n))) return;
  sb_printf(c, &c->S->helpers,
            "float4 _g2c_projlod(sampler2D s, float%d p, float l) { return tex2Dlod(s, float4(p.xy / p.%c, 0.0, l)); }\n",
            n, n == 4 ? 'w' : 'z');
}

static G2C_NOINLINE ex construct(ctx *c, gty T, ex *a, int n) {
  if (T.b == B_VOID || T.b == B_S2D || T.b == B_SCUBE) fail(c, "can't construct %s", tname(c, T));
  if (!n) fail(c, "constructor needs arguments");
  for (int i = 0; i < n; i++)
    if (!is_num(a[i].t)) fail(c, "bad argument to %s constructor", cgty(c, T));
  const char *tn = cgty(c, T);
  if (T.c == 1 && T.r == 1) {  // scalar
    if (n != 1) fail(c, "too many arguments to %s constructor", tn);
    if (is_mat(a[0].t)) fail(c, "scalar from matrix constructor is not supported");
    const char *s = fin(c, &a[0]);
    if (is_vec(a[0].t)) s = TS("(%s).x", s);
    else if (a[0].t.b == T.b) {
      ex r = mk(TS("(%s)", s), T);
      r.has_ival = a[0].has_ival;
      r.ival = a[0].ival;
      return r;
    }
    return mk(TS("((%s)(%s))", tn, s), T);
  }
  if (T.c == 1) {  // vector
    int N = T.r;
    if (n == 1) {
      if (is_mat(a[0].t)) fail(c, "vector from matrix constructor is not supported");
      const char *s = fin(c, &a[0]);
      int M = comps(a[0].t);
      if (M == 1) return mk(TS("((%s)(%s))", tn, s), T);
      if (M < N) fail(c, "not enough components for %s constructor", tn);
      if (M > N) s = TS("%s.%s", s, swz_n(N));
      if (a[0].t.b != T.b) s = TS("((%s)(%s))", tn, s);
      return mk(s, T);
    }
    int used = 0;
    const char *l = "";
    for (int i = 0; i < n; i++) {
      if (is_mat(a[i].t)) fail(c, "matrix argument in vector constructor is not supported");
      if (used >= N) fail(c, "too many arguments to %s constructor", tn);
      int k = comps(a[i].t);
      const char *s = fin(c, &a[i]);
      if (used + k > N) {
        if (i != n - 1) fail(c, "too many arguments to %s constructor", tn);
        k = N - used;
        s = TS("%s.%s", s, swz_n(k));
      }
      if (a[i].t.b != T.b) s = bcast(c, ty(T.b, k, 1), s);
      l = TS("%s%s%s", l, i ? ", " : "", s);
      used += k;
    }
    if (used < N) fail(c, "not enough components for %s constructor", tn);
    return mk(TS("%s(%s)", tn, l), T);
  }
  // matrix
  int C = T.c, R = T.r;
  if (n == 1) {
    const char *s = fin(c, &a[0]);
    if (is_scalar(a[0].t)) {
      if (a[0].t.b != B_FLOAT) s = TS("((float)(%s))", s);
      need_diag(c, C, R);
      return mk(TS("_g2c_diag%dx%d(%s)", C, R, s), T);
    }
    if (is_mat(a[0].t)) {
      if (a[0].t.c == C && a[0].t.r == R) return mk(s, T);
      need_resize(c, C, R, a[0].t.c, a[0].t.r);
      return mk(TS("_g2c_m%dx%d_from_%dx%d(%s)", C, R, a[0].t.c, a[0].t.r, s), T);
    }
    fail(c, "unsupported %s constructor", tn);
  }
  int cols = n == C, scal = n == C * R;
  for (int i = 0; i < n; i++) {
    if (!(is_vec(a[i].t) && a[i].t.r == R)) cols = 0;
    if (!is_scalar(a[i].t)) scal = 0;
  }
  if (!cols && !scal) fail(c, "unsupported %s constructor (mixed columns/scalars)", tn);
  const char *l = "";
  for (int i = 0; i < n; i++) {
    const char *s = fin(c, &a[i]);
    if (a[i].t.b != B_FLOAT) s = bcast(c, ty(B_FLOAT, a[i].t.r, 1), s);
    l = TS("%s%s%s", l, i ? ", " : "", s);
  }
  // Cg rows == GLSL columns, so column-major args map straight across
  return mk(TS("%s(%s)", tn, l), T);
}

static G2C_NOINLINE ex struct_ctor(ctx *c, int si, ex *a, int n) {
  const sdef *d = &c->structs[si];
  if (n != d->nm) fail(c, "wrong number of arguments to %s constructor", d->name);
  const char *l = "";
  for (int i = 0; i < n; i++) {
    const char *s = fin(c, &a[i]);
    if (STRICT && !teq(a[i].t, d->m[i].t))
      fail(c, "%s constructor: member %d is %s, got %s", d->name, i, tname(c, d->m[i].t), tname(c, a[i].t));
    l = TS("%s%s%s", l, i ? ", " : "", s);
  }
  gty t = ty(B_STRUCT, 1, 1);
  t.st = (short)si;
  return mk(TS("_g2c_mk_%s(%s)", d->cg, l), t);
}

// builtin functions
enum {
  BI_GEN = 1, BI_GENB, BI_FLT, BI_RED, BI_CMP, BI_NOT, BI_CROSS, BI_MIX, BI_ATAN,
  BI_MOD, BI_TRANS, BI_MCM, BI_BVEC, BI_TEX, BI_UNSUP
};
enum { TX_PLAIN, TX_LOD, TX_PROJ, TX_PROJLOD, TX_GRAD };
typedef struct { const char *g, *cg; unsigned char k, sub, mn, mx, samp; } bdef;

static const bdef bdefs[] = {
  { "radians", "radians", BI_GEN, 0, 1, 1, 0 }, { "degrees", "degrees", BI_GEN, 0, 1, 1, 0 },
  { "sin", "sin", BI_GEN, 0, 1, 1, 0 }, { "cos", "cos", BI_GEN, 0, 1, 1, 0 },
  { "tan", "tan", BI_GEN, 0, 1, 1, 0 }, { "asin", "asin", BI_GEN, 0, 1, 1, 0 },
  { "acos", "acos", BI_GEN, 0, 1, 1, 0 }, { "sinh", "sinh", BI_GEN, 0, 1, 1, 0 },
  { "cosh", "cosh", BI_GEN, 0, 1, 1, 0 }, { "tanh", "tanh", BI_GEN, 0, 1, 1, 0 },
  { "atan", "atan", BI_ATAN, 0, 1, 2, 0 },
  { "pow", "pow", BI_GEN, 0, 2, 2, 0 }, { "exp", "exp", BI_GEN, 0, 1, 1, 0 },
  { "log", "log", BI_GEN, 0, 1, 1, 0 }, { "exp2", "exp2", BI_GEN, 0, 1, 1, 0 },
  { "log2", "log2", BI_GEN, 0, 1, 1, 0 }, { "sqrt", "sqrt", BI_GEN, 0, 1, 1, 0 },
  { "inversesqrt", "rsqrt", BI_GEN, 0, 1, 1, 0 },
  { "abs", "abs", BI_GEN, 0, 1, 1, 0 }, { "sign", "sign", BI_GEN, 0, 1, 1, 0 },
  { "floor", "floor", BI_GEN, 0, 1, 1, 0 }, { "trunc", "trunc", BI_GEN, 0, 1, 1, 0 },
  { "round", "round", BI_GEN, 0, 1, 1, 0 }, { "roundEven", "round", BI_GEN, 0, 1, 1, 0 },
  { "ceil", "ceil", BI_GEN, 0, 1, 1, 0 }, { "fract", "frac", BI_GEN, 0, 1, 1, 0 },
  { "mod", NULL, BI_MOD, 0, 2, 2, 0 }, { "modf", "modf", BI_GEN, 0, 2, 2, 0 },
  { "min", "min", BI_GENB, 0, 2, 2, 0 }, { "max", "max", BI_GENB, 0, 2, 2, 0 },
  { "clamp", "clamp", BI_GENB, 0, 3, 3, 0 }, { "mix", NULL, BI_MIX, 0, 3, 3, 0 },
  { "step", "step", BI_GENB, 0, 2, 2, 0 }, { "smoothstep", "smoothstep", BI_GENB, 0, 3, 3, 0 },
  { "isnan", "isnan", BI_BVEC, 0, 1, 1, 0 }, { "isinf", "isinf", BI_BVEC, 0, 1, 1, 0 },
  { "length", "length", BI_FLT, 0, 1, 1, 0 }, { "distance", "distance", BI_FLT, 0, 2, 2, 0 },
  { "dot", "dot", BI_FLT, 0, 2, 2, 0 }, { "cross", "cross", BI_CROSS, 0, 2, 2, 0 },
  { "normalize", "normalize", BI_GEN, 0, 1, 1, 0 },
  { "faceforward", "faceforward", BI_GEN, 0, 3, 3, 0 },
  { "reflect", "reflect", BI_GEN, 0, 2, 2, 0 }, { "refract", "refract", BI_GEN, 0, 3, 3, 0 },
  { "matrixCompMult", NULL, BI_MCM, 0, 2, 2, 0 }, { "transpose", "transpose", BI_TRANS, 0, 1, 1, 0 },
  { "determinant", "determinant", BI_FLT, 0, 1, 1, 0 },
  { "lessThan", "<", BI_CMP, 0, 2, 2, 0 }, { "lessThanEqual", "<=", BI_CMP, 0, 2, 2, 0 },
  { "greaterThan", ">", BI_CMP, 0, 2, 2, 0 }, { "greaterThanEqual", ">=", BI_CMP, 0, 2, 2, 0 },
  { "equal", "==", BI_CMP, 0, 2, 2, 0 }, { "notEqual", "!=", BI_CMP, 0, 2, 2, 0 },
  { "any", "any", BI_RED, 0, 1, 1, 0 }, { "all", "all", BI_RED, 0, 1, 1, 0 },
  { "not", NULL, BI_NOT, 0, 1, 1, 0 },
  { "dFdx", "ddx", BI_GEN, 0, 1, 1, 0 }, { "dFdy", "ddy", BI_GEN, 0, 1, 1, 0 },
  { "fwidth", "fwidth", BI_GEN, 0, 1, 1, 0 },
  { "texture", NULL, BI_TEX, TX_PLAIN, 2, 3, 0 }, { "texture2D", NULL, BI_TEX, TX_PLAIN, 2, 3, 1 },
  { "textureCube", NULL, BI_TEX, TX_PLAIN, 2, 3, 2 },
  { "textureLod", NULL, BI_TEX, TX_LOD, 3, 3, 0 }, { "texture2DLod", NULL, BI_TEX, TX_LOD, 3, 3, 1 },
  { "texture2DLodEXT", NULL, BI_TEX, TX_LOD, 3, 3, 1 },
  { "textureCubeLod", NULL, BI_TEX, TX_LOD, 3, 3, 2 },
  { "textureCubeLodEXT", NULL, BI_TEX, TX_LOD, 3, 3, 2 },
  { "textureProj", NULL, BI_TEX, TX_PROJ, 2, 3, 1 }, { "texture2DProj", NULL, BI_TEX, TX_PROJ, 2, 3, 1 },
  { "textureProjLod", NULL, BI_TEX, TX_PROJLOD, 3, 3, 1 },
  { "texture2DProjLod", NULL, BI_TEX, TX_PROJLOD, 3, 3, 1 },
  { "texture2DProjLodEXT", NULL, BI_TEX, TX_PROJLOD, 3, 3, 1 },
  { "textureGrad", NULL, BI_TEX, TX_GRAD, 4, 4, 0 },
  { "texture2DGradEXT", NULL, BI_TEX, TX_GRAD, 4, 4, 1 },
  { "textureCubeGradEXT", NULL, BI_TEX, TX_GRAD, 4, 4, 2 },
  { "asinh", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "acosh", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "atanh", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "floatBitsToInt", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "floatBitsToUint", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "intBitsToFloat", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "uintBitsToFloat", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "packSnorm2x16", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "unpackSnorm2x16", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "packUnorm2x16", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "unpackUnorm2x16", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "packHalf2x16", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "unpackHalf2x16", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "outerProduct", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "inverse", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "texelFetch", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "texelFetchOffset", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "textureSize", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "textureOffset", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "textureProjOffset", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "textureLodOffset", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "textureProjLodOffset", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "textureGradOffset", NULL, BI_UNSUP, 0, 0, 0, 0 }, { "textureProjGrad", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "textureProjGradOffset", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { "texture2DProjGradEXT", NULL, BI_UNSUP, 0, 0, 0, 0 },
  { NULL, NULL, 0, 0, 0, 0, 0 }
};

static const bdef *find_bdef(const char *n) {
  for (int i = 0; bdefs[i].g; i++)
    if (!strcmp(bdefs[i].g, n)) return &bdefs[i];
  return NULL;
}

static const char *join(ctx *c, const char **s, int n) {
  const char *l = "";
  for (int i = 0; i < n; i++) l = TS("%s%s%s", l, i ? ", " : "", s[i]);
  return l;
}

static G2C_NOINLINE ex tex_call(ctx *c, const bdef *b, ex *a, const char **s, int n) {
  gty st = a[0].t, pt = a[1].t, v4 = ty(B_FLOAT, 4, 1);
  if (st.arr || (st.b != B_S2D && st.b != B_SCUBE)) fail(c, "'%s' needs a sampler", b->g);
  int cube = st.b == B_SCUBE;
  if ((b->samp == 1 && cube) || (b->samp == 2 && !cube)) fail(c, "'%s' used with the wrong sampler type", b->g);
  if (pt.b != B_FLOAT || !is_num(pt) || pt.c != 1) fail(c, "'%s': bad coordinate type %s", b->g, tname(c, pt));
  for (int i = 2; i < n; i++)
    if (b->sub != TX_GRAD && !is_scalar(a[i].t)) fail(c, "'%s': bias/lod must be a float", b->g);
  if (b->sub != TX_PROJ && b->sub != TX_PROJLOD && pt.r != (cube ? 3 : 2))
    fail(c, "'%s': coordinate should be vec%d, got %s", b->g, cube ? 3 : 2, tname(c, pt));
  switch (b->sub) {
  case TX_PLAIN:
    if (n == 2) return mk(TS("%s(%s, %s)", cube ? "texCUBE" : "tex2D", s[0], s[1]), v4);
    if (cube) return mk(TS("texCUBEbias(%s, float4(%s, %s))", s[0], s[1], s[2]), v4);
    return mk(TS("tex2Dbias(%s, float4(%s, 0.0, %s))", s[0], s[1], s[2]), v4);
  case TX_LOD:
    if (cube) return mk(TS("texCUBElod(%s, float4(%s, %s))", s[0], s[1], s[2]), v4);
    return mk(TS("tex2Dlod(%s, float4(%s, 0.0, %s))", s[0], s[1], s[2]), v4);
  case TX_PROJ:
    if (n == 3) fail(c, "'%s' with bias is not supported", b->g);
    if (pt.r == 4) return mk(TS("tex2Dproj(%s, (%s).xyw)", s[0], s[1]), v4);
    if (pt.r == 3) return mk(TS("tex2Dproj(%s, %s)", s[0], s[1]), v4);
    break;
  case TX_PROJLOD:
    if (pt.r != 3 && pt.r != 4) break;
    need_projlod(c, pt.r);
    return mk(TS("_g2c_projlod(%s, %s, %s)", s[0], s[1], s[2]), v4);
  case TX_GRAD:
    return mk(TS("%s(%s, %s, %s, %s)", cube ? "texCUBEgrad" : "tex2Dgrad", s[0], s[1], s[2], s[3]), v4);
  }
  fail(c, "'%s': bad coordinate type %s", b->g, tname(c, pt));
}

static G2C_NOINLINE ex call_builtin(ctx *c, const bdef *b, ex *a, int n) {
  if (b->k == BI_UNSUP) fail(c, "builtin '%s' is not supported", b->g);
  if (n < b->mn || n > b->mx) fail(c, "wrong number of arguments to '%s'", b->g);
  const char *s[4];
  for (int i = 0; i < n; i++) {
    if (a[i].t.arr || a[i].t.b == B_STRUCT || a[i].t.b == B_VOID) fail(c, "bad argument to '%s'", b->g);
    s[i] = fin(c, &a[i]);
  }
  if (b->k == BI_TEX) return tex_call(c, b, a, s, n);
  for (int i = 0; i < n; i++)
    if (!is_num(a[i].t)) fail(c, "bad argument to '%s'", b->g);
  gty w = a[0].t;
  for (int i = 1; i < n; i++)
    if (!is_mat(a[i].t) && comps(a[i].t) > comps(w)) w = a[i].t;
  switch (b->k) {
  case BI_GENB:
  case BI_MOD:
    if (is_vec(w))
      for (int i = 0; i < n; i++)
        if (is_scalar(a[i].t)) s[i] = bcast(c, ty(a[i].t.b, w.r, 1), s[i]);
    if (b->k == BI_MOD) {
      if (w.b != B_FLOAT) fail(c, "mod() needs float arguments");
      need_mod(c, w);
      return mk(TS("_g2c_mod(%s, %s)", s[0], s[1]), w);
    }
    return mk(TS("%s(%s)", b->cg, join(c, s, n)), w);
  case BI_GEN:
    return mk(TS("%s(%s)", b->cg, join(c, s, n)), w);
  case BI_ATAN:
    if (n == 2) return mk(TS("atan2(%s, %s)", s[0], s[1]), w);
    return mk(TS("atan(%s)", s[0]), w);
  case BI_MIX:
    if (a[2].t.b == B_BOOL) return mk(TS("(%s ? %s : %s)", s[2], s[1], s[0]), a[0].t);
    if (is_vec(w))
      for (int i = 0; i < n; i++)
        if (is_scalar(a[i].t)) s[i] = bcast(c, w, s[i]);
    return mk(TS("lerp(%s, %s, %s)", s[0], s[1], s[2]), w);
  case BI_FLT:
    return mk(TS("%s(%s)", b->cg, join(c, s, n)), ty(B_FLOAT, 1, 1));
  case BI_RED:
    return mk(TS("%s(%s)", b->cg, s[0]), ty(B_BOOL, 1, 1));
  case BI_BVEC:
    return mk(TS("%s(%s)", b->cg, s[0]), ty(B_BOOL, a[0].t.r, 1));
  case BI_CMP:
    return mk(TS("(%s %s %s)", s[0], b->cg, s[1]), ty(B_BOOL, a[0].t.r, 1));
  case BI_NOT:
    return mk(TS("(!%s)", s[0]), a[0].t);
  case BI_CROSS:
    return mk(TS("cross(%s, %s)", s[0], s[1]), ty(B_FLOAT, 3, 1));
  case BI_MCM:
    return mk(TS("(%s * %s)", s[0], s[1]), a[0].t);
  case BI_TRANS:
    if (!is_mat(a[0].t)) fail(c, "transpose() needs a matrix");
    return mk(TS("transpose(%s)", s[0]), ty(B_FLOAT, a[0].t.c, a[0].t.r));
  default:
    break;
  }
  fail(c, "builtin '%s' is not supported", b->g);
}

static G2C_NOINLINE ex call_fn(ctx *c, const char *name, ex *a, int n) {
  fn *best = NULL;
  int cands = 0;
  for (fn *f = c->fns; f; f = f->next) {
    if (strcmp(f->name, name)) continue;
    cands++;
    if (f->np != n) continue;
    int ok = 1;
    for (int i = 0; i < n; i++)
      if (!teq(f->pt[i], a[i].t)) ok = 0;
    if (ok) { best = f; break; }
  }
  if (!cands) {
    const bdef *b = find_bdef(name);
    if (b) return call_builtin(c, b, a, n);
    fail(c, "unknown function '%s'", name);
  }
  if (!best && !STRICT) {
    // lenient: take the one overload with a matching argument count
    for (fn *f = c->fns; f; f = f->next) {
      if (strcmp(f->name, name) || f->np != n) continue;
      if (best) { best = NULL; break; }
      best = f;
    }
  }
  if (!best) fail(c, "no matching overload for '%s'", name);
  const char *l = "";
  for (int i = 0; i < n; i++) {
    if (best->pq[i] && (a[i].pn || !a[i].lval)) fail(c, "out argument %d of '%s' isn't writable", i + 1, name);
    l = TS("%s%s%s", l, i ? ", " : "", fin(c, &a[i]));
  }
  return mk(TS("%s(%s)", best->cg, l), best->ret);
}

static int parse_args(ctx *c, ex *args) {  // after '('
  int n = 0;
  if (accept(c, ")")) return 0;
  if (isid(pk(c, 0), "void") && isop(pk(c, 1), ")")) { c->p += 2; return 0; }
  for (;;) {
    if (n == MAX_ARGS) fail(c, "too many arguments");
    args[n++] = parse_assign(c);
    if (accept(c, ",")) continue;
    expect(c, ")");
    return n;
  }
}

static short parse_array_size(ctx *c) {  // after '['
  if (accept(c, "]")) return -1;
  ex e = parse_cond(c);
  if (!e.has_ival || e.ival <= 0 || e.ival > 4096) fail(c, "array size must be a positive integer constant");
  expect(c, "]");
  if (isop(pk(c, 0), "[")) fail(c, "arrays of arrays are not supported");
  return (short)e.ival;
}

// builtin variables
typedef struct { const char *name; unsigned char stage, b, r, arr, lval; const char *cg; unsigned bit; } bvar;
static const bvar bvars[] = {
  { "gl_Position", 1, B_FLOAT, 4, 0, 1, "_g2c_Position", U_POS },
  { "gl_PointSize", 1, B_FLOAT, 1, 0, 1, "_g2c_PointSize", U_PSIZE },
  { "gl_InstanceID", 1, B_INT, 1, 0, 0, "_g2c_InstanceID", U_IID },
  { "gl_VertexID", 1, B_INT, 1, 0, 0, "_g2c_VertexID", U_VID },
  { "gl_FragCoord", 2, B_FLOAT, 4, 0, 0, "_g2c_FragCoord", U_FCOORD },
  { "gl_FrontFacing", 2, B_BOOL, 1, 0, 0, "_g2c_FrontFacing", U_FACE },
  { "gl_PointCoord", 2, B_FLOAT, 2, 0, 0, "_g2c_PointCoord", U_PCOORD },
  { "gl_FragColor", 2, B_FLOAT, 4, 0, 1, "_g2c_FragColor", U_FCOLOR },
  { "gl_FragData", 2, B_FLOAT, 4, 1, 1, "_g2c_FragData", U_FDATA },
  { "gl_FragDepth", 2, B_FLOAT, 1, 0, 1, "_g2c_FragDepth", U_FDEPTH },
  { NULL, 0, 0, 0, 0, 0, NULL, 0 }
};

static G2C_NOINLINE ex builtin_var(ctx *c, const char *name) {
  for (int i = 0; bvars[i].name; i++) {
    const bvar *v = &bvars[i];
    if (strcmp(v->name, name)) continue;
    if (!(v->stage & (c->S->vs ? 1 : 2))) fail(c, "'%s' isn't available in this stage", name);
    c->S->used |= v->bit;
    gty t = ty(v->b, v->r, 1);
    t.arr = v->arr;
    ex e = mk(v->cg, t);
    e.lval = v->lval;
    return e;
  }
  if (!strcmp(name, "gl_MaxDrawBuffers")) return mk_int(c, 1);
  if (!strcmp(name, "gl_MaxVertexAttribs")) return mk_int(c, GLSL2CG_MAX_ATTRIBS);
  if (!strcmp(name, "gl_MaxVaryingVectors")) return mk_int(c, MAX_TEXCOORD);
  fail(c, "unsupported builtin variable '%s'", name);
}

static G2C_NOINLINE ex ident_expr(ctx *c, const char *name) {
  sym *s = lookup(c, name);
  if (!s) {
    if (!strncmp(name, "gl_", 3)) return builtin_var(c, name);
    fail(c, "undeclared identifier '%s'", name);
  }
  ex e = mk(s->cg, s->t);
  e.lval = s->kind != K_UNIFORM && s->kind != K_IN && !s->is_const;
  if (s->has_ival) { e.has_ival = 1; e.ival = s->ival; }
  if (s->kind == K_UNIFORM) {
    e.u = s;
    if (s->store == ST_MAT) e.pn = PN_MATU;
    else if (s->store == ST_SLOT || s->store == ST_MATARR) e.pn = PN_ARR;
  }
  return e;
}

static const char *norm_float(ctx *c, const char *s) {
  size_t n = strlen(s);
  if (n && (s[n - 1] == 'f' || s[n - 1] == 'F')) n--;
  int dot_end = n && s[n - 1] == '.';
  return TS("%s%.*s%s", s[0] == '.' ? "0" : "", (int)n, s, dot_end ? "0" : "");
}

static ex parse_primary(ctx *c) {
  tok *t = pk(c, 0);
  if (t->k == TK_INT) {
    c->p++;
    size_t n = strlen(t->s);
    const char *s = (t->s[n - 1] == 'u' || t->s[n - 1] == 'U') ? astrndup(c, &c->tmp, t->s, n - 1) : t->s;
    ex e = mk(s, ty(B_INT, 1, 1));
    e.has_ival = 1;
    e.ival = strtoll(s, NULL, 0);
    return e;
  }
  if (t->k == TK_FLT) {
    c->p++;
    return mk(norm_float(c, t->s), ty(B_FLOAT, 1, 1));
  }
  if (isop(t, "(")) {
    c->p++;
    ex e = parse_expr(c);
    expect(c, ")");
    const char *s = fin(c, &e);
    e.s = TS("(%s)", s);
    e.lval = 0;
    return e;
  }
  if (t->k != TK_ID) fail(c, "unexpected '%s'", t->s);
  if (!strcmp(t->s, "true") || !strcmp(t->s, "false")) {
    c->p++;
    return mk(t->s, ty(B_BOOL, 1, 1));
  }
  gty ct;
  if (type_kw(c, t->s, &ct)) {
    c->p++;
    if (accept(c, "[")) {
      // array constructor, only good for initializers
      short sz = parse_array_size(c);
      expect(c, "(");
      ex *args = amalloc(c, &c->tmp, sizeof(ex) * MAX_ARGS);
      int n = parse_args(c, args);
      if (sz > 0 && n != sz) fail(c, "array constructor has %d elements, expected %d", n, sz);
      if (!n) fail(c, "empty array constructor");
      const char *l = "";
      for (int i = 0; i < n; i++) {
        if (STRICT && !teq(args[i].t, ct)) fail(c, "array constructor element %d has the wrong type", i);
        l = TS("%s%s%s", l, i ? ", " : "", fin(c, &args[i]));
      }
      ct.arr = (short)n;
      ex e = mk(TS("{%s}", l), ct);
      e.pn = PN_INIT;
      return e;
    }
    expect(c, "(");
    ex *args = amalloc(c, &c->tmp, sizeof(ex) * MAX_ARGS);
    int n = parse_args(c, args);
    return construct(c, ct, args, n);
  }
  if (isop(pk(c, 1), "(")) {
    const char *name = t->s;
    c->p += 2;
    ex *args = amalloc(c, &c->tmp, sizeof(ex) * MAX_ARGS);
    int n = parse_args(c, args);
    int si = find_struct(c, name);
    if (si >= 0 && !lookup(c, name)) return struct_ctor(c, si, args, n);
    return call_fn(c, name, args, n);
  }
  c->p++;
  return ident_expr(c, t->s);
}

static G2C_NOINLINE ex index_expr(ctx *c, ex *a, ex *ix) {
  const char *is = fin(c, ix);
  if (STRICT && !(is_scalar(ix->t) && ix->t.b == B_INT)) fail(c, "array index must be an int");
  gty t = a->t, et;
  if (t.arr) { et = t; et.arr = 0; }
  else if (is_mat(t)) et = ty(B_FLOAT, t.r, 1);
  else if (is_vec(t)) et = ty(t.b, 1, 1);
  else fail(c, "indexing something that isn't an array, vector or matrix");
  if (t.arr > 0 && ix->has_ival && (ix->ival < 0 || ix->ival >= t.arr)) fail(c, "index %lld out of range", ix->ival);
  ex r;
  switch (a->pn) {
  case PN_ARR:
    if (a->u->store == ST_MATARR) {
      r = mk(a->s, et);
      r.pn = PN_MATEL;
      r.u = a->u;
      r.idx = is;
      return r;
    }
    r = mk(TS("%s[%s]", a->s, is), et);
    r.pn = PN_STORE;
    r.raw4 = comps(et) < 4;
    r.cast = et.b != B_FLOAT;
    return r;
  case PN_MATU:
    r = mk(TS("%s[%s]", a->s, is), et);
    r.pn = PN_STORE;
    r.raw4 = et.r < 4;
    return r;
  case PN_MATEL:
    r = mk(TS("%s[(%s) * %d + (%s)]", a->s, a->idx, a->u->t.c, is), et);
    r.pn = PN_STORE;
    r.raw4 = et.r < 4;
    return r;
  case PN_STORE:
    r = mk(TS("%s[%s]", a->s, is), et);
    r.pn = PN_STORE;
    r.cast = a->cast;
    return r;
  default:
    break;
  }
  r = mk(TS("%s[%s]", fin(c, a), is), et);
  r.lval = a->lval;
  return r;
}

static G2C_NOINLINE ex field_expr(ctx *c, ex *e, const char *f) {
  gty t = e->t;
  if (t.b == B_STRUCT && !t.arr) {
    const sdef *d = &c->structs[t.st];
    for (int i = 0; i < d->nm; i++) {
      if (strcmp(d->m[i].name, f)) continue;
      ex r = mk(TS("%s.%s", fin(c, e), d->m[i].cg), d->m[i].t);
      r.lval = e->lval;
      return r;
    }
    fail(c, "struct %s has no member '%s'", d->name, f);
  }
  if (!is_num(t) || t.c != 1) fail(c, "can't access field '%s' on %s", f, tname(c, t));
  static const char *const sets[3] = { "xyzw", "rgba", "stpq" };
  size_t n = strlen(f);
  if (n < 1 || n > 4) fail(c, "bad swizzle '.%s'", f);
  char sw[5];
  int set = -1, dup = 0;
  unsigned seen = 0;
  for (size_t i = 0; i < n; i++) {
    int k = -1, si = -1;
    for (int s = 0; s < 3 && k < 0; s++) {
      const char *q = strchr(sets[s], f[i]);
      if (q) { k = (int)(q - sets[s]); si = s; }
    }
    if (k < 0 || k >= t.r || (set >= 0 && si != set)) fail(c, "bad swizzle '.%s'", f);
    set = si;
    sw[i] = "xyzw"[k];
    if (seen & (1u << k)) dup = 1;
    seen |= 1u << k;
  }
  sw[n] = 0;
  gty rt = ty(t.b, (int)n, 1);
  if (e->pn == PN_STORE) {
    ex r = *e;
    r.s = TS("%s.%s", e->s, sw);
    r.t = rt;
    r.raw4 = 0;
    r.has_ival = 0;
    return r;
  }
  ex r = mk(TS("%s.%s", fin(c, e), sw), rt);
  r.lval = e->lval && !dup;
  return r;
}

static ex parse_postfix(ctx *c) {
  ex e = parse_primary(c);
  for (;;) {
    tok *t = pk(c, 0);
    if (isop(t, "[")) {
      c->p++;
      ex i = parse_expr(c);
      expect(c, "]");
      e = index_expr(c, &e, &i);
    } else if (isop(t, ".")) {
      c->p++;
      const char *f = expect_id(c);
      if (!strcmp(f, "length") && isop(pk(c, 0), "(")) {
        c->p++;
        expect(c, ")");
        int n = e.t.arr > 0 ? e.t.arr : is_mat(e.t) ? e.t.c : is_vec(e.t) ? e.t.r : 0;
        if (!n) fail(c, "length() on something that isn't an array");
        e = mk_int(c, n);
      } else {
        e = field_expr(c, &e, f);
      }
    } else if (isop(t, "++") || isop(t, "--")) {
      c->p++;
      if (e.pn || !e.lval) fail(c, "'%s' needs a writable value", t->s);
      e = mk(TS("(%s%s)", e.s, t->s), e.t);
    } else {
      return e;
    }
  }
}

static ex parse_unary(ctx *c) {
  tok *t = pk(c, 0);
  if (t->k == TK_OP && (isop(t, "++") || isop(t, "--") || isop(t, "-") || isop(t, "+") || isop(t, "!") || isop(t, "~"))) {
    c->p++;
    if (++c->edepth > MAX_EDEPTH) fail(c, "expression too deeply nested");
    ex e = parse_unary(c);
    c->edepth--;
    if (t->s[0] == t->s[1]) {  // ++ / --
      if (e.pn || !e.lval) fail(c, "'%s' needs a writable value", t->s);
      return mk(TS("(%s%s)", t->s, e.s), e.t);
    }
    const char *s = fin(c, &e);
    ex r;
    switch (t->s[0]) {
    case '+':
      return e;
    case '-':
      r = mk(TS("(-%s)", s), e.t);
      r.has_ival = e.has_ival;
      r.ival = -e.ival;
      return r;
    case '!':
      if (STRICT && !(is_scalar(e.t) && e.t.b == B_BOOL)) fail(c, "'!' needs a bool");
      return mk(TS("(!%s)", s), e.t);
    default:
      r = mk(TS("(~%s)", s), e.t);
      r.has_ival = e.has_ival;
      r.ival = ~e.ival;
      return r;
    }
  }
  return parse_postfix(c);
}

static int bin_prec(const tok *t) {
  static const char *const tab[] = {
    "||", "^^", "&&", "|", "^", "&", "==", "!=", "<", ">", "<=", ">=", "<<", ">>", "+", "-", "*", "/", "%", NULL
  };
  static const unsigned char pr[] = { 1, 2, 3, 4, 5, 6, 7, 7, 8, 8, 8, 8, 9, 9, 10, 10, 11, 11, 11 };
  if (t->k != TK_OP) return 0;
  for (int i = 0; tab[i]; i++)
    if (!strcmp(tab[i], t->s)) return pr[i];
  return 0;
}

static G2C_NOINLINE ex binop(ctx *c, const char *op, ex *a, ex *b) {
  const char *x = fin(c, a), *y = fin(c, b);
  gty ta = a->t, tb = b->t;
  gty B1 = ty(B_BOOL, 1, 1);
  int eqop = !strcmp(op, "==") || !strcmp(op, "!=");
  if (!is_num(ta) || !is_num(tb)) {
    if (eqop) fail(c, "comparing arrays/structs/samplers is not supported");
    fail(c, "bad operands to '%s' (%s, %s)", op, tname(c, ta), tname(c, tb));
  }
  if (!strcmp(op, "*") && (is_mat(ta) || is_mat(tb)) && !is_scalar(ta) && !is_scalar(tb)) {
    gty rt;
    if (is_mat(ta) && is_mat(tb)) {
      if (ta.c != tb.r) fail(c, "matrix size mismatch in '*'");
      rt = ty(B_FLOAT, ta.r, tb.c);
    } else if (is_mat(ta)) {
      if (tb.r != ta.c) fail(c, "matrix * vector size mismatch");
      rt = ty(B_FLOAT, ta.r, 1);
    } else {
      if (ta.r != tb.r) fail(c, "vector * matrix size mismatch");
      rt = ty(B_FLOAT, tb.c, 1);
    }
    return mk(TS("mul(%s, %s)", y, x), rt);
  }
  if (!strcmp(op, "&&") || !strcmp(op, "||") || !strcmp(op, "^^")) {
    if (STRICT && !(teq(ta, B1) && teq(tb, B1))) fail(c, "'%s' needs bools", op);
    if (op[0] == '^') return mk(TS("(%s != %s)", x, y), B1);
    return mk(TS("(%s %s %s)", x, op, y), B1);
  }
  if (eqop) {
    if (STRICT && !teq(ta, tb)) fail(c, "'%s' on %s and %s", op, tname(c, ta), tname(c, tb));
    if (is_mat(ta) || is_mat(tb)) fail(c, "matrix comparison is not supported");
    if (is_vec(ta) || is_vec(tb)) return mk(TS("%s(%s %s %s)", op[0] == '=' ? "all" : "any", x, op, y), B1);
    return mk(TS("(%s %s %s)", x, op, y), B1);
  }
  if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) {
    if (STRICT && !(is_scalar(ta) && teq(ta, tb))) fail(c, "'%s' on %s and %s", op, tname(c, ta), tname(c, tb));
    return mk(TS("(%s %s %s)", x, op, y), B1);
  }
  int shift = !strcmp(op, "<<") || !strcmp(op, ">>");
  gty rt = shift ? ta : is_scalar(ta) ? tb : ta;
  if (STRICT && !shift) {
    if (ta.b != tb.b) fail(c, "'%s' on %s and %s", op, tname(c, ta), tname(c, tb));
    if (!is_scalar(ta) && !is_scalar(tb) && !teq(ta, tb)) fail(c, "'%s' on %s and %s", op, tname(c, ta), tname(c, tb));
  }
  ex r = mk(TS("(%s %s %s)", x, op, y), rt);
  if (a->has_ival && b->has_ival && is_scalar(ta) && ta.b == B_INT && is_scalar(tb) && tb.b == B_INT) {
    long long u = a->ival, v = b->ival;
    int ok = 1;
    long long w = 0;
    switch (op[0]) {
    case '+': w = u + v; break;
    case '-': w = u - v; break;
    case '*': w = u * v; break;
    case '/': if (v) w = u / v; else ok = 0; break;
    case '%': if (v) w = u % v; else ok = 0; break;
    case '&': w = u & v; break;
    case '|': w = u | v; break;
    case '^': w = u ^ v; break;
    case '<': w = (v >= 0 && v < 63) ? u << v : 0; break;
    case '>': w = (v >= 0 && v < 63) ? u >> v : 0; break;
    default: ok = 0;
    }
    r.has_ival = (unsigned char)ok;
    r.ival = w;
  }
  return r;
}

static ex parse_bin(ctx *c, int minp) {
  ex l = parse_unary(c);
  for (;;) {
    tok *t = pk(c, 0);
    int p = bin_prec(t);
    if (!p || p < minp) return l;
    c->p++;
    ex r = parse_bin(c, p + 1);
    l = binop(c, t->s, &l, &r);
  }
}

static ex parse_cond(ctx *c) {
  ex e = parse_bin(c, 1);
  if (!isop(pk(c, 0), "?")) return e;
  c->p++;
  ex a = parse_expr(c);
  expect(c, ":");
  ex b = parse_assign(c);
  const char *cs = fin(c, &e), *as = fin(c, &a), *bs = fin(c, &b);
  if (STRICT && !teq(e.t, ty(B_BOOL, 1, 1))) fail(c, "?: condition must be a bool");
  if (STRICT && !teq(a.t, b.t)) fail(c, "?: branches have different types");
  return mk(TS("(%s ? %s : %s)", cs, as, bs), a.t);
}

static const char *const asg_ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|=", NULL };

static ex parse_assign(ctx *c) {
  if (++c->edepth > MAX_EDEPTH) fail(c, "expression too deeply nested");
  ex l = parse_cond(c);
  tok *t = pk(c, 0);
  int k = -1;
  if (t->k == TK_OP)
    for (int i = 0; asg_ops[i]; i++)
      if (!strcmp(asg_ops[i], t->s)) k = i;
  if (k >= 0) {
    c->p++;
    ex r = parse_assign(c);
    if (l.pn || !l.lval) fail(c, "left side of '%s' isn't writable", t->s);
    const char *rs = fin(c, &r);
    if (k == 0) {
      if (r.pn == PN_INIT) fail(c, "array constructors are only supported as initializers");
      if (STRICT && !teq(l.t, r.t)) fail(c, "assigning %s to %s", tname(c, r.t), tname(c, l.t));
      l = mk(TS("%s = %s", l.s, rs), l.t);
    } else if (k == 3 && (is_mat(l.t) || is_mat(r.t)) && !is_scalar(r.t)) {
      // m *= n  ->  m = m * n  ->  mul(n, m), same for v *= m
      l = mk(TS("%s = mul(%s, %s)", l.s, rs, l.s), l.t);
    } else {
      if (STRICT && (l.t.b != r.t.b || (!is_scalar(r.t) && !teq(l.t, r.t))))
        fail(c, "'%s' on %s and %s", t->s, tname(c, l.t), tname(c, r.t));
      l = mk(TS("%s %s %s", l.s, t->s, rs), l.t);
    }
  }
  c->edepth--;
  return l;
}

static ex parse_expr(ctx *c) {
  ex e = parse_assign(c);
  while (accept(c, ",")) {
    ex r = parse_assign(c);
    const char *a = fin(c, &e), *b = fin(c, &r);
    e = mk(TS("%s, %s", a, b), r.t);
  }
  return e;
}

// ---- declarations ----

static int parse_struct(ctx *c) {  // after 'struct'
  const char *name = NULL;
  if (pk(c, 0)->k == TK_ID) name = expect_id(c);
  expect(c, "{");
  member *m = amalloc(c, &c->perm, sizeof(member) * MAX_MEMBERS);
  int nm = 0;
  while (!accept(c, "}")) {
    while (pk(c, 0)->k == TK_ID && is_prec(pk(c, 0)->s)) c->p++;
    gty bt = parse_type(c, 0);
    if (bt.b == B_VOID || bt.b == B_S2D || bt.b == B_SCUBE) fail(c, "bad struct member type");
    for (;;) {
      const char *mn = expect_id(c);
      gty mt = bt;
      if (accept(c, "[")) {
        if (mt.arr) fail(c, "arrays of arrays are not supported");
        mt.arr = parse_array_size(c);
      }
      if (mt.arr < 0) fail(c, "unsized struct member");
      if (nm == MAX_MEMBERS) fail(c, "struct has too many members");
      for (int i = 0; i < nm; i++)
        if (!strcmp(m[i].name, mn)) fail(c, "duplicate struct member '%s'", mn);
      m[nm].name = mn;
      m[nm].cg = is_cg_reserved(mn) ? PS("g2cm_%s", mn) : mn;
      m[nm].t = mt;
      nm++;
      if (accept(c, ",")) continue;
      expect(c, ";");
      break;
    }
  }
  if (!nm) fail(c, "empty struct");
  if (c->nstructs == c->capstructs) {
    int cap = c->capstructs ? c->capstructs * 2 : 16;
    sdef *ns = amalloc(c, &c->perm, sizeof(sdef) * (size_t)cap);
    if (c->nstructs) memcpy(ns, c->structs, sizeof(sdef) * (size_t)c->nstructs);
    c->structs = ns;
    c->capstructs = cap;
  }
  sdef *d = &c->structs[c->nstructs];
  d->name = name ? name : PS("anon%d", c->uniq + 1);
  d->cg = name ? global_cg(c, name) : unique(c, "anon");
  d->m = m;
  d->nm = nm;
  int si = c->nstructs++;
  sb *o = &c->scratch;
  sb_clear(o);
  sb_printf(c, o, "struct %s {\n", d->cg);
  for (int i = 0; i < nm; i++) sb_printf(c, o, "\t%s %s%s;\n", cgty(c, m[i].t), m[i].cg, arrsuf(c, m[i].t));
  sb_puts(c, o, "};\n");
  // Cg has no struct constructors, so make one
  sb_printf(c, o, "%s _g2c_mk_%s(", d->cg, d->cg);
  for (int i = 0; i < nm; i++) sb_printf(c, o, "%s%s _p%d%s", i ? ", " : "", cgty(c, m[i].t), i, arrsuf(c, m[i].t));
  sb_printf(c, o, ") {\n\t%s r;\n", d->cg);
  for (int i = 0; i < nm; i++) sb_printf(c, o, "\tr.%s = _p%d;\n", m[i].cg, i);
  sb_puts(c, o, "\treturn r;\n}\n");
  emit_global_text(c, o->p);
  return si;
}

static gty parse_type(ctx *c, int allow_struct) {
  while (pk(c, 0)->k == TK_ID && is_prec(pk(c, 0)->s)) c->p++;
  tok *t = pk(c, 0);
  gty r;
  if (t->k != TK_ID) fail(c, "expected a type near '%s'", t->s);
  if (!strcmp(t->s, "struct")) {
    if (!allow_struct) fail(c, "struct definition not allowed here");
    c->p++;
    r = ty(B_STRUCT, 1, 1);
    r.st = (short)parse_struct(c);
  } else if (type_kw(c, t->s, &r)) {
    c->p++;
  } else {
    int si = find_struct(c, t->s);
    if (si < 0) fail(c, "unknown type '%s'", t->s);
    c->p++;
    r = ty(B_STRUCT, 1, 1);
    r.st = (short)si;
  }
  if (accept(c, "[")) r.arr = parse_array_size(c);
  return r;
}

enum { Q_NONE, Q_UNIFORM, Q_IN, Q_OUT, Q_INOUT, Q_ATTR, Q_VARYING };
typedef struct { int storage, loc, is_const; } quals;

static void parse_quals(ctx *c, quals *q) {
  q->storage = Q_NONE;
  q->loc = -1;
  q->is_const = 0;
  for (;;) {
    tok *t = pk(c, 0);
    if (t->k != TK_ID) return;
    const char *s = t->s;
    if (!strcmp(s, "layout")) {
      c->p++;
      expect(c, "(");
      for (;;) {
        const char *id = expect_id(c);
        long long v = -1;
        if (accept(c, "=")) {
          tok *n = pk(c, 0);
          if (n->k != TK_INT) fail(c, "layout value must be an integer");
          v = strtoll(n->s, NULL, 0);
          c->p++;
        }
        if (!strcmp(id, "location")) q->loc = (int)v;
        else if (!strcmp(id, "binding")) fail(c, "layout(binding) is not supported");
        if (accept(c, ",")) continue;
        expect(c, ")");
        break;
      }
      continue;
    }
    int st = !strcmp(s, "uniform") ? Q_UNIFORM : !strcmp(s, "in") ? Q_IN : !strcmp(s, "out") ? Q_OUT
           : !strcmp(s, "inout") ? Q_INOUT : !strcmp(s, "attribute") ? Q_ATTR : !strcmp(s, "varying") ? Q_VARYING : -1;
    if (st >= 0) {
      q->storage = st;
      c->p++;
      continue;
    }
    if (!strcmp(s, "const")) {
      q->is_const = 1;
      c->p++;
      continue;
    }
    if (!strcmp(s, "centroid") || !strcmp(s, "flat") || !strcmp(s, "smooth") || !strcmp(s, "noperspective") ||
        !strcmp(s, "invariant") || is_prec(s)) {
      c->p++;
      continue;
    }
    return;
  }
}

static void add_uniform(ctx *c, const char *name, const char *cgn, gty t, int slots) {
  g2c_result *r = c->res;
  g2c_type gt = gtype_enum(t);
  int asz = t.arr ? t.arr : 1;
  for (int i = 0; i < r->n_uniforms; i++) {
    g2c_uniform *u = &r->uniforms[i];
    if (strcmp(u->name, name)) continue;
    if (u->type != gt || u->array_size != asz || u->vec4_slots != slots)
      fail(c, "uniform '%s' is declared differently in the two stages", name);
    if (c->S->vs) u->in_vs = 1;
    else u->in_fs = 1;
    return;
  }
  if (r->n_uniforms >= GLSL2CG_MAX_UNIFORMS) fail(c, "too many uniforms");
  g2c_uniform *u = &r->uniforms[r->n_uniforms++];
  memset(u, 0, sizeof *u);
  strcpy(u->name, name);
  strcpy(u->cg_name, cgn);
  u->type = gt;
  u->array_size = asz;
  u->vec4_slots = slots;
  if (c->S->vs) u->in_vs = 1;
  else u->in_fs = 1;
}

static void declare_uniform(ctx *c, const char *name, gty t) {
  if (t.b == B_STRUCT) fail(c, "uniform '%s': struct uniforms are not supported", name);
  if (t.arr < 0) fail(c, "uniform '%s': unsized array", name);
  if ((t.b == B_S2D || t.b == B_SCUBE) && t.arr) fail(c, "uniform '%s': sampler arrays are not supported", name);
  const char *cgn = api_cg_name(c, name, "uniform");
  sym *s = add_sym(c, name, cgn, t, K_UNIFORM);
  int n = t.arr ? t.arr : 1, slots = 1;
  if (t.c > 1) {
    // matrices: one float4 per column, rebuilt in the shader
    slots = n * t.c;
    s->store = t.arr ? ST_MATARR : ST_MAT;
    OUT("uniform float4 %s[%d];\n", cgn, slots);
    if (t.arr) {
      const char *sw = t.r == 4 ? "" : t.r == 3 ? ".xyz" : ".xy";
      const char *l = "";
      for (int j = 0; j < t.c; j++) l = TS("%s%s%s[i * %d + %d]%s", l, j ? ", " : "", cgn, t.c, j, sw);
      OUT("float%dx%d _g2c_mu_%s(int i) { return float%dx%d(%s); }\n", t.c, t.r, name, t.c, t.r, l);
    }
  } else if (t.arr) {
    slots = n;
    s->store = (t.b == B_FLOAT && t.r == 4) ? ST_NATIVE : ST_SLOT;
    OUT("uniform float4 %s[%d];\n", cgn, n);
  } else {
    OUT("uniform %s %s;\n", cgty(c, t), cgn);
  }
  add_uniform(c, name, cgn, t, slots);
}

static void declare_attrib(ctx *c, const char *name, gty t, int loc) {
  stage *st = c->S;
  if (t.arr || t.c > 1 || (t.b != B_FLOAT && t.b != B_INT))
    fail(c, "attribute '%s': unsupported type %s", name, tname(c, t));
  const char *cgn = api_cg_name(c, name, "attribute");
  if (st->nin >= MAX_IO || c->res->n_attribs >= GLSL2CG_MAX_ATTRIBS) fail(c, "too many attributes");
  sym *s = add_sym(c, name, PS("_g2c_in_%s", name), t, K_IN);
  iovar *v = &st->in[st->nin++];
  v->name = cgn;  // for attribs this is only the entry param name
  v->cg = s->cg;
  v->t = t;
  v->loc = loc;
  v->tc = -1;
  g2c_attrib *a = &c->res->attribs[c->res->n_attribs++];
  memset(a, 0, sizeof *a);
  strcpy(a->name, name);
  strcpy(a->cg_name, cgn);
  a->type = gtype_enum(t);
  a->location = loc;
  OUT("static %s %s;\n", cgty(c, t), s->cg);
}

static void declare_varying(ctx *c, const char *name, gty t, int is_out) {
  stage *st = c->S;
  if (t.arr || t.c > 1 || (t.b != B_FLOAT && t.b != B_INT))
    fail(c, "varying '%s': unsupported type %s (arrays/matrices/structs aren't supported)", name, tname(c, t));
  iovar *list = is_out ? st->out : st->in;
  int *n = is_out ? &st->nout : &st->nin;
  if (*n >= MAX_IO) fail(c, "too many varyings");
  sym *s = add_sym(c, name, PS("_g2c_v_%s", name), t, is_out ? K_OUT : K_IN);
  iovar *v = &list[(*n)++];
  v->name = name;
  v->cg = s->cg;
  v->t = t;
  v->loc = -1;
  v->tc = -1;
  OUT("static %s %s;\n", cgty(c, t), s->cg);
}

static void declare_fragout(ctx *c, const char *name, gty t, int loc) {
  stage *st = c->S;
  if (loc > 0) fail(c, "fragment output '%s' uses location %d, only 0 is supported (no MRT)", name, loc);
  if (st->nout) fail(c, "multiple fragment outputs are not supported");
  if (t.arr || t.c > 1 || t.b != B_FLOAT) fail(c, "fragment output '%s': unsupported type %s", name, tname(c, t));
  sym *s = add_sym(c, name, PS("_g2c_o_%s", name), t, K_OUT);
  iovar *v = &st->out[st->nout++];
  v->name = name;
  v->cg = s->cg;
  v->t = t;
  v->loc = 0;
  v->tc = -1;
  OUT("static %s %s;\n", cgty(c, t), s->cg);
}

static const char *decl_init(ctx *c, gty *t, ex *e) {
  if (e->pn == PN_INIT) {
    if (!t->arr) fail(c, "array initializer for a non-array");
    gty et = e->t, dt = *t;
    et.arr = dt.arr = 0;
    if (STRICT && !teq(et, dt)) fail(c, "initializer type mismatch");
    if (t->arr < 0) t->arr = e->t.arr;
    else if (t->arr != e->t.arr) fail(c, "array size mismatch in initializer");
    return e->s;
  }
  const char *s = fin(c, e);
  if (t->arr < 0 && e->t.arr > 0) t->arr = e->t.arr;
  if (STRICT && !teq(*t, e->t)) fail(c, "can't initialize %s with %s", tname(c, *t), tname(c, e->t));
  return s;
}

static void global_vars(ctx *c, gty bt, const quals *q) {
  int vs = c->S->vs;
  for (;;) {
    const char *name = expect_id(c);
    gty t = bt;
    if (accept(c, "[")) {
      if (t.arr) fail(c, "arrays of arrays are not supported");
      t.arr = parse_array_size(c);
    }
    if (t.b == B_VOID) fail(c, "void variable '%s'", name);
    int st = q->storage;
    if (st != Q_NONE && isop(pk(c, 0), "=")) fail(c, "initializer on '%s' isn't supported", name);
    if (st == Q_UNIFORM) {
      declare_uniform(c, name, t);
    } else if (st == Q_INOUT) {
      fail(c, "'inout' at global scope");
    } else if (st == Q_ATTR || (vs && st == Q_IN)) {
      if (!vs) fail(c, "'attribute' in a fragment shader");
      declare_attrib(c, name, t, q->loc);
    } else if (st == Q_VARYING || st == Q_IN || st == Q_OUT) {
      if (!vs && st == Q_OUT) declare_fragout(c, name, t, q->loc);
      else declare_varying(c, name, t, vs);
    } else {
      const char *init = NULL;
      ex e;
      memset(&e, 0, sizeof e);
      if (accept(c, "=")) {
        e = parse_assign(c);
        init = decl_init(c, &t, &e);
      }
      if (t.arr < 0) fail(c, "unsized array '%s'", name);
      if (t.b == B_S2D || t.b == B_SCUBE) fail(c, "sampler '%s' must be a uniform", name);
      if (q->is_const && !init) fail(c, "const '%s' needs an initializer", name);
      sym *s = add_sym(c, name, global_cg(c, name), t, K_VAR);
      s->is_const = (unsigned char)q->is_const;
      if (q->is_const && e.has_ival && teq(t, ty(B_INT, 1, 1))) {
        s->has_ival = 1;
        s->ival = e.ival;
      }
      // Cg treats plain globals as uniforms, hence static
      OUT("static %s%s %s%s%s%s;\n", q->is_const ? "const " : "", cgty(c, t), s->cg, arrsuf(c, t),
          init ? " = " : "", init ? init : "");
    }
    if (accept(c, ",")) continue;
    expect(c, ";");
    return;
  }
}

// ---- statements ----

static void stmt(ctx *c);

static int is_decl_start(ctx *c) {
  tok *t = pk(c, 0);
  if (t->k != TK_ID) return 0;
  const char *s = t->s;
  if (!strcmp(s, "const") || is_prec(s) || !strcmp(s, "struct") || !strcmp(s, "precision")) return 1;
  gty g;
  if (!type_kw(c, s, &g) && (find_struct(c, s) < 0 || lookup(c, s))) return 0;
  tok *n = pk(c, 1);
  if (n->k == TK_ID) return 1;
  if (isop(n, "[")) {  // T[N] name vs T[N](...)
    int i = 2, lvl = 1;
    while (lvl) {
      tok *x = pk(c, i++);
      if (x->k == TK_EOF) return 0;
      if (isop(x, "[")) lvl++;
      else if (isop(x, "]")) lvl--;
    }
    return pk(c, i)->k == TK_ID;
  }
  return 0;
}

static void local_decl(ctx *c) {
  if (isid(pk(c, 0), "precision")) { skip_to_semi(c); return; }
  int is_const = 0;
  for (;;) {
    tok *t = pk(c, 0);
    if (isid(t, "const")) { is_const = 1; c->p++; }
    else if (t->k == TK_ID && is_prec(t->s)) c->p++;
    else break;
  }
  gty bt = parse_type(c, 1);
  if (accept(c, ";")) return;
  for (;;) {
    const char *name = expect_id(c);
    gty t = bt;
    if (accept(c, "[")) {
      if (t.arr) fail(c, "arrays of arrays are not supported");
      t.arr = parse_array_size(c);
    }
    const char *init = NULL;
    ex e;
    memset(&e, 0, sizeof e);
    if (accept(c, "=")) {
      e = parse_assign(c);
      init = decl_init(c, &t, &e);
    }
    if (t.arr < 0) fail(c, "unsized array '%s'", name);
    if (t.b == B_VOID || t.b == B_S2D || t.b == B_SCUBE) fail(c, "bad type for local '%s'", name);
    if (is_const && !init) fail(c, "const '%s' needs an initializer", name);
    sym *s = add_sym(c, name, local_cg(c, name), t, K_VAR);
    s->is_const = (unsigned char)is_const;
    if (is_const && e.has_ival && teq(t, ty(B_INT, 1, 1))) {
      s->has_ival = 1;
      s->ival = e.ival;
    }
    ind(c);
    OUT("%s%s %s%s%s%s;\n", is_const ? "const " : "", cgty(c, t), s->cg, arrsuf(c, t), init ? " = " : "", init ? init : "");
    if (accept(c, ",")) continue;
    expect(c, ";");
    return;
  }
}

static void block(ctx *c) {
  expect(c, "{");
  ind(c);
  OUT("{\n");
  c->indent++;
  push_scope(c);
  while (!accept(c, "}")) {
    if (pk(c, 0)->k == TK_EOF) fail(c, "missing '}'");
    stmt(c);
  }
  pop_scope(c);
  c->indent--;
  ind(c);
  OUT("}\n");
}

// body of if/for/while always gets braces
static void substmt(ctx *c) {
  if (isop(pk(c, 0), "{")) { block(c); return; }
  ind(c);
  OUT("{\n");
  c->indent++;
  push_scope(c);
  stmt(c);
  pop_scope(c);
  c->indent--;
  ind(c);
  OUT("}\n");
}

static const char *cond_text(ctx *c, ex *e) {
  const char *s = fin(c, e);
  if (STRICT && !teq(e->t, ty(B_BOOL, 1, 1))) fail(c, "condition must be a bool, got %s", tname(c, e->t));
  return s;
}

static void stmt(ctx *c) {
  amark m = amark_get(&c->tmp);
  if (++c->sdepth > MAX_SDEPTH) fail(c, "statements nested too deeply");
  tok *t = pk(c, 0);
  if (isop(t, "{")) {
    block(c);
  } else if (isop(t, ";")) {
    c->p++;
  } else if (isid(t, "if")) {
    c->p++;
    expect(c, "(");
    ex e = parse_expr(c);
    expect(c, ")");
    const char *s = cond_text(c, &e);
    ind(c);
    OUT("if (%s)\n", s);
    substmt(c);
    if (isid(pk(c, 0), "else")) {
      c->p++;
      ind(c);
      OUT("else\n");
      substmt(c);
    }
  } else if (isid(t, "for")) {
    c->p++;
    expect(c, "(");
    // wrap in a block so a declared counter can't clash with anything
    ind(c);
    OUT("{\n");
    c->indent++;
    push_scope(c);
    const char *init = "";
    if (accept(c, ";")) {
    } else if (is_decl_start(c)) {
      local_decl(c);
    } else {
      ex e = parse_expr(c);
      expect(c, ";");
      init = fin(c, &e);
    }
    const char *cond = "", *inc = "";
    if (!isop(pk(c, 0), ";")) {
      if (is_decl_start(c)) fail(c, "declarations in a for condition are not supported");
      ex e = parse_expr(c);
      cond = cond_text(c, &e);
    }
    expect(c, ";");
    if (!isop(pk(c, 0), ")")) {
      ex e = parse_expr(c);
      inc = fin(c, &e);
    }
    expect(c, ")");
    ind(c);
    OUT("for (%s; %s; %s)\n", init, cond, inc);
    substmt(c);
    pop_scope(c);
    c->indent--;
    ind(c);
    OUT("}\n");
  } else if (isid(t, "while")) {
    c->p++;
    expect(c, "(");
    if (is_decl_start(c)) fail(c, "declarations in a while condition are not supported");
    ex e = parse_expr(c);
    expect(c, ")");
    const char *s = cond_text(c, &e);
    ind(c);
    OUT("while (%s)\n", s);
    substmt(c);
  } else if (isid(t, "do")) {
    c->p++;
    ind(c);
    OUT("do\n");
    substmt(c);
    if (!isid(pk(c, 0), "while")) fail(c, "expected 'while' after do body");
    c->p++;
    expect(c, "(");
    ex e = parse_expr(c);
    expect(c, ")");
    expect(c, ";");
    const char *s = cond_text(c, &e);
    ind(c);
    OUT("while (%s);\n", s);
  } else if (isid(t, "return")) {
    c->p++;
    ind(c);
    if (accept(c, ";")) {
      if (STRICT && c->fn_ret.b != B_VOID) fail(c, "missing return value");
      OUT("return;\n");
    } else {
      ex e = parse_expr(c);
      expect(c, ";");
      const char *s = fin(c, &e);
      if (STRICT && !teq(e.t, c->fn_ret)) fail(c, "returning %s from a %s function", tname(c, e.t), tname(c, c->fn_ret));
      OUT("return %s;\n", s);
    }
  } else if (isid(t, "break") || isid(t, "continue") || isid(t, "discard")) {
    c->p++;
    expect(c, ";");
    ind(c);
    OUT("%s;\n", t->s);
  } else if (isid(t, "switch") || isid(t, "case") || isid(t, "default")) {
    fail(c, "switch statements are not supported");
  } else if (is_decl_start(c)) {
    local_decl(c);
  } else {
    ex e = parse_expr(c);
    expect(c, ";");
    const char *s = fin(c, &e);
    ind(c);
    OUT("%s;\n", s);
  }
  c->sdepth--;
  amark_release(&c->tmp, m);
}

static void parse_function(ctx *c, gty ret, const char *name) {  // at '('
  expect(c, "(");
  gty pt[MAX_ARGS];
  unsigned char pq[MAX_ARGS];
  const char *pn[MAX_ARGS];
  int np = 0;
  if (isid(pk(c, 0), "void") && isop(pk(c, 1), ")")) c->p++;
  if (!accept(c, ")")) {
    for (;;) {
      int q = 0;
      for (;;) {
        tok *t = pk(c, 0);
        if (isid(t, "const") || isid(t, "in") || (t->k == TK_ID && is_prec(t->s))) c->p++;
        else if (isid(t, "out")) { q = 1; c->p++; }
        else if (isid(t, "inout")) { q = 2; c->p++; }
        else break;
      }
      gty t = parse_type(c, 0);
      const char *n = NULL;
      if (pk(c, 0)->k == TK_ID) n = expect_id(c);
      if (accept(c, "[")) {
        if (t.arr) fail(c, "arrays of arrays are not supported");
        t.arr = parse_array_size(c);
      }
      if (t.arr < 0) fail(c, "unsized array parameter");
      if (t.b == B_VOID) fail(c, "void parameter");
      if (np == MAX_ARGS) fail(c, "too many parameters");
      pt[np] = t;
      pq[np] = (unsigned char)q;
      pn[np] = n;
      np++;
      if (accept(c, ",")) continue;
      expect(c, ")");
      break;
    }
  }
  if (ret.arr) fail(c, "functions returning arrays are not supported");
  int is_main = !strcmp(name, "main");
  if (is_main && (np || ret.b != B_VOID)) fail(c, "main must be 'void main()'");
  fn *f = NULL;
  for (fn *g = c->fns; g; g = g->next) {
    if (strcmp(g->name, name) || g->np != np) continue;
    int ok = 1;
    for (int i = 0; i < np; i++)
      if (!teq(g->pt[i], pt[i])) ok = 0;
    if (ok) { f = g; break; }
  }
  if (!f) {
    if (find_bdef(name)) fail(c, "redefining builtin function '%s'", name);
    f = amalloc(c, &c->perm, sizeof *f);
    memset(f, 0, sizeof *f);
    f->name = name;
    f->ret = ret;
    f->np = np;
    f->pt = amalloc(c, &c->perm, sizeof(gty) * (size_t)(np ? np : 1));
    f->pq = amalloc(c, &c->perm, (size_t)(np ? np : 1));
    if (np) {
      memcpy(f->pt, pt, sizeof(gty) * (size_t)np);
      memcpy(f->pq, pq, (size_t)np);
    }
    const char *cg = NULL;
    for (fn *g = c->fns; g; g = g->next)
      if (!strcmp(g->name, name)) cg = g->cg;  // overloads share a name
    if (lookup(c, name)) fail(c, "'%s' is already a variable", name);
    f->cg = cg ? cg : is_main ? "_g2c_main" : global_cg(c, name);
    f->next = c->fns;
    c->fns = f;
  } else if (!teq(f->ret, ret)) {
    fail(c, "conflicting return type for '%s'", name);
  }
  if (accept(c, ";")) {
    OUT("%s %s(", cgty(c, ret), f->cg);
    for (int i = 0; i < np; i++)
      OUT("%s%s%s _g2c_p%d%s", i ? ", " : "", pq[i] == 1 ? "out " : pq[i] == 2 ? "inout " : "", cgty(c, pt[i]), i,
          arrsuf(c, pt[i]));
    OUT(");\n");
    return;
  }
  if (f->defined) fail(c, "function '%s' redefined", name);
  f->defined = 1;
  if (is_main) c->S->have_main = 1;
  c->in_fn = 1;
  c->fn_start = c->S->body.n;
  c->fn_ret = ret;
  c->nfnames = 0;
  push_scope(c);
  OUT("%s %s(", cgty(c, ret), f->cg);
  for (int i = 0; i < np; i++) {
    const char *cg;
    if (pn[i]) {
      cg = local_cg(c, pn[i]);
      sym *s = add_sym(c, pn[i], cg, pt[i], K_VAR);
      (void)s;
    } else {
      cg = PS("_g2c_p%d", i);
    }
    OUT("%s%s%s %s%s", i ? ", " : "", pq[i] == 1 ? "out " : pq[i] == 2 ? "inout " : "", cgty(c, pt[i]), cg,
        arrsuf(c, pt[i]));
  }
  OUT(")\n");
  block(c);
  pop_scope(c);
  c->in_fn = 0;
  OUT("\n");
}

static void parse_external(ctx *c) {
  tok *t = pk(c, 0);
  if (accept(c, ";")) return;
  if (isid(t, "precision")) { skip_to_semi(c); return; }
  if (isid(t, "invariant") && pk(c, 1)->k == TK_ID && (isop(pk(c, 2), ";") || isop(pk(c, 2), ","))) {
    skip_to_semi(c);
    return;
  }
  quals q;
  parse_quals(c, &q);
  if (accept(c, ";")) return;
  if (q.storage != Q_NONE && pk(c, 0)->k == TK_ID && isop(pk(c, 1), "{")) fail(c, "interface blocks are not supported");
  gty ty_ = parse_type(c, 1);
  if (accept(c, ";")) return;
  tok *n = pk(c, 0);
  if (n->k == TK_ID && isop(pk(c, 1), "(")) {
    if (q.storage != Q_NONE) fail(c, "storage qualifier on a function");
    c->p++;
    parse_function(c, ty_, n->s);
    return;
  }
  global_vars(c, ty_, &q);
}

static void parse_stage(ctx *c, int idx, const char *src) {
  stage *st = &c->st[idx];
  st->vs = idx == 0;
  c->S = st;
  c->macros = NULL;
  c->syms = NULL;
  c->depth = 0;
  c->nstructs = 0;
  c->fns = NULL;
  c->nhkeys = 0;
  c->in_fn = 0;
  c->indent = 0;
  c->edepth = c->sdepth = 0;
  c->tk = NULL;
  c->pp_line = 0;
  define_int(c, "GL_ES", 1);
  define_int(c, "__VERSION__", 300);
  define_int(c, "GL_FRAGMENT_PRECISION_HIGH", 1);
  tlist raw = { 0 }, pp = { 0 };
  lex(c, src, &raw);
  preprocess(c, &raw, &pp);
  tok eof;
  memset(&eof, 0, sizeof eof);
  eof.k = TK_EOF;
  eof.s = "end of input";
  eof.line = pp.n ? pp.v[pp.n - 1].line : 1;
  tl_push(c, &pp, eof);
  c->tk = pp.v;
  c->ntk = pp.n;
  c->p = 0;
  while (pk(c, 0)->k != TK_EOF) {
    amark m = amark_get(&c->tmp);
    parse_external(c);
    amark_release(&c->tmp, m);
  }
  for (fn *f = c->fns; f; f = f->next)
    if (!f->defined) fail(c, "function '%s' declared but never defined", f->name);
  if (!st->have_main) fail(c, "no main() found");
  c->tk = NULL;
  c->ntk = 0;
  c->pp_line = 0;
}

// ---- linking + main() generation ----

static iovar *find_io(iovar *v, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (!strcmp(v[i].name, name)) return &v[i];
  return NULL;
}

static void link_varyings(ctx *c, stage *vs, stage *fs) {
  int tc = 0;
  if (vs && fs) {
    for (int i = 0; i < fs->nin; i++) {
      iovar *f = &fs->in[i], *o = find_io(vs->out, vs->nout, f->name);
      if (!o) fail(c, "varying '%s' is read by the fragment shader but never written by the vertex shader", f->name);
      if (!teq(o->t, f->t)) fail(c, "varying '%s' has different types in the two stages", f->name);
    }
    for (int i = 0; i < vs->nout; i++) {
      iovar *o = &vs->out[i], *f = find_io(fs->in, fs->nin, o->name);
      if (f) o->tc = f->tc = tc++;
    }
  } else if (vs) {
    for (int i = 0; i < vs->nout; i++) vs->out[i].tc = tc++;
  } else if (fs) {
    for (int i = 0; i < fs->nin; i++) fs->in[i].tc = tc++;
  }
  if (tc > MAX_TEXCOORD) fail(c, "too many varyings (%d, the Vita only has TEXCOORD0..9)", tc);
}

static void add_param(ctx *c, sb *ps, const char *fmt, ...) G2C_FMT(3, 4);
static void add_param(ctx *c, sb *ps, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (ps->n) sb_puts(c, ps, ",\n");
  sb_puts(c, ps, "\t");
  sb_reserve(c, ps, (size_t)n);
  vsnprintf(ps->p + ps->n, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  ps->n += (size_t)n;
}

static const char *ftype(ctx *c, gty t) { return cgty(c, ty(B_FLOAT, t.r, 1)); }

static void emit_stage(ctx *c, stage *st, sb *o) {
  sb *ps = &c->scratch;
  sb_clear(ps);
  unsigned u = st->used;
  sb_puts(c, o, "// generated by glsl2cg\n");
  if (st->helpers.n) sb_puts(c, o, st->helpers.p);
  if (st->vs) {
    sb_puts(c, o, "static float4 _g2c_Position;\n");
    if (u & U_PSIZE) sb_puts(c, o, "static float _g2c_PointSize;\n");
    if (u & U_IID) sb_puts(c, o, "static int _g2c_InstanceID;\n");
    if (u & U_VID) sb_puts(c, o, "static int _g2c_VertexID;\n");
  } else {
    if (u & U_FCOORD) sb_puts(c, o, "static float4 _g2c_FragCoord;\n");
    if (u & U_FACE) sb_puts(c, o, "static bool _g2c_FrontFacing;\n");
    if (u & U_PCOORD) sb_puts(c, o, "static float2 _g2c_PointCoord;\n");
    if (u & U_FCOLOR) sb_puts(c, o, "static float4 _g2c_FragColor;\n");
    if (u & U_FDATA) sb_puts(c, o, "static float4 _g2c_FragData[1];\n");
    if (u & U_FDEPTH) sb_puts(c, o, "static float _g2c_FragDepth;\n");
  }
  if (st->body.n) sb_puts(c, o, st->body.p);

  if (st->vs) {
    // attribs come in as float params named exactly like the GLSL ones
    for (int i = 0; i < st->nin; i++) add_param(c, ps, "%s %s", ftype(c, st->in[i].t), st->in[i].name);
    if (u & U_IID) add_param(c, ps, "float _g2c_iid");   // fed by the port's instance ramp stream
    if (u & U_VID) add_param(c, ps, "int _g2c_vid : INDEX");
    add_param(c, ps, "out float4 _g2c_opos : POSITION");
    if (u & U_PSIZE) add_param(c, ps, "out float _g2c_opsize : PSIZE");
    for (int i = 0; i < st->nout; i++)
      if (st->out[i].tc >= 0)
        add_param(c, ps, "out %s _g2c_tc%d : TEXCOORD%d", ftype(c, st->out[i].t), st->out[i].tc, st->out[i].tc);
    sb_printf(c, o, "void main(\n%s)\n{\n", ps->p ? ps->p : "");
    for (int i = 0; i < st->nin; i++) {
      iovar *v = &st->in[i];
      if (v->t.b == B_FLOAT) sb_printf(c, o, "\t%s = %s;\n", v->cg, v->name);
      else sb_printf(c, o, "\t%s = (%s)(%s);\n", v->cg, cgty(c, v->t), v->name);
    }
    if (u & U_IID) sb_puts(c, o, "\t_g2c_InstanceID = (int)_g2c_iid;\n");
    if (u & U_VID) sb_puts(c, o, "\t_g2c_VertexID = _g2c_vid;\n");
    sb_puts(c, o, "\t_g2c_Position = (float4)0;\n");
    if (u & U_PSIZE) sb_puts(c, o, "\t_g2c_PointSize = 1.0;\n");
    for (int i = 0; i < st->nout; i++) sb_printf(c, o, "\t%s = (%s)0;\n", st->out[i].cg, cgty(c, st->out[i].t));
    sb_puts(c, o, "\t_g2c_main();\n\t_g2c_opos = _g2c_Position;\n");
    if (u & U_PSIZE) sb_puts(c, o, "\t_g2c_opsize = _g2c_PointSize;\n");
    for (int i = 0; i < st->nout; i++) {
      iovar *v = &st->out[i];
      if (v->tc < 0) continue;
      if (v->t.b == B_FLOAT) sb_printf(c, o, "\t_g2c_tc%d = %s;\n", v->tc, v->cg);
      else sb_printf(c, o, "\t_g2c_tc%d = (%s)(%s);\n", v->tc, ftype(c, v->t), v->cg);
    }
    sb_puts(c, o, "}\n");
    return;
  }

  int nouts = (st->nout ? 1 : 0) + ((u & U_FCOLOR) ? 1 : 0) + ((u & U_FDATA) ? 1 : 0);
  if (nouts > 1) fail(c, "fragment shader writes more than one kind of color output");
  for (int i = 0; i < st->nin; i++)
    if (st->in[i].tc >= 0)
      add_param(c, ps, "%s _g2c_tc%d : TEXCOORD%d", ftype(c, st->in[i].t), st->in[i].tc, st->in[i].tc);
  if (u & U_FCOORD) add_param(c, ps, "float4 _g2c_wpos : WPOS");
  if (u & U_FACE) add_param(c, ps, "float _g2c_face : FACE");
  if (u & U_PCOORD) add_param(c, ps, "float2 _g2c_spc : SPRITECOORD");
  if (u & U_FDEPTH) add_param(c, ps, "out float _g2c_odepth : DEPTH");
  sb_printf(c, o, "float4 main(\n%s) : COLOR\n{\n", ps->p ? ps->p : "");
  for (int i = 0; i < st->nin; i++) {
    iovar *v = &st->in[i];
    if (v->tc < 0) sb_printf(c, o, "\t%s = (%s)0;\n", v->cg, cgty(c, v->t));
    else if (v->t.b == B_FLOAT) sb_printf(c, o, "\t%s = _g2c_tc%d;\n", v->cg, v->tc);
    else sb_printf(c, o, "\t%s = (%s)round(_g2c_tc%d);\n", v->cg, cgty(c, v->t), v->tc);
  }
  if (u & U_FCOORD) sb_puts(c, o, "\t_g2c_FragCoord = _g2c_wpos;\n");
  if (u & U_FACE) sb_puts(c, o, "\t_g2c_FrontFacing = _g2c_face > 0.0;\n");
  if (u & U_PCOORD) sb_puts(c, o, "\t_g2c_PointCoord = _g2c_spc;\n");
  if (u & U_FCOLOR) sb_puts(c, o, "\t_g2c_FragColor = (float4)0;\n");
  if (u & U_FDATA) sb_puts(c, o, "\t_g2c_FragData[0] = (float4)0;\n");
  if (u & U_FDEPTH) sb_puts(c, o, "\t_g2c_FragDepth = 0.0;\n");
  for (int i = 0; i < st->nout; i++) sb_printf(c, o, "\t%s = (%s)0;\n", st->out[i].cg, cgty(c, st->out[i].t));
  sb_puts(c, o, "\t_g2c_main();\n");
  if (u & U_FDEPTH) sb_puts(c, o, "\t_g2c_odepth = _g2c_FragDepth;\n");
  const char *ret = "(float4)0";
  if (u & U_FCOLOR) ret = "_g2c_FragColor";
  else if (u & U_FDATA) ret = "_g2c_FragData[0]";
  else if (st->nout) {
    iovar *v = &st->out[0];
    ret = v->t.r == 4 ? v->cg
        : v->t.r == 3 ? TS("float4(%s, 1.0)", v->cg)
        : v->t.r == 2 ? TS("float4(%s, 0.0, 1.0)", v->cg)
        : TS("float4(%s, 0.0, 0.0, 1.0)", v->cg);
  }
  sb_printf(c, o, "\treturn %s;\n}\n", ret);
}

// ---- API ----

static void ctx_free(ctx *c) {
  for (int i = 0; i < 2; i++) {
    sb_free(&c->st[i].helpers);
    sb_free(&c->st[i].body);
    sb_free(&c->fin[i]);
  }
  sb_free(&c->scratch);
  arena_free(&c->perm);
  arena_free(&c->tmp);
  free(c);
}

int g2c_translate_ex(const char *vs_glsl, const char *fs_glsl, unsigned flags, g2c_result *out) {
  if (!out) return -1;
  memset(out, 0, sizeof *out);
  if (!vs_glsl && !fs_glsl) {
    strcpy(out->error, "no shader source given");
    return -1;
  }
  ctx *c = calloc(1, sizeof *c);
  if (!c) {
    strcpy(out->error, "out of memory");
    return -1;
  }
  c->res = out;
  c->flags = flags;
  if (setjmp(c->jb)) {
    char err[sizeof out->error];
    memcpy(err, c->err, sizeof err);
    err[sizeof err - 1] = 0;
    ctx_free(c);
    memset(out, 0, sizeof *out);
    memcpy(out->error, err, sizeof err);
    return -1;
  }
  c->st[0].vs = 1;
  if (vs_glsl) parse_stage(c, 0, vs_glsl);
  if (fs_glsl) parse_stage(c, 1, fs_glsl);
  c->S = NULL;
  stage *vs = vs_glsl ? &c->st[0] : NULL, *fs = fs_glsl ? &c->st[1] : NULL;
  link_varyings(c, vs, fs);
  if (vs) {
    c->S = vs;
    emit_stage(c, vs, &c->fin[0]);
    out->uses_point_size = !!(vs->used & U_PSIZE);
    out->uses_instance_id = !!(vs->used & U_IID);
    out->uses_vertex_id = !!(vs->used & U_VID);
  }
  if (fs) {
    c->S = fs;
    emit_stage(c, fs, &c->fin[1]);
    out->uses_frag_coord = !!(fs->used & U_FCOORD);
    out->uses_front_facing = !!(fs->used & U_FACE);
    out->uses_point_coord = !!(fs->used & U_PCOORD);
    out->uses_frag_depth = !!(fs->used & U_FDEPTH);
  }
  out->vs_cg = c->fin[0].p;
  out->fs_cg = c->fin[1].p;
  c->fin[0].p = c->fin[1].p = NULL;
  ctx_free(c);
  return 0;
}

int g2c_translate(const char *vs_glsl, const char *fs_glsl, g2c_result *out) {
  return g2c_translate_ex(vs_glsl, fs_glsl, 0, out);
}

// cache stamp for shader.c: sha1 of this file + glsl2cg.h, set by cmake.
// was __DATE__ " " __TIME__, which wiped the whole translation cache on
// every rebuild even with no translator change
#ifndef G2C_BUILD
#define G2C_BUILD "no-stamp"
#endif
const char g2c_build_id[] = G2C_BUILD;

void g2c_free(g2c_result *r) {
  if (!r) return;
  free(r->vs_cg);
  free(r->fs_cg);
  r->vs_cg = r->fs_cg = NULL;
}
