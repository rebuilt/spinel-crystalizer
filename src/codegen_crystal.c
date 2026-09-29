/* Crystal proof-of-concept backend for Spinel.
 *
 * A second code generator that reads the SAME in-memory analysis the C backend
 * uses (the Compiler produced by analyze_program over the NodeTable) and emits
 * Crystal source instead of C. It exists to answer one question: does the
 * whole-program type inference carry enough information for a non-C target, and
 * do Ruby's block/control-flow semantics in Spinel's supported subset survive
 * translation to Crystal's block model?
 *
 * It deliberately covers only a small, concrete subset (literals, locals,
 * arithmetic/comparison, if/while, def, classes with ivars + accessors,
 * Array/Hash/String, and each/map/select/sum/times blocks). Anything outside
 * the subset is counted as an "unsupported" metric rather than translated, so
 * the corpus run measures how much of a real program falls in the gap.
 *
 * The C backend's emitter (codegen*.c) is intentionally NOT reused: its helpers
 * are fused to the sp_* runtime ABI (sp_RbVal boxing, SP_GC_ROOT, GNU statement
 * expressions, cls_id switches). Reusing them would prove nothing about the
 * analysis contract. This file walks the NodeTable directly and maps TyKind onto
 * Crystal's own type system.
 */
#include "analyze.h"   /* -> compiler.h -> node_table.h, types.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- output buffer (defined in codegen_util.c, shared with the C backend) ---- */
typedef struct { char *p; size_t len, cap; } Buf;
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
const int *call_args(const NodeTable *nt, int id, int *argc);

/* ---- emitter state ---- */
typedef struct { char **names; int n, cap; } NameSet;

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  NameSet locals;     /* declared local names in the current scope (stacked) */
  int poly_sites;     /* metric: expression nodes inferred as TY_POLY/TY_UNKNOWN */
  int unsupported;    /* metric: constructs outside the PoC subset */
  int nclasses;       /* classes emitted */
  int nmethods;       /* methods emitted */
} CrGen;

static void cr_indent(Buf *b, int n) { for (int i = 0; i < n; i++) buf_puts(b, "  "); }

/* ---- local-name set (per-scope declaration tracking) ---- */
static int nameset_has(NameSet *s, const char *name) {
  if (!name) return 0;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->names[i], name)) return 1;
  return 0;
}
static void nameset_add(NameSet *s, const char *name) {
  if (!name || nameset_has(s, name)) return;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->names = realloc(s->names, sizeof(char *) * s->cap);
  }
  s->names[s->n++] = (char *)name;
}
/* Save the current depth so a nested scope (method/block) can be popped back. */
static int cr_push_scope(CrGen *g) { return g->locals.n; }
static void cr_pop_scope(CrGen *g, int saved) { g->locals.n = saved; }

/* ---- forward declarations ---- */
static void cr_emit_expr(CrGen *g, int id, Buf *b);
static void cr_emit_stmts(CrGen *g, int body_id, Buf *b, int indent);
static void cr_emit_stmt(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_call(CrGen *g, int id, Buf *b);
static void cr_emit_block(Buf *b, CrGen *g, int block, int indent);
static void cr_emit_if(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b);
static void cr_emit_method(CrGen *g, Scope *s, Buf *b);

/* ---- type mapping: TyKind -> Crystal type name ----
   Writes the Crystal type to b. Returns 1 when the result is a CONCRETE type
   (usable as an annotation), 0 when it fell back to `untyped`. */
static int cr_map_type(CrGen *g, TyKind t, Buf *b) {
  switch (t) {
    case TY_INT:        buf_puts(b, "Int64"); return 1;
    case TY_BIGINT:     buf_puts(b, "BigInt"); return 1;
    case TY_FLOAT:      buf_puts(b, "Float64"); return 1;
    case TY_STRING:
    case TY_STRBUF:     buf_puts(b, "String"); return 1;
    case TY_SYMBOL:     buf_puts(b, "Symbol"); return 1;
    case TY_BOOL:       buf_puts(b, "Bool"); return 1;
    case TY_INT_ARRAY:      buf_puts(b, "Array(Int64)"); return 1;
    case TY_FLOAT_ARRAY:    buf_puts(b, "Array(Float64)"); return 1;
    case TY_STR_ARRAY:      buf_puts(b, "Array(String)"); return 1;
    case TY_POLY_ARRAY:     buf_puts(b, "Array(untyped)"); return 0;
    case TY_STR_INT_HASH:   buf_puts(b, "Hash(String, Int64)"); return 1;
    case TY_STR_STR_HASH:   buf_puts(b, "Hash(String, String)"); return 1;
    case TY_INT_INT_HASH:   buf_puts(b, "Hash(Int64, Int64)"); return 1;
    case TY_INT_STR_HASH:   buf_puts(b, "Hash(Int64, String)"); return 1;
    case TY_POLY_POLY_HASH: buf_puts(b, "Hash(untyped, untyped)"); return 0;
    default: break;
  }
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_puts(b, g->c->classes[cid].name); return 1; }
  }
  if (ty_is_obj_array(t)) {
    int cid = ty_obj_array_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_printf(b, "Array(%s)", g->c->classes[cid].name); return 1; }
  }
  buf_puts(b, "untyped");
  return 0;
}

/* The ELEMENT Crystal type of an array kind (for empty `[] of T` literals). */
static void cr_array_elem_type(CrGen *g, TyKind t, Buf *b) {
  switch (t) {
    /* scalar element types (the common `a << 1` / `a << "s"` case) */
    case TY_INT:        buf_puts(b, "Int64"); return;
    case TY_BIGINT:     buf_puts(b, "BigInt"); return;
    case TY_FLOAT:      buf_puts(b, "Float64"); return;
    case TY_STRING:
    case TY_STRBUF:     buf_puts(b, "String"); return;
    case TY_SYMBOL:     buf_puts(b, "Symbol"); return;
    case TY_BOOL:       buf_puts(b, "Bool"); return;
    /* array element types */
    case TY_INT_ARRAY:      buf_puts(b, "Int64"); return;
    case TY_FLOAT_ARRAY:    buf_puts(b, "Float64"); return;
    case TY_STR_ARRAY:      buf_puts(b, "String"); return;
    case TY_POLY_ARRAY:     buf_puts(b, "untyped"); return;
    case TY_INT_ARRAY_ARRAY:  buf_puts(b, "Array(Int64)"); return;
    case TY_FLOAT_ARRAY_ARRAY: buf_puts(b, "Array(Float64)"); return;
    default: break;
  }
  if (ty_is_obj_array(t)) {
    int cid = ty_obj_array_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_puts(b, g->c->classes[cid].name); return; }
  }
  /* a single object pushed into the array (the common `a << Point.new` case) */
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_puts(b, g->c->classes[cid].name); return; }
  }
  buf_puts(b, "untyped");
}

/* For an empty array literal whose element type the inference left poly (the
   common `a = []` then `a << x` inside a block), find the first push to that
   variable and use the pushed value's type as the element type. Scans the whole
   table -- fine for the PoC corpus, where names are unambiguous. */
static TyKind cr_find_pushed_elem(CrGen *g, const char *varname) {
  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(g->nt, id, "name");
    if (!nm || !(sp_streq(nm, "<<") || sp_streq(nm, "push"))) continue;
    int recv = nt_ref(g->nt, id, "receiver");
    if (recv < 0 || nt_kind(g->nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(g->nt, recv, "name");
    if (!rn || !sp_streq(rn, varname)) continue;
    int argc = 0; const int *argv = call_args(g->nt, id, &argc);
    if (argc >= 1) return comp_ntype(g->c, argv[0]);
  }
  return TY_UNKNOWN;
}

/* Emit ` : Type` when t is concrete, nothing otherwise. */
static void cr_emit_type_ann(CrGen *g, TyKind t, Buf *b) {
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  if (cr_map_type(g, t, &tmp)) { buf_printf(b, " : %s", tmp.p); }
  free(tmp.p);
}

/* ---- string escaping (Crystal double-quoted literal body) ---- */
static void cr_emit_escaped(Buf *b, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '\\': buf_puts(b, "\\\\"); break;
      case '"':  buf_puts(b, "\\\""); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\t': buf_puts(b, "\\t"); break;
      case '\r': buf_puts(b, "\\r"); break;
      default:
        if (*p < 0x20) buf_printf(b, "\\x%02x", (int)*p);
        else buf_putn(b, (const char *)p, 1);
    }
  }
}

/* A StatementsNode holding exactly one child -> that child's id, else -1. */
static int cr_single_expr(const NodeTable *nt, int stmts_id) {
  if (stmts_id < 0) return -1;
  const char *ty = nt_type(nt, stmts_id);
  if (!ty || !sp_streq(ty, "StatementsNode")) return stmts_id;
  int n = 0; const int *arr = nt_arr(nt, stmts_id, "body", &n);
  if (n == 1) return arr[0];
  return -1;
}

/* The scope whose def_node is `def_id`, or NULL. */
static Scope *cr_scope_for_def(Compiler *c, int def_id) {
  for (int s = 1; s < c->nscopes; s++)
    if (c->scopes[s].def_node == def_id) return &c->scopes[s];
  return NULL;
}

static int cr_is_binop(const char *n) {
  return sp_streq(n, "+") || sp_streq(n, "-") || sp_streq(n, "*") || sp_streq(n, "/") ||
         sp_streq(n, "%") || sp_streq(n, "**") || sp_streq(n, "==") || sp_streq(n, "!=") ||
         sp_streq(n, "<") || sp_streq(n, ">") || sp_streq(n, "<=") || sp_streq(n, ">=") ||
         sp_streq(n, "<=>");
}

/* Ruby -> Crystal stdlib name divergences for the unambiguous ones in the PoC
   subset. A full backend would key this on the receiver's type; here the names
   are unique enough within the supported surface that a flat map is safe. */
static const char *cr_translate_name(const char *n) {
  if (sp_streq(n, "length"))   return "size";        /* Array/Hash/String#length -> size */
  if (sp_streq(n, "include?")) return "includes?";   /* Enumerable/String#include? -> includes? */
  return n;
}

/* ---- expressions ---- */
static void cr_emit_expr(CrGen *g, int id, Buf *b) {
  if (id < 0) { buf_puts(b, "nil"); return; }
  NodeKind k = nt_kind(g->nt, id);
  TyKind t = comp_ntype(g->c, id);
  if (t == TY_POLY || t == TY_UNKNOWN) g->poly_sites++;

  switch (k) {
    case NK_IntegerNode: {
      /* TY_INT maps to Int64; Crystal literals default to Int32, so pin the
         width explicitly to keep arithmetic and container element types in
         agreement with the annotated Int64 slots. */
      buf_printf(b, "%lld_i64", nt_int(g->nt, id, "value", 0));
      return;
    }
    case NK_FloatNode: {
      const char *c = nt_content(g->nt, id);
      if (c && c[0]) buf_puts(b, c); else buf_puts(b, "0.0");
      return;
    }
    case NK_StringNode: {
      const char *s = nt_str(g->nt, id, "content");
      buf_puts(b, "\""); cr_emit_escaped(b, s ? s : ""); buf_puts(b, "\"");
      return;
    }
    case NK_SymbolNode: {
      const char *s = nt_str(g->nt, id, "content");
      buf_printf(b, ":%s", s ? s : "");
      return;
    }
    case NK_TrueNode:  buf_puts(b, "true");  return;
    case NK_FalseNode: buf_puts(b, "false"); return;
    case NK_NilNode:   buf_puts(b, "nil");   return;
    case NK_LocalVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");
      buf_puts(b, n ? n : "");
      return;
    }
    case NK_InstanceVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");   /* includes the leading @ */
      buf_puts(b, n ? n : "");
      return;
    }
    case NK_ConstantReadNode: {
      const char *n = nt_str(g->nt, id, "name");
      buf_puts(b, n ? n : "");
      return;
    }
    case NK_ArrayNode: {
      int n = 0; const int *els = nt_arr(g->nt, id, "elements", &n);
      if (n == 0) {
        /* Crystal cannot infer an empty literal's element type. */
        buf_puts(b, "[] of ");
        cr_array_elem_type(g, comp_ntype(g->c, id), b);
        return;
      }
      buf_puts(b, "[");
      for (int i = 0; i < n; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, els[i], b); }
      buf_puts(b, "]");
      return;
    }
    case NK_HashNode: {
      int n = 0; const int *els = nt_arr(g->nt, id, "elements", &n);
      buf_puts(b, "{ ");
      for (int i = 0; i < n; i++) {
        int assoc = els[i];
        int key = nt_ref(g->nt, assoc, "key");
        int val = nt_ref(g->nt, assoc, "value");
        if (i) buf_puts(b, ", ");
        cr_emit_expr(g, key, b);
        buf_puts(b, " => ");
        cr_emit_expr(g, val, b);
      }
      buf_puts(b, " }");
      return;
    }
    case NK_InterpolatedStringNode: {
      int n = 0; const int *parts = nt_arr(g->nt, id, "parts", &n);
      buf_puts(b, "\"");
      for (int i = 0; i < n; i++) {
        NodeKind pk = nt_kind(g->nt, parts[i]);
        if (pk == NK_StringNode) {
          const char *s = nt_str(g->nt, parts[i], "content");
          cr_emit_escaped(b, s ? s : "");
        } else if (pk == NK_EmbeddedStatementsNode) {
          int stmts = nt_ref(g->nt, parts[i], "statements");
          buf_puts(b, "#{");
          int e = cr_single_expr(g->nt, stmts);
          if (e >= 0) cr_emit_expr(g, e, b); else cr_emit_stmts(g, stmts, b, 0);
          buf_puts(b, "}");
        }
      }
      buf_puts(b, "\"");
      return;
    }
    case NK_AndNode: {
      int l = nt_ref(g->nt, id, "left"), r = nt_ref(g->nt, id, "right");
      buf_puts(b, "("); cr_emit_expr(g, l, b); buf_puts(b, " && "); cr_emit_expr(g, r, b); buf_puts(b, ")");
      return;
    }
    case NK_OrNode: {
      int l = nt_ref(g->nt, id, "left"), r = nt_ref(g->nt, id, "right");
      buf_puts(b, "("); cr_emit_expr(g, l, b); buf_puts(b, " || "); cr_emit_expr(g, r, b); buf_puts(b, ")");
      return;
    }
    case NK_ParenthesesNode: {
      /* Prism wraps the parenthesised expression in a StatementsNode under "body". */
      int body = nt_ref(g->nt, id, "body");
      int e = cr_single_expr(g->nt, body);
      buf_puts(b, "(");
      if (e >= 0) cr_emit_expr(g, e, b); else cr_emit_stmts(g, body, b, 0);
      buf_puts(b, ")");
      return;
    }
    case NK_RangeNode: {
      int l = nt_ref(g->nt, id, "left"), r = nt_ref(g->nt, id, "right");
      long long flags = nt_int(g->nt, id, "flags", 0);
      /* parenthesised: `..` binds looser than a method call, so `a..b.each()`
         would otherwise parse as `a .. (b.each())`. */
      buf_puts(b, "(");
      cr_emit_expr(g, l, b);
      buf_puts(b, (flags & 4) ? "..." : "..");   /* bit 2 = exclusive end */
      cr_emit_expr(g, r, b);
      buf_puts(b, ")");
      return;
    }
    case NK_IfNode:
      /* Crystal's if is a valid expression, so the same emitter serves both
         statement and value position. */
      cr_emit_if(g, id, b, 0);
      return;
    case NK_CallNode:
      cr_emit_call(g, id, b);
      return;
    default:
      g->unsupported++;
      buf_puts(b, "nil");
      return;
  }
}

/* ---- if / elsif / else (statement and expression) ---- */
static void cr_emit_if(CrGen *g, int id, Buf *b, int indent) {
  int pred = nt_ref(g->nt, id, "predicate");
  int then = nt_ref(g->nt, id, "statements");
  int sub  = nt_ref(g->nt, id, "subsequent");

  cr_indent(b, indent);
  buf_puts(b, "if ");
  cr_emit_expr(g, pred, b);
  buf_puts(b, "\n");
  cr_emit_stmts(g, then, b, indent + 1);

  int cur = sub;
  while (cur >= 0) {
    NodeKind ck = nt_kind(g->nt, cur);
    if (ck == NK_IfNode) {           /* elsif */
      int cp = nt_ref(g->nt, cur, "predicate");
      int ct = nt_ref(g->nt, cur, "statements");
      cr_indent(b, indent);
      buf_puts(b, "elsif ");
      cr_emit_expr(g, cp, b);
      buf_puts(b, "\n");
      cr_emit_stmts(g, ct, b, indent + 1);
      cur = nt_ref(g->nt, cur, "subsequent");
    } else if (ck == NK_ElseNode) {
      int cs = nt_ref(g->nt, cur, "statements");
      cr_indent(b, indent);
      buf_puts(b, "else\n");
      cr_emit_stmts(g, cs, b, indent + 1);
      cur = -1;
    } else break;
  }
  cr_indent(b, indent);
  buf_puts(b, "end\n");
}

/* ---- blocks: recv.<name> do |p| ... end ---- */
static void cr_emit_block(Buf *b, CrGen *g, int block, int indent) {
  int params = nt_ref(g->nt, block, "parameters");
  int body   = nt_ref(g->nt, block, "body");

  int saved = cr_push_scope(g);
  const char *pty = params >= 0 ? nt_type(g->nt, params) : NULL;
  int pn = 0; const int *preqs = NULL;
  if (pty && sp_streq(pty, "BlockParametersNode")) {
    int inner = nt_ref(g->nt, params, "parameters");
    if (inner >= 0) preqs = nt_arr(g->nt, inner, "requireds", &pn);
  } else if (pty && sp_streq(pty, "ParametersNode")) {
    preqs = nt_arr(g->nt, params, "requireds", &pn);
  }
  buf_puts(b, pn > 0 ? " do |" : " do");
  for (int i = 0; i < pn; i++) {
    const char *rn = nt_str(g->nt, preqs[i], "name");
    if (i) buf_puts(b, ", ");
    buf_puts(b, rn ? rn : "");
    nameset_add(&g->locals, rn);
  }
  if (pn > 0) buf_puts(b, "|");
  buf_puts(b, "\n");
  cr_emit_stmts(g, body, b, indent + 1);
  cr_indent(b, indent);
  buf_puts(b, "end\n");
  cr_pop_scope(g, saved);
}

/* ---- calls (operators, method calls, blocks) ---- */
static void cr_emit_call(CrGen *g, int id, Buf *b) {
  const char *name = nt_str(g->nt, id, "name");
  int recv  = nt_ref(g->nt, id, "receiver");
  int argc  = 0; const int *argv = call_args(g->nt, id, &argc);
  int block = nt_ref(g->nt, id, "block");
  if (!name) { g->unsupported++; buf_puts(b, "nil"); return; }

  /* unary - / + : no receiver, one argument */
  if (recv < 0 && argc == 1 && (sp_streq(name, "-") || sp_streq(name, "+"))) {
    buf_printf(b, "%c(", name[0]); cr_emit_expr(g, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* unary ! : receiver present, no arguments */
  if (recv >= 0 && argc == 0 && sp_streq(name, "!")) {
    buf_puts(b, "!("); cr_emit_expr(g, recv, b); buf_puts(b, ")");
    return;
  }
  /* binary operator */
  if (recv >= 0 && argc == 1 && cr_is_binop(name)) {
    const char *op = name;
    /* Ruby's `/` on two integers is truncating integer division; Crystal's `/`
       always yields a Float, so the integer case must use `//`. */
    if (sp_streq(name, "/")) {
      TyKind rt = comp_ntype(g->c, recv);
      TyKind at = comp_ntype(g->c, argv[0]);
      int ri = (rt == TY_INT || rt == TY_BIGINT);
      int ai = (at == TY_INT || at == TY_BIGINT);
      if (ri && ai) op = "//";
    }
    cr_emit_expr(g, recv, b);
    buf_printf(b, " %s ", op);
    cr_emit_expr(g, argv[0], b);
    return;
  }

  /* index read: Crystal uses bracket syntax, not a .[]() call */
  if (recv >= 0 && sp_streq(name, "[]")) {
    cr_emit_expr(g, recv, b);
    buf_puts(b, "[");
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
    buf_puts(b, "]");
    return;
  }

  /* method call */
  const char *mname = cr_translate_name(name);
  if (recv >= 0) { cr_emit_expr(g, recv, b); buf_printf(b, ".%s", mname); }
  else buf_puts(b, mname);
  buf_puts(b, "(");
  for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
  buf_puts(b, ")");

  if (block >= 0) cr_emit_block(b, g, block, 0);
}

/* ---- statements ---- */
static void cr_emit_stmts(CrGen *g, int body_id, Buf *b, int indent) {
  if (body_id < 0) return;
  const char *ty = nt_type(g->nt, body_id);
  if (!ty || !sp_streq(ty, "StatementsNode")) { cr_emit_stmt(g, body_id, b, indent); return; }
  int n = 0; const int *arr = nt_arr(g->nt, body_id, "body", &n);
  for (int i = 0; i < n; i++) cr_emit_stmt(g, arr[i], b, indent);
}

static void cr_emit_stmt(CrGen *g, int id, Buf *b, int indent) {
  NodeKind k = nt_kind(g->nt, id);
  switch (k) {
    case NK_LocalVariableWriteNode: {
      const char *name = nt_str(g->nt, id, "name");
      int val = nt_ref(g->nt, id, "value");
      /* Empty array literal whose element type the inference left poly: pin it
         from the first push so Crystal can form `[] of T`. */
      if (!nameset_has(&g->locals, name) && name &&
          nt_kind(g->nt, val) == NK_ArrayNode) {
        int en = 0; nt_arr(g->nt, val, "elements", &en);
        if (en == 0) {
          Buf probe; memset(&probe, 0, sizeof probe);
          int concrete = cr_map_type(g, comp_ntype(g->c, val), &probe);
          free(probe.p);
          if (!concrete) {
            TyKind et = cr_find_pushed_elem(g, name);
            if (et != TY_UNKNOWN && et != TY_POLY) {
              Buf at; memset(&at, 0, sizeof at);
              buf_puts(&at, "Array("); cr_array_elem_type(g, et, &at); buf_puts(&at, ")");
              cr_indent(b, indent);
              buf_printf(b, "%s : %s = [] of ", name, at.p);
              cr_array_elem_type(g, et, b);
              free(at.p);
              nameset_add(&g->locals, name);
              buf_puts(b, "\n");
              return;
            }
          }
        }
      }
      cr_indent(b, indent);
      if (!nameset_has(&g->locals, name)) {
        TyKind t = comp_ntype(g->c, val);
        buf_printf(b, "%s", name ? name : "");
        cr_emit_type_ann(g, t, b);
        buf_puts(b, " = ");
        nameset_add(&g->locals, name);
      } else {
        buf_printf(b, "%s = ", name ? name : "");
      }
      cr_emit_expr(g, val, b);
      buf_puts(b, "\n");
      return;
    }
    case NK_InstanceVariableWriteNode: {
      const char *name = nt_str(g->nt, id, "name");   /* includes the leading @ */
      int val = nt_ref(g->nt, id, "value");
      cr_indent(b, indent);
      buf_printf(b, "%s = ", name ? name : "");
      cr_emit_expr(g, val, b);
      buf_puts(b, "\n");
      return;
    }
    case NK_LocalVariableOperatorWriteNode: {
      const char *name = nt_str(g->nt, id, "name");
      const char *op   = nt_str(g->nt, id, "binary_operator");
      int val = nt_ref(g->nt, id, "value");
      cr_indent(b, indent);
      if (op && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") ||
                 sp_streq(op, "/") || sp_streq(op, "%"))) {
        buf_printf(b, "%s %s= ", name ? name : "", op);
        cr_emit_expr(g, val, b);
      } else {
        /* no compound form in Crystal (e.g. **=): expand to a plain assignment */
        buf_printf(b, "%s = %s", name ? name : "", name ? name : "");
        if (op) buf_printf(b, " %s ", op);
        cr_emit_expr(g, val, b);
      }
      buf_puts(b, "\n");
      return;
    }
    case NK_IfNode:
      cr_emit_if(g, id, b, indent);
      return;
    case NK_UnlessNode: {
      int pred = nt_ref(g->nt, id, "predicate");
      int then = nt_ref(g->nt, id, "statements");
      int cons = nt_ref(g->nt, id, "consequent");
      cr_indent(b, indent);
      buf_puts(b, "unless ");
      cr_emit_expr(g, pred, b);
      buf_puts(b, "\n");
      cr_emit_stmts(g, then, b, indent + 1);
      if (cons >= 0 && nt_kind(g->nt, cons) == NK_ElseNode) {
        int cs = nt_ref(g->nt, cons, "statements");
        cr_indent(b, indent); buf_puts(b, "else\n");
        cr_emit_stmts(g, cs, b, indent + 1);
      }
      cr_indent(b, indent); buf_puts(b, "end\n");
      return;
    }
    case NK_WhileNode: {
      int pred = nt_ref(g->nt, id, "predicate");
      int body = nt_ref(g->nt, id, "statements");
      cr_indent(b, indent); buf_puts(b, "while ");
      cr_emit_expr(g, pred, b); buf_puts(b, "\n");
      cr_emit_stmts(g, body, b, indent + 1);
      cr_indent(b, indent); buf_puts(b, "end\n");
      return;
    }
    case NK_UntilNode: {
      int pred = nt_ref(g->nt, id, "predicate");
      int body = nt_ref(g->nt, id, "statements");
      cr_indent(b, indent); buf_puts(b, "until ");
      cr_emit_expr(g, pred, b); buf_puts(b, "\n");
      cr_emit_stmts(g, body, b, indent + 1);
      cr_indent(b, indent); buf_puts(b, "end\n");
      return;
    }
    case NK_ReturnNode: {
      int args = nt_ref(g->nt, id, "arguments");
      cr_indent(b, indent); buf_puts(b, "return");
      if (args >= 0) { buf_puts(b, " "); cr_emit_expr(g, args, b); }
      buf_puts(b, "\n");
      return;
    }
    case NK_NextNode: {
      int args = nt_ref(g->nt, id, "arguments");
      cr_indent(b, indent); buf_puts(b, "next");
      if (args >= 0) { buf_puts(b, " "); cr_emit_expr(g, args, b); }
      buf_puts(b, "\n");
      return;
    }
    case NK_BreakNode: {
      int args = nt_ref(g->nt, id, "arguments");
      cr_indent(b, indent); buf_puts(b, "break");
      if (args >= 0) { buf_puts(b, " "); cr_emit_expr(g, args, b); }
      buf_puts(b, "\n");
      return;
    }
    case NK_BeginNode: {
      /* PoC: emit the plain statements; rescue/ensure are counted unsupported. */
      int stmts = nt_ref(g->nt, id, "statements");
      if (nt_ref(g->nt, id, "rescue_clause") >= 0 || nt_ref(g->nt, id, "ensure_clause") >= 0)
        g->unsupported++;
      cr_emit_stmts(g, stmts, b, indent);
      return;
    }
    case NK_StatementsNode:
      cr_emit_stmts(g, id, b, indent);
      return;
    case NK_PostExecutionNode:
    case NK_PreExecutionNode:
      return;   /* BEGIN/END blocks: out of scope */
    default: {
      /* A bare expression used as a statement (a call for its side effect, an
         assignment-shaped node we don't special-case, ...). Emit it as an
         expression; if it is not one, count it unsupported. */
      cr_indent(b, indent);
      cr_emit_expr(g, id, b);
      buf_puts(b, "\n");
      return;
    }
  }
}

/* ---- methods ---- */
static void cr_emit_method(CrGen *g, Scope *s, Buf *b) {
  /* The analyzer splices in compiler-internal wrapper methods under the
     reserved "__" prefix (__enumw_*, __int_/__flt_/__cmp_*, __enum_to_a,
     __bam_*, ...) to build the C backend's Enumerable/Integer surface. Their
     bodies lower through constructs this PoC does not model, so emitting them
     verbatim would inject broken Crystal. Crystal's stdlib already provides
     map/select/each/gcd natively, so skip every "__"-prefixed synthesized
     method and let the stdlib answer. (A program-authored `def __foo` would be
     dropped too; the PoC subset never uses one.) */
  if (s->name && s->name[0] == '_' && s->name[1] == '_') return;
  g->nmethods++;
  buf_printf(b, "def %s%s(", s->is_cmethod ? "self." : "", s->name);
  for (int i = 0; i < s->nparams; i++) {
    const char *pn = s->pnames[i];
    LocalVar *lv = scope_local(s, pn);
    TyKind pt = lv ? lv->type : TY_UNKNOWN;
    if (i) buf_puts(b, ", ");
    buf_printf(b, "%s", pn);
    cr_emit_type_ann(g, pt, b);
  }
  buf_puts(b, ")\n");

  int saved = cr_push_scope(g);
  for (int i = 0; i < s->nparams; i++) nameset_add(&g->locals, s->pnames[i]);
  cr_emit_stmts(g, s->body, b, 1);
  cr_pop_scope(g, saved);

  buf_puts(b, "end\n");
}

/* ---- classes ---- */
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b) {
  g->nclasses++;
  buf_printf(b, "class %s\n", ci->name);

  /* typed ivar declarations */
  for (int i = 0; i < ci->nivars; i++) {
    buf_printf(b, "  %s", ci->ivars[i]);
    cr_emit_type_ann(g, ci->ivar_types[i], b);
    buf_puts(b, "\n");
  }

  /* methods: walk the class body's DefNodes */
  int cbody = -1;
  if (ci->def_node >= 0) {
    const char *cty = nt_type(g->nt, ci->def_node);
    if (cty && sp_streq(cty, "ClassNode")) cbody = nt_ref(g->nt, ci->def_node, "body");
  }
  if (cbody >= 0) {
    int n = 0; const int *arr = nt_arr(g->nt, cbody, "body", &n);
    for (int i = 0; i < n; i++) {
      if (nt_kind(g->nt, arr[i]) != NK_DefNode) continue;
      Scope *s = cr_scope_for_def(g->c, arr[i]);
      if (s) cr_emit_method(g, s, b);
    }
  }

  /* attr readers / writers (resolved by the analyzer into ci->readers/writers) */
  for (int i = 0; i < ci->nreaders; i++) {
    buf_printf(b, "  def %s\n    @%s\n  end\n", ci->readers[i], ci->readers[i]);
  }
  for (int i = 0; i < ci->nwriters; i++) {
    buf_printf(b, "  def %s=(v)\n    @%s = v\n  end\n", ci->writers[i], ci->writers[i]);
  }

  buf_puts(b, "end\n");
}

/* ---- entry point ---- */
char *codegen_program_crystal(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  CrGen g; memset(&g, 0, sizeof g);
  g.c = c; g.nt = nt;

  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "# Generated by Spinel (Crystal proof-of-concept backend)\n");
  buf_puts(&b, "# This file is machine-generated; do not edit.\n\n");

  int top_body = c->scopes[0].body;
  const int *tstmts = NULL; int tn = 0;
  if (top_body >= 0 && nt_kind(nt, top_body) == NK_StatementsNode)
    tstmts = nt_arr(nt, top_body, "body", &tn);

  /* Pass 1: declarations (classes and top-level defs), so they are available
     to the executable statements that follow -- matching Ruby, where a def or
     class is visible throughout regardless of its position. */
  if (tstmts) {
    for (int k = 0; k < tn; k++) {
      NodeKind kk = nt_kind(nt, tstmts[k]);
      if (kk == NK_ClassNode) {
        const char *nm = NULL;
        int cp = nt_ref(nt, tstmts[k], "constant_path");
        if (cp >= 0) nm = nt_str(nt, cp, "name");
        int ci = nm ? comp_class_index(c, nm) : -1;
        if (ci >= 0) cr_emit_class(&g, &c->classes[ci], &b);
      } else if (kk == NK_DefNode) {
        Scope *s = cr_scope_for_def(c, tstmts[k]);
        if (s) cr_emit_method(&g, s, &b);
      }
    }
  }

  /* Pass 2: executable top-level statements, in source order. */
  int saved = cr_push_scope(&g);
  if (tstmts) {
    for (int k = 0; k < tn; k++) {
      NodeKind kk = nt_kind(nt, tstmts[k]);
      if (kk == NK_ClassNode || kk == NK_DefNode) continue;
      cr_emit_stmt(&g, tstmts[k], &b, 0);
    }
  }
  cr_pop_scope(&g, saved);

  fprintf(stderr, "[spinel-crystal] classes=%d methods=%d poly_sites=%d unsupported=%d\n",
          g.nclasses, g.nmethods, g.poly_sites, g.unsupported);

  free(g.locals.names);
  comp_free(c);
  return b.p;
}
