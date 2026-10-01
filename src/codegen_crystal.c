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
/* Rejection diagnostic shared with the C backend (codegen_util.c): names the
   feature and stops, anchored to the Ruby source line of `id`. This is what
   turns a construct outside the subset into a hard error instead of a silent
   `nil` placeholder that would ship as wrong output. */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg);
/* True when `val` is a `Struct.new(...)` / `Data.define(...)` call (analyze_scope.c). */
int is_struct_call(Compiler *c, int val);

/* ---- emitter state ---- */
typedef struct { char **names; int n, cap; } NameSet;

typedef struct { char **names; int *reads; int *safe; int n, cap; } NilUseTally;

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  NameSet locals;     /* declared local names in the current scope (stacked) */
  int poly_sites;     /* metric: expression nodes inferred as TY_POLY/TY_UNKNOWN */
  int unsupported;    /* metric: constructs outside the PoC subset */
  int first_unsup;    /* node id of the first unsupported construct (-1 if none);
                         anchors the end-of-run refusal to a Ruby source line */
  char first_why[160];/* what that first construct was, in words for the diagnostic */
  int cur_class;      /* class_id of the method being emitted (-1 at top level);
                         lets a receiverless call resolve against the enclosing
                         class's own methods, like Ruby's implicit self */  int nclasses;       /* classes emitted */
  int nmethods;       /* methods emitted */
  int tail_value;     /* node id of the last statement of the method/block body
                         currently being emitted, or -1. That statement's value
                         is the method's or block's return value, so it is NOT a
                         discarded read -- and turning a trailing read into `[]?`
                         would silently widen the return type to a union that
                         every caller then has to absorb. */
  int nil_ok;         /* the expression being emitted sits in a position that
                         tolerates nil (a `.nil?` receiver, an `||` left arm, an
                         inspect/to_s receiver), so a Hash/Array read there must
                         answer nil like Ruby's rather than raise. Set by the
                         parent around the child's emission, not a property of the
                         node itself. */
  NilUseTally nil_uses; /* per method: for each local, how many of its reads sit
                           in a nil-tolerant position and how many reads it has in
                           total. A binding from a maybe-missing read is declared
                           `T?` only when every read tolerates nil. */
  const char *rescue_var; /* name bound by the rescue clause currently being
                             emitted, or NULL outside one. Ruby's bare `raise`
                             re-raises `$!`; Crystal's is `raise e`, so a re-raise
                             needs the binding the enclosing clause introduced.
                             Set around the emission of a clause body, which may
                             itself contain nested begin/rescue. */
  int tmp_seq;          /* counter for synthetic emitter-local names, so two
                           desugarings in one method never collide */
} CrGen;

/* Record that `id` is outside the supported subset. Emission continues (a
   placeholder is emitted by the caller) so the whole program is scanned and the
   count is complete; codegen_program_crystal refuses at the end if any were seen,
   naming the first one. */
static void cr_mark_unsupported(CrGen *g, int id, const char *why) {
  g->unsupported++;
  if (g->first_unsup < 0) {
    g->first_unsup = id;
    snprintf(g->first_why, sizeof g->first_why, "%s", why);
  }
}

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
static void cr_emit_block(Buf *b, CrGen *g, int block, int indent, int recv,
                          const char *meth);
static void cr_emit_if(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_case(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b, int is_module);
static void cr_emit_method(CrGen *g, Scope *s, Buf *b);
static int cr_single_expr(const NodeTable *nt, int stmts_id);
static void cr_scan_nil_uses(CrGen *g, int id, NilUseTally *out, int safe);

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

/* True when a Ruby local's name is reserved in Crystal and so cannot be emitted
   verbatim. Ruby and Crystal share most keywords, so a local named `class` or
   `if` is already broken in Ruby; the ones that matter are the Crystal-only
   keywords `out`, `in`, `as` and `with`, which are ordinary Ruby identifiers. A
   local named `out` used to emit `out = ""` and fail to compile with "expecting
   variable or instance variable after out", because Crystal's parser reads `out`
   as the start of an out-parameter. The Ruby-keyword list is included too:
   renaming those is harmless (they cannot be legal Ruby locals) and keeps one
   rule instead of an incomplete keyword list. */
static int cr_is_reserved(const char *n) {
  if (!n) return 0;
  static const char *const kw[] = {
    "alias", "abstract", "annotation", "as", "begin", "break", "case", "class",
    "def", "do", "else", "elsif", "end", "ensure", "false", "if", "in",
    "module", "next", "nil", "out", "rescue", "retry", "return", "self", "super",
    "then", "true", "unless", "until", "when", "while", "with", "yield",
  };
  for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
    if (sp_streq(n, kw[i])) return 1;
  return 0;
}

/* The emitted name for a Ruby local: `_cr_<name>` when reserved, else the name
   itself. The rename is a pure function of the Ruby name, so every occurrence of
   that local -- its write, its reads, a block parameter, a multi-assign target,
   a method parameter -- maps to the same identifier and the binding still lines
   up. The analyzer's symbol table keeps the Ruby name (scope_local/nameset are
   keyed on it), so only the emitted text changes.

   The returned buffer is per-call, so the result must be used before the next
   cr_local call; callers either buf_puts it immediately or call it once per
   printf argument, which is safe because printf arguments are evaluated before
   the format is consumed. Four slots cover the widest single printf. */
static const char *cr_local(const char *n) {
  if (!n) return "";
  if (!cr_is_reserved(n)) return n;
  static char bufs[4][96];
  static int turn;
  char *out = bufs[turn++ & 3];
  snprintf(out, 96, "_cr_%s", n);
  return out;
}

/* The ELEMENT Crystal type of an array kind (for empty array literals).
   Returns 1 when the result is a CONCRETE Crystal type, 0 when the element type
   is poly/unknown -- which is an outright refusal, not something to paper over:
   Crystal 1.21 has no `untyped` type at all (`untyped`, and the `Any` alias that
   replaced it, are both gone), so there is no name to write for a dynamic
   element type. Emitting one only ever produced a parse error. */
static int cr_array_elem_type(CrGen *g, TyKind t, Buf *b) {
  switch (t) {
    /* scalar element types (the common `a << 1` / `a << "s"` case) */
    case TY_INT:        buf_puts(b, "Int64"); return 1;
    case TY_BIGINT:     buf_puts(b, "BigInt"); return 1;
    case TY_FLOAT:      buf_puts(b, "Float64"); return 1;
    case TY_STRING:
    case TY_STRBUF:     buf_puts(b, "String"); return 1;
    case TY_SYMBOL:     buf_puts(b, "Symbol"); return 1;
    case TY_BOOL:       buf_puts(b, "Bool"); return 1;
    /* array element types */
    case TY_INT_ARRAY:      buf_puts(b, "Int64"); return 1;
    case TY_FLOAT_ARRAY:    buf_puts(b, "Float64"); return 1;
    case TY_STR_ARRAY:      buf_puts(b, "String"); return 1;
    case TY_POLY_ARRAY:     buf_puts(b, "Object"); return 0;
    case TY_INT_ARRAY_ARRAY:  buf_puts(b, "Array(Int64)"); return 1;
    case TY_FLOAT_ARRAY_ARRAY: buf_puts(b, "Array(Float64)"); return 1;
    default: break;
  }
  if (ty_is_obj_array(t)) {
    int cid = ty_obj_array_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_puts(b, g->c->classes[cid].name); return 1; }
  }
  /* a single object pushed into the array (the common `a << Point.new` case) */
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < g->c->nclasses) { buf_puts(b, g->c->classes[cid].name); return 1; }
  }
  buf_puts(b, "Object");
  return 0;
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

/* Same idea for a bare `Hash.new` bound to a new local: Crystal cannot infer K
   and V from an argument-less constructor, so take them from the first `h[k] = v`
   store to that variable. Returns 0 when no store pins both. */
static int cr_find_stored_kv(CrGen *g, const char *varname, TyKind *k, TyKind *v) {
  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(g->nt, id, "name");
    if (!nm || !sp_streq(nm, "[]=")) continue;
    int recv = nt_ref(g->nt, id, "receiver");
    if (recv < 0 || nt_kind(g->nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(g->nt, recv, "name");
    if (!rn || !sp_streq(rn, varname)) continue;
    int argc = 0; const int *argv = call_args(g->nt, id, &argc);
    if (argc < 2) continue;
    *k = comp_ntype(g->c, argv[0]);
    *v = comp_ntype(g->c, argv[1]);
    if (*k != TY_UNKNOWN && *v != TY_UNKNOWN) return 1;
  }
  return 0;
}

/* Emit ` : Type` when t is concrete, nothing otherwise. */
static void cr_emit_type_ann(CrGen *g, TyKind t, Buf *b) {
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  if (cr_map_type(g, t, &tmp)) { buf_printf(b, " : %s", tmp.p); }
  free(tmp.p);
}

/* True when `id` is a read that can answer nil in Ruby -- a `[]` index into a
   Hash or Array, or a parenthesised copy of one. These are the expressions whose
   value is `T?` on the Crystal side, so a local bound from one must be declared
   nilable for later `v.nil?` / `v || d` uses to typecheck. */
static int cr_maybe_missing(CrGen *g, int id) {
  int cur = id;
  for (int hop = 0; hop < 8; hop++) {   /* look through parentheses */
    if (cur < 0) return 0;
    NodeKind k = nt_kind(g->nt, cur);
    if (k == NK_ParenthesesNode) {
      int body = nt_ref(g->nt, cur, "body");
      int inner = cr_single_expr(g->nt, body);
      if (inner < 0) return 0;
      cur = inner;
      continue;
    }
    if (k != NK_CallNode) return 0;
    const char *nm = nt_str(g->nt, cur, "name");
    if (!nm || !sp_streq(nm, "[]")) return 0;
    int recv = nt_ref(g->nt, cur, "receiver");
    if (recv < 0) return 0;
    TyKind rt = comp_ntype(g->c, recv);
    return ty_is_hash(rt) || ty_is_array(rt);
  }
  return 0;
}

/* ---- locals asked about as nilable ----
   A local bound from a possibly-missing read should be declared `T?` only when
   something downstream actually asks whether it is nil -- `v.nil?`, `v == nil`,
   `v || d`, or handing `v` to p/puts. When nothing asks, the reads are the
   ordinary "this key is there" reads and the local stays `T`, which is what
   `x = c.last; p x + 1.0` needs. This is the same consumption-site discipline
   the index-read flag uses: nilability is claimed where it is consumed, not
   everywhere a missing key is theoretically possible. */
static void cr_tally_bump(NilUseTally *t, const char *name, int safe) {
  if (!name) return;
  for (int i = 0; i < t->n; i++) {
    if (sp_streq(t->names[i], name)) {
      t->reads[i]++;
      if (safe) t->safe[i]++;
      return;
    }
  }
  if (t->n == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 16;
    t->names = realloc(t->names, sizeof(char *) * t->cap);
    t->reads = realloc(t->reads, sizeof(int) * t->cap);
    t->safe  = realloc(t->safe,  sizeof(int) * t->cap);
  }
  t->names[t->n] = (char *)name; t->reads[t->n] = 1; t->safe[t->n] = safe; t->n++;
}
/* A local may be declared `T?` only when EVERY read of it tolerates nil. One
   plain read (`x + 1.0`) means the program relies on the value being there, and
   claiming `T?` would turn a working translation into a build error. */
static int cr_all_reads_nil_safe(NilUseTally *t, const char *name) {
  if (!name) return 0;
  for (int i = 0; i < t->n; i++)
    if (sp_streq(t->names[i], name)) return t->reads[i] > 0 && t->reads[i] == t->safe[i];
  return 0;
}
/* The node id of the last statement of a body, whose value is the body's result. */
static int cr_tail_stmt(CrGen *g, int body) {
  if (body < 0 || nt_kind(g->nt, body) != NK_StatementsNode) return -1;
  int n = 0; const int *a = nt_arr(g->nt, body, "body", &n);
  return n > 0 ? a[n - 1] : -1;
}
/* Peek through parentheses at `a`; returns the inner expression or -1. */
static int cr_peek(CrGen *g, int a) {
  for (int h = 0; h < 8 && a >= 0 && nt_kind(g->nt, a) == NK_ParenthesesNode; h++) {
    int inner = cr_single_expr(g->nt, nt_ref(g->nt, a, "body"));
    if (inner < 0) return -1;
    a = inner;
  }
  return a;
}
/* Handle one nil-tolerant slot. A bare local read in the slot is a safe read of
   that name; anything else is an ordinary expression whose own subexpressions
   are not in a nil-tolerant position, so recurse with safe=0. In either case
   the child must NOT be walked again by the caller's generic child loop, so the
   id is recorded in `handled` and the caller skips it -- otherwise the same read
   is counted once as safe and once as unsafe and every local looks unsafe. */
static void cr_nil_slot(CrGen *g, int a, NilUseTally *out, int *handled, int *nhandled) {
  int r = cr_peek(g, a);
  handled[(*nhandled)++] = a;
  if (r < 0) return;
  if (nt_kind(g->nt, r) == NK_LocalVariableReadNode)
    cr_tally_bump(out, nt_str(g->nt, r, "name"), 1);
  else
    cr_scan_nil_uses(g, r, out, 0);
}
static int cr_was_handled(const int *handled, int nhandled, int id) {
  for (int i = 0; i < nhandled; i++) if (handled[i] == id) return 1;
  return 0;
}
static void cr_scan_nil_uses(CrGen *g, int id, NilUseTally *out, int safe) {
  if (id < 0) return;
  NodeKind k = nt_kind(g->nt, id);
  if (k == NK_LocalVariableReadNode) { cr_tally_bump(out, nt_str(g->nt, id, "name"), safe); return; }

  int handled[8]; int nh = 0;   /* children this call already dealt with */
  if (k == NK_CallNode) {
    const char *nm = nt_str(g->nt, id, "name");
    int recv = nt_ref(g->nt, id, "receiver");
    int is_query = nm && (sp_streq(nm, "nil?") || sp_streq(nm, "inspect") ||
                          sp_streq(nm, "to_s") || sp_streq(nm, "to_a"));
    int is_out  = nm && (sp_streq(nm, "p") || sp_streq(nm, "puts") ||
                         sp_streq(nm, "print") || sp_streq(nm, "pp"));
    if (is_query && recv >= 0) cr_nil_slot(g, recv, out, handled, &nh);
    else if (is_out) {
      int argc = 0; const int *argv = call_args(g->nt, id, &argc);
      for (int i = 0; i < argc && nh < 8; i++) cr_nil_slot(g, argv[i], out, handled, &nh);
    }
  } else if (k == NK_OrNode) {
    cr_nil_slot(g, nt_ref(g->nt, id, "left"), out, handled, &nh);
  }

  for (int i = 0; i < nt_num_refs(g->nt, id); i++) {
    int ch = nt_ref_at(g->nt, id, i);
    if (ch < 0 || cr_was_handled(handled, nh, ch)) continue;
    cr_scan_nil_uses(g, ch, out, 0);
  }
  for (int i = 0; i < nt_num_arrs(g->nt, id); i++) {
    int n = 0; const int *a = nt_arr_at(g->nt, id, i, &n);
    for (int j = 0; j < n; j++) {
      if (cr_was_handled(handled, nh, a[j])) continue;
      cr_scan_nil_uses(g, a[j], out, 0);
    }
  }
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

/* Copy a regex pattern into a /.../ literal, escaping only the delimiter. The
   unescaped source resolves \/ and %r{} slashes to a bare /, which would close
   the Crystal literal early; re-escape it. Backslashes are left as-is -- they
   are regex syntax in both Ruby and Crystal, not string escapes. */
static void cr_emit_regex_pat(Buf *b, const char *s) {
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    if (*p == '/') buf_puts(b, "\\/");
    else buf_putn(b, (const char *)p, 1);
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
         sp_streq(n, "<=>") || sp_streq(n, ">>") || sp_streq(n, "&") ||
         sp_streq(n, "|") || sp_streq(n, "^");
}

/* Ruby -> Crystal stdlib name divergences for the unambiguous ones in the PoC
   subset. Keyed on the receiver's inferred type where the mapping depends on it:
   `include?` is `includes?` on Array/String but `has_key?` on Hash (Crystal's
   Hash has no `includes?`). A full backend would grow this into a complete
   receiver-type-aware table; for now only the type-sensitive names are keyed. */
/* Ruby's core class name -> Crystal's spelling of the same type. Only the names
   Crystal actually defines are listed; anything else (Module, Data, Comparable
   in a value position, ...) has no Crystal form and is left alone to fail
   loudly rather than be mapped to a guess. */
static const char *cr_core_class_name(CrGen *g, const char *n) {
  for (int i = 0; i < g->c->nclasses; i++)
    if (sp_streq(g->c->classes[i].name, n)) return NULL;   /* a user class wins */
  if (sp_streq(n, "Integer"))       return "Int";
  if (sp_streq(n, "NilClass"))      return "Nil";
  if (sp_streq(n, "TrueClass"))     return "Bool";
  if (sp_streq(n, "FalseClass"))    return "Bool";
  if (sp_streq(n, "BasicObject"))   return "Object";
  return NULL;
}

/* A Ruby constant that has an exact Crystal spelling with the same value, so it
   is translated rather than refused. Returns 1 and writes the Crystal
   expression when `path::leaf` has one. Only exact-equivalent spellings belong
   here; a value that merely resembles the Ruby one is a refusal, not a guess. */
static int cr_const_remap(const char *path, const char *leaf, Buf *b) {
  if (!leaf) return 0;
  /* Ruby's Float::INFINITY is a constant VALUE, and a positive-zero divisor is
     the Crystal expression with exactly that value (verified: it compiles and
     prints Infinity). Writing the constant name itself would not compile. A bare
     `Float` read has no owner, so `path` is NULL there and must not be compared. */
  if (path && sp_streq(path, "Float") && sp_streq(leaf, "INFINITY")) {
    buf_puts(b, "(1.0 / 0.0)");
    return 1;
  }
  return 0;
}

/* A Ruby name Crystal's stdlib does not define, so emitting one is a guaranteed
   "undefined constant" build error. Returns the refusal reason, or NULL when the
   name is fine. Keyed by full path where the bare name IS defined but the member
   is not: `Float` is a real Crystal class, `Float::INFINITY` is not one of its
   members, so a bare `Float` must keep working.

   Consulted only after cr_core_class_name has already given a user class of the
   same name priority, so a program's own `class Data` still works -- a class it
   defines is a different thing from the stdlib name it shadows. */
static const char *cr_missing_const(const char *path, const char *leaf) {
  if (!leaf) return NULL;
  static const struct { const char *path; const char *leaf; const char *why; } m[] = {
    /* No Crystal stdlib name of that kind at all. */
    { "TCPServer", NULL, "Crystal's sockets are a separate library with no TCPServer" },
    { "Socket", NULL, "Crystal's sockets are a separate library with no Socket" },
    { "Addrinfo", NULL, "Crystal has no Addrinfo; an address is an Addr" },
    { "Data", NULL, "Crystal 1.21 has no Data.define" },
    { "Enumerator", NULL, "Crystal has no Enumerator" },
    { "Queue", NULL, "Crystal's channel type is Channel, not Queue" },
    { "ConditionVariable", NULL, "Crystal has no ConditionVariable" },
    { "Regexp", NULL, "Crystal spells the class Regex; there is no Regexp" },
    { "RUBY_ENGINE", NULL, "Crystal has no RUBY_ENGINE constant" },
    { "FileTest", NULL, "Crystal has no FileTest module" },
    { "Interrupt", NULL, "Crystal has no Interrupt exception class" },
    { "NoMethodError", NULL, "Crystal has no NoMethodError exception class" },
    { "FrozenError", NULL, "Crystal has no FrozenError exception class" },
    { "Complex", NULL, "Crystal has no Complex number type" },
    /* The class exists; the member does not. */
    { "Float", "INFINITY", "Crystal has no Float::INFINITY" },
    { "Float", "NAN", "Crystal has no Float::NAN" },
  };
  for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
    if (m[i].leaf) {
      if (path && sp_streq(m[i].path, path) && sp_streq(m[i].leaf, leaf)) return m[i].why;
    } else if (sp_streq(m[i].path, leaf)) {
      return m[i].why;
    }
  }
  return NULL;
}

/* ---- exception classes ----
   Crystal 1.21 defines only a handful of exception classes (Exception,
   ArgumentError, IndexError, KeyError, RuntimeError, DivisionByZeroError,
   OverflowError, TypeCastError, NotImplementedError, NilAssertionError,
   InvalidByteSequenceError). Ruby has ~40, and the ones the corpus rescues most
   -- NoMethodError, TypeError, FrozenError -- have no counterpart at all.

   Mapping a missing class to a broad one would be wrong, not a compromise:
   `rescue TypeError` becoming `rescue Exception` catches everything, so a clause
   that should fall through would swallow the error and take the handler branch.
   That is a silent semantic change, so an unmapped class is REFUSED at its Ruby
   line instead (same discipline as every other out-of-subset construct).

   The return value distinguishes the two cases that must behave differently:
     NULL  -> refuse; the caller names the offending clause
     ""    -> Crystal's bare `rescue`, which catches Exception (a bare Ruby
              `rescue` catches StandardError, a strict subset, so this is a
              documented widening rather than an exact match) */
static const char *cr_exception_name(CrGen *g, const char *ruby) {
  for (int i = 0; i < g->c->nclasses; i++)
    if (sp_streq(g->c->classes[i].name, ruby)) return ruby;  /* a user class wins */
  if (!ruby) return "";
  /* Same name in both languages. These are the whole of Crystal 1.21's
     hierarchy apart from Exception itself, minus the ones with no Ruby
     counterpart (TypeCastError, NilAssertionError, InvalidByteSequenceError are
     Crystal-only and cannot appear in a Ruby rescue clause). */
  if (sp_streq(ruby, "ArgumentError"))  return "ArgumentError";
  if (sp_streq(ruby, "IndexError"))     return "IndexError";
  if (sp_streq(ruby, "KeyError"))       return "KeyError";
  if (sp_streq(ruby, "RuntimeError"))   return "RuntimeError";
  if (sp_streq(ruby, "OverflowError"))  return "OverflowError";
  if (sp_streq(ruby, "NotImplementedError")) return "NotImplementedError";
  if (sp_streq(ruby, "StandardError"))  return "Exception";
  if (sp_streq(ruby, "Exception"))      return "Exception";
  /* Spelled differently, same meaning: CRuby raises ZeroDivisionError for `1/0`
     and Crystal raises DivisionByZeroError. Both catch the same condition. */
  if (sp_streq(ruby, "ZeroDivisionError")) return "DivisionByZeroError";
  return NULL;
}

/* The dotted/colon name of an exception operand in a rescue clause, e.g.
   "Math::DomainError" or "IO::EAGAINWaitReadable". Returns NULL for a node
   that is not a plain class reference (`rescue *list` is a SplatNode, a
   non-class operand is a TypeError in CRuby) -- the caller refuses those. */
static const char *cr_exception_operand(CrGen *g, int id) {
  if (id < 0) return NULL;
  NodeKind k = nt_kind(g->nt, id);
  if (k == NK_ConstantReadNode) return nt_str(g->nt, id, "name");
  if (k != NK_ConstantPathNode) return NULL;
  const char *leaf = nt_str(g->nt, id, "name");
  if (!leaf) return NULL;
  /* Build the full A::B::C path by walking the parent chain. `Math::DomainError`
     has parent ConstantReadNode(Math); deeper paths are rare but legal. */
  int cur = nt_ref(g->nt, id, "parent");
  if (cur < 0) return leaf;
  const char *parent = NULL;
  NodeKind pk = nt_kind(g->nt, cur);
  if (pk == NK_ConstantReadNode) parent = nt_str(g->nt, cur, "name");
  else if (pk == NK_ConstantPathNode) {
    const char *pl = nt_str(g->nt, cur, "name");
    int gpar = nt_ref(g->nt, cur, "parent");
    const char *pp = (gpar >= 0 && nt_kind(g->nt, gpar) == NK_ConstantReadNode)
                     ? nt_str(g->nt, gpar, "name") : NULL;
    if (pl && pp) { char buf[128]; snprintf(buf, sizeof buf, "%s::%s", pp, pl); parent = buf; }
    else parent = pl;
  }
  if (!parent) return leaf;
  static char path[128];
  snprintf(path, sizeof path, "%s::%s", parent, leaf);
  return path;
}

/* The list of classes Crystal 1.21 actually has, for the refusal message. Kept
   in one place so it cannot drift from cr_exception_name above. */
#define CR_NO_CRYSTAL_EXC \
  "Crystal 1.21 defines only Exception, ArgumentError, IndexError, KeyError, " \
  "RuntimeError, DivisionByZeroError, OverflowError, NotImplementedError, " \
  "TypeCastError, NilAssertionError and InvalidByteSequenceError"

/* Emit the `rescue` line of one RescueNode.

   Crystal's clause syntax is not Ruby's. The classes are joined with `|` rather
   than `,`, and the bound variable is introduced with ` : ` -- `rescue e` alone
   is a CATCH-ALL that binds e, and `rescue A | B => e` does not parse at all.
   A clause with both a type list and a binding is `rescue e : A | B`. */
static void cr_emit_rescue_head(CrGen *g, int clause, const char *vn, Buf *b, int indent) {
  int nexc = 0; const int *exc = nt_arr(g->nt, clause, "exceptions", &nexc);

  cr_indent(b, indent);
  buf_puts(b, "rescue");
  /* A binding takes `rescue e` when the clause is catch-all and `rescue e : A`
     when it is restricted. The trailing colon is emitted with the list, so a
     catch-all does not produce a dangling `rescue e :`. */
  if (vn) buf_printf(b, " %s", vn);
  if (vn && nexc > 0) buf_puts(b, " :");
  for (int i = 0; i < nexc; i++) {
    const char *nm = cr_exception_operand(g, exc[i]);
    if (!nm) {
      cr_mark_unsupported(g, exc[i],
        "a rescue clause operand that is not a class reference (Crystal rescue "
        "needs an exception class)");
      buf_printf(b, "%sException", i ? " | " : " ");
      continue;
    }
    const char *cn = cr_exception_name(g, nm);
    if (!cn) {
      /* Refuse the CLASS, not the clause: the message should name the exception
         the programmer actually has to deal with. */
      cr_mark_unsupported(g, exc[i],
        "an exception class with no Crystal equivalent: " CR_NO_CRYSTAL_EXC);
      buf_printf(b, "%sException", i ? " | " : " ");
      continue;
    }
    buf_printf(b, "%s%s", i ? " | " : " ", cn);
  }
  buf_puts(b, "\n");
}

/* begin/rescue/else/ensure. `indent` is the indentation of the `begin` keyword.
   A BeginNode with no rescue/else/ensure protects nothing, so it is emitted as
   plain statements -- that also keeps the `begin ... end while cond` do-while
   idiom from picking up a handler frame. */
static void cr_emit_rescue(CrGen *g, int id, Buf *b, int indent) {
  int body   = nt_ref(g->nt, id, "statements");
  int clause = nt_ref(g->nt, id, "rescue_clause");
  int else_c = nt_ref(g->nt, id, "else_clause");
  int ens_c  = nt_ref(g->nt, id, "ensure_clause");
  int else_stmts = else_c >= 0 ? nt_ref(g->nt, else_c, "statements") : -1;
  int ens_stmts  = ens_c  >= 0 ? nt_ref(g->nt, ens_c,  "statements") : -1;

  if (clause < 0 && else_stmts < 0 && ens_stmts < 0) {
    cr_emit_stmts(g, body, b, indent);
    return;
  }

  cr_indent(b, indent);
  buf_puts(b, "begin\n");
  cr_emit_stmts(g, body, b, indent + 1);

  /* `rescue`/`else`/`ensure` sit at the `begin` level, their bodies one step in:
     the shape a Crystal reader expects. Crystal does not care about columns, but
     the emitted file is meant to be read. */
  int saw_catchall = 0;
  while (clause >= 0) {
    int ref = nt_ref(g->nt, clause, "reference");
    const char *vn = (ref >= 0) ? nt_str(g->nt, ref, "name") : NULL;
    int nexc = 0; nt_arr(g->nt, clause, "exceptions", &nexc);
    /* A catch-all clause followed by a specific one is meaningful in Ruby and
       inexpressible in Crystal. Ruby's bare `rescue` matches StandardError only,
       so a later `rescue Exception` is still reachable for a ScriptError or a
       bare Exception -- that is the whole point of the idiom. Crystal's bare
       `rescue` already matches everything, so the later clause is dead, and the
       compiler rejects the order outright ("specific rescue must come before
       catch-all rescue"). Reordering it would silently change which clause runs,
       so the program is refused instead. */
    if (saw_catchall && nexc > 0) {
      cr_mark_unsupported(g, clause,
        "a specific rescue clause after a catch-all one (Ruby's bare `rescue` "
        "matches StandardError only, so the later clause is reachable there; "
        "Crystal's catch-all already matches everything)");
      break;
    }
    /* A clause that binds no name still has to re-raise correctly, so when the
       Ruby clause is catch-all with no `=> v` a name is synthesized and emitted
       on the rescue line as well, so that a bare `raise` in the body has
       something to name. A synthesized name cannot collide: Ruby code in that
       body has no handle on the exception (`$!` is the only way in, and this
       body is being translated to Crystal), so nothing else can refer to it. */
    char synth[24];
    if (!vn) {
      snprintf(synth, sizeof synth, "_sp_ex%d", g->unsupported + (int)clause);
      vn = synth;
    }
    cr_emit_rescue_head(g, clause, vn, b, indent);
    if (nexc == 0) saw_catchall = 1;
    const char *save = g->rescue_var;
    g->rescue_var = vn;
    cr_emit_stmts(g, nt_ref(g->nt, clause, "statements"), b, indent + 1);
    g->rescue_var = save;
    clause = nt_ref(g->nt, clause, "subsequent");
  }
  if (else_stmts >= 0) {
    cr_indent(b, indent); buf_puts(b, "else\n");
    cr_emit_stmts(g, else_stmts, b, indent + 1);
  }
  if (ens_stmts >= 0) {
    cr_indent(b, indent); buf_puts(b, "ensure\n");
    cr_emit_stmts(g, ens_stmts, b, indent + 1);
  }
  cr_indent(b, indent);
  buf_puts(b, "end\n");
}

static const char *cr_translate_name(CrGen *g, int recv, const char *n) {
  if (sp_streq(n, "include?")) {
    TyKind rt = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;
    /* Hash#include? checks keys; Crystal's Hash answers that as has_key?. */
    return ty_is_hash(rt) ? "has_key?" : "includes?";
  }
  if (sp_streq(n, "length"))   return "size";        /* Array/Hash/String#length -> size */
  /* Ruby spells these predicate methods in the third person; Crystal does not. */
  if (sp_streq(n, "start_with?"))   return "starts_with?";
  if (sp_streq(n, "end_with?"))     return "ends_with?";
  if (sp_streq(n, "exclude_end?"))  return "excludes_end?";
  if (sp_streq(n, "cover?"))        return "covers?";
  /* Class-level predicates: Crystal spells these in the plural. */
  if (sp_streq(n, "exist?"))        return "exists?";
  if (sp_streq(n, "chdir"))         return "cd";
  /* Time#to_i is epoch seconds, which Crystal names to_unix. Only on a Time:
     Integer#to_i is an identity conversion that Crystal has no need to name. */
  if (sp_streq(n, "to_i") && recv >= 0 && comp_ntype(g->c, recv) == TY_TIME)
    return "to_unix";
  return n;
}

/* A method that does not exist on this receiver, so the call is a guaranteed
   "undefined method" build error. Returns the refusal reason, or NULL.

   This is the other half of the honesty gate. cr_builtin_ok is a NAME
   allowlist, so it can only refuse a name it does not know -- it cannot notice
   that `<<` is fine on an Array and undefined on a String, or that a class
   receiver like `Time` is not concrete in this type model and so never reaches
   the concrete-receiver check in cr_emit_call at all. Those names are in
   cr_builtin_ok (or slip past it entirely) and still cannot compile, so they are
   matched here by (receiver, name).

   Keyed by receiver rather than by name alone on purpose: `<<` must keep working
   on Array, `<<` on String is refused, and a program may define either name on
   its own class. User methods are checked first by the caller, so a class the
   program defines is never matched here. */
static const char *cr_missing_method(CrGen *g, int id, int recv, const char *name) {
  if (!name) return NULL;
  /* The module or class a call is on, for the table below. A bare constant
     (`GC.start`) names it directly; `Thread.new{...}.value` names it through the
     constructor, so resolve one `new` hop as well. NULL when the receiver is a
     value of some other shape. The type model gives a bare class receiver
     TY_UNKNOWN, so the generic concrete-receiver gate in cr_emit_call cannot see
     these calls at all; that is why the check is by name here. */
  const char *mod = NULL;
  if (recv >= 0 && nt_kind(g->nt, recv) == NK_ConstantReadNode) {
    mod = nt_str(g->nt, recv, "name");
  } else if (recv >= 0 && nt_kind(g->nt, recv) == NK_CallNode) {
    int inner = nt_ref(g->nt, recv, "receiver");
    const char *ctor = nt_str(g->nt, recv, "name");
    if (inner >= 0 && ctor && sp_streq(ctor, "new") &&
        nt_kind(g->nt, inner) == NK_ConstantReadNode)
      mod = nt_str(g->nt, inner, "name");
  }
  struct { const char *recv; const char *meth; const char *why; } const t[] = {
    { "GC",      "start", "Crystal's GC has no start" },
    { "GC",      "stat",  "Crystal's GC has no stat; timing is Process.monotonic_time" },
    { "Thread",  "value", "Crystal's Thread has no value; join then read a field" },
    { "File",    "stat",  "Crystal's File has no class-level stat; use File.info" },
    { "File",    "lstat", "Crystal's File has no class-level lstat" },
    { "Time",    "to_a",  "Crystal's Time has no to_a" },
    { "Kernel",  "gets",  "Crystal reads stdin with gets, not Kernel.gets" },
  };
  if (mod) {
    /* A class the program defines shadows the stdlib name, so a call on it is
       the program's own method and not the missing one. */
    for (int i = 0; i < g->c->nclasses; i++)
      if (sp_streq(g->c->classes[i].name, mod)) return NULL;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
      if (sp_streq(t[i].recv, mod) && sp_streq(t[i].meth, name)) return t[i].why;
    /* Time.at is translated, not refused -- but only for a single integer
       argument, which is the one shape Crystal's Time.unix matches. Any other
       argument has no equivalent, and saying so is better than emitting the
       rewrite and letting it fail as "expected Int, not Float64". */
    if (sp_streq(mod, "Time") && sp_streq(name, "at")) {
      int ac = 0; const int *av = call_args(g->nt, id, &ac);
      if (ac != 1 || comp_ntype(g->c, av[0]) != TY_INT)
        return "Crystal's Time.unix takes one Int; Time.at with a fractional or "
               "sub-second argument has no Crystal form";
    }
  }
  /* A receiver with an inferred value type. Match on the Crystal type the
     emitter would have written, so the rule is stated in the same terms as the
     output. */
  TyKind rt = (recv >= 0) ? comp_ntype(g->c, recv) : TY_UNKNOWN;
  struct { TyKind type; const char *meth; const char *why; } const v[] = {
    { TY_STRING, "<<",
      "Crystal's String is immutable and has no <<; a Ruby append-in-place needs a mutable buffer" },
    { TY_RANGE,  "lazy",
      "Crystal's Range has no lazy; there is no lazy enumerator to build" },
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
    if (v[i].type == rt && sp_streq(v[i].meth, name)) return v[i].why;
  if (ty_is_hash(rt)) {
    if (sp_streq(name, "freeze"))   return "Crystal has no freeze; a Hash is not frozen at runtime";
    if (sp_streq(name, "frozen?"))  return "Crystal has no frozen?";
    if (sp_streq(name, "sort"))     return "Hash has no sort; collect the pairs and sort those";
    if (sp_streq(name, "sort_by"))  return "Hash has no sort_by; collect the pairs and sort those";
  }
  if (rt == TY_MUTEX && sp_streq(name, "locked?"))
    return "Crystal's Mutex has no locked?";
  return NULL;
}

/* A Ruby method whose Crystal equivalent is a DIFFERENT call shape, not just a
   different name, so cr_translate_name cannot express it. Returns 1 and writes
   the whole call (receiver, arguments and all) when it applies.

   Only exact-semantics rewrites belong here. `Time.at` and `Time.unix` agree on
   the instant, but Ruby's Time.at is in LOCAL time while Crystal's Time.unix is
   UTC, so the rewrite carries `.to_local`; without it the value's zone would
   silently differ. `Dir.rmdir` and `Dir.delete` are the same operation. */
static int cr_call_remap(CrGen *g, int id, int recv, const char *name, Buf *b) {
  if (!name || recv < 0) return 0;
  if (nt_kind(g->nt, recv) != NK_ConstantReadNode) return 0;
  const char *mod = nt_str(g->nt, recv, "name");
  if (!mod) return 0;
  for (int i = 0; i < g->c->nclasses; i++)
    if (sp_streq(g->c->classes[i].name, mod)) return 0;  /* a user class wins */
  int argc = 0; const int *argv = call_args(g->nt, id, &argc);
  if (sp_streq(mod, "Time") && sp_streq(name, "at")) {
    /* Crystal's Time.unix takes an Int, and Ruby has no sub-argument form. So the
       rewrite only holds for a single integer-valued argument. `Time.at(1.5)`
       and `Time.at(secs, usec)` are refused rather than rewritten, because
       emitting the rewrite anyway would only move the failure to a Crystal
       "expected Int, not Float64" -- the same build error with less of a clue. */
    if (argc != 1 || comp_ntype(g->c, argv[0]) != TY_INT) return 0;
    buf_puts(b, "Time.unix(");
    for (int i = 0; i < argc; i++) {
      if (i) buf_puts(b, ", ");
      cr_emit_expr(g, argv[i], b);
    }
    buf_puts(b, ").to_local");
    return 1;
  }
  /* Time.now is "the current time in local zone", which is Crystal's
     Time.local with no arguments. Crystal has no Time.now. */
  if (sp_streq(mod, "Time") && sp_streq(name, "now") && argc == 0) {
    buf_puts(b, "Time.local");
    return 1;
  }
  if (sp_streq(mod, "Dir") && sp_streq(name, "rmdir")) {
    buf_printf(b, "Dir.delete(");
    for (int i = 0; i < argc; i++) {
      if (i) buf_puts(b, ", ");
      cr_emit_expr(g, argv[i], b);
    }
    buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* A method name is emittable only if it is a Crystal stdlib method whose
   semantics match Ruby's on the supported types. This is the honesty gate: an
   unknown name used to pass through verbatim and surface later as a silent
   `undefined method` build error (or, worse, a method that exists in Crystal
   but means something different). Refusing it here names the gap at the Ruby
   line instead. Deliberately conservative -- Phase 2 grows this into a full
   receiver-type-aware table; for now anything not listed is refused. */
static int cr_builtin_ok(const char *n) {
  static const char *const ok[] = {
    /* Kernel / top-level (receiverless) */
    "puts", "print", "p", "warn", "raise", "gets", "exit", "abort", "sleep",
    "loop", "tap",
    /* Array / Enumerable / Range -- each verified present in Crystal 1.21 with
       matching semantics on the supported types (inject/lazy/slice/frozen?/
       equal?/freeze/then/respond_to? are NOT in Crystal and stay refused). */
    "size", "length", "empty?", "first", "last", "each", "map", "select",
    "reject", "sum", "times", "join", "push", "<<", "pop", "shift", "unshift",
    "concat", "reverse", "sort", "sort_by", "flat_map", "reduce", "count",
    "fetch", "each_with_index", "any?", "all?", "include?", "includes?",
    "index", "rindex", "to_a", "uniq", "zip", "values_at", "each_cons",
    "each_slice", "step",
    /* Verified by build-and-run against Ruby, not just by compiling -- each was
       exercised on a realistic receiver and its output diffed byte-for-byte:
          Array/Enumerable: fill, each_index, max, min, map!, slice_when,
                            compact, flatten, chunk_while
          Integer#upto, Object#itself, Hash#dig, String#tr
        (Enumerable-with-block methods the analyzer desugars into a helper --
         take_while, find, each_with_index, ... -- are refused at that helper and
         never reach this table, so they are not listed here.)
        Deliberately NOT added from the same sweep, because Crystal's version is
       not Ruby's: product/union/divmod/rpartition/partition/minmax return a
       Tuple where Ruby returns an Array (so `p` and `.class` differ), bsearch
       disagrees on a no-match, and union/difference/take/except/default/
       delete_if/pow/ord/casecmp/swapcase/chop/to_sym/intern/delete_suffix/
       reverse_each/ljust/center/then/yield_self do not exist in Crystal at all. */
    "fill", "each_index", "max", "min", "map!", "slice_when", "upto",
    "itself", "dig", "tr",
    "compact", "flatten", "chunk_while",
    /* Hash -- verified byte-identical: each_key, each_value, transform_values,
       transform_keys, invert, clear (also on Array). to_h was verified on an
       Array of pairs but is NOT listed: it is net-negative in the corpus because
       a name-based allowlist lets it through on a Struct receiver, where Crystal
       fails -- a clear refusal is better than that build error. */
    "keys", "values", "has_key?", "key?", "each_pair", "merge", "delete",
    "each_key", "each_value", "transform_values", "transform_keys", "invert",
    "clear",
    /* String -- verified byte-identical: squeeze. chomp is NOT listed for the
       same reason as to_h: it is correct on a plain String but net-negative once
       allowed on a nilable receiver (`gets.chomp`), where Crystal fails. */
    "upcase", "downcase", "capitalize", "strip", "lstrip", "rstrip", "gsub",
    "sub", "split", "chars", "to_s", "to_i", "to_f", "start_with?", "end_with?",
    "bytes", "bytesize", "squeeze",
    /* Each verified present in Crystal 1.21 AND verified to produce the same
       output as Ruby on a build-and-run comparison, not just to compile:
         String#match      -> Regex::MatchData, same indexing and pre_match
         String#=~         -> same integer offset
         Exception#message -> same string
         Class#name        -> same module/class name string
         Kernel#at_exit    -> runs at exit, in order
       Deliberately NOT added, because Crystal's version is not Ruby's:
         Object#class  -> Ruby's `5.class` is Integer, Crystal's is Int32
         String#scan   -> Ruby's yields Strings, Crystal's yields MatchData
         match?/freeze/frozen?/inject/slice/slice!/lazy/send/equal? do not
         exist in Crystal at all. Refusing those is the honest answer; adding
         one would trade a named refusal for a silently wrong value. */
    "match", "=~", "message", "name", "at_exit",
    /* Numeric -- succ verified byte-identical (on Int and String). modulo is NOT
       listed: it is correct for same-type operands but net-negative once allowed
       on mixed Int/Float operands, where Crystal's `%` requires matching types. */
    "abs", "ceil", "floor", "round", "truncate", "div", "gcd", "lcm", "clamp",
    "odd?", "even?", "succ",
    /* Math (called as Math.x) */
    "sqrt", "sin", "cos", "tan", "log", "exp",
    /* Object / constructors */
    "new", "nil?", "inspect", "dup",
  };
  for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
    if (sp_streq(n, ok[i])) return 1;
  return 0;
}

/* True when `name` is a method the program itself defines and the call can reach:
   either a top-level def, a def on the receiver's user class, or (for a
   receiverless call) a def on the enclosing class -- Ruby's implicit self. */
static int cr_user_method_ok(CrGen *g, const char *name, TyKind rt, int bare) {
  for (int s = 1; s < g->c->nscopes; s++) {
    Scope *sc = &g->c->scopes[s];
    if (!sc->name || !sp_streq(sc->name, name)) continue;
    if (bare && (sc->class_id == -1 || sc->class_id == g->cur_class)) return 1;
    if (!bare && ty_is_object(rt) && sc->class_id == ty_object_class(rt)) return 1;
  }
  /* attr_accessor/reader/writer generate methods the analyzer records in the
     class's readers[]/writers[] rather than as def scopes, so a call to them
     (c.x / c.x = v) must be accepted here or it is refused as "not in subset". */
  if (!bare && ty_is_object(rt)) {
    int ci = ty_object_class(rt);
    if (ci >= 0 && ci < g->c->nclasses) {
      ClassInfo *cl = &g->c->classes[ci];
      for (int i = 0; i < cl->nreaders; i++)
        if (sp_streq(cl->readers[i], name)) return 1;
      size_t nl = strlen(name);
      if (nl >= 2 && name[nl - 1] == '=') {   /* setter: "x=" -> base "x" */
        for (int i = 0; i < cl->nwriters; i++) {
          const char *w = cl->writers[i];
          if (strlen(w) == nl - 1 && strncmp(w, name, nl - 1) == 0) return 1;
        }
      }
    }
  }
  return 0;
}

/* Emit a constant path (C::CONST, A::B::C) by walking the parent chain. */
static void cr_emit_const_path(CrGen *g, int id, Buf *b) {
  int parent = nt_ref(g->nt, id, "parent");
  /* `Float::INFINITY` is a constant PATH whose leaf has no Crystal member, so
     the whole path is what decides: remap when the path has an exact Crystal
     spelling, refuse when it names something Crystal does not define. A nested
     path (`A::B::C`) is left to the recursive walk below, since these tables
     only know single-level names. */
  if (parent >= 0 && nt_kind(g->nt, parent) == NK_ConstantReadNode) {
    const char *owner = nt_str(g->nt, parent, "name");
    const char *leaf  = nt_str(g->nt, id, "name");
    if (owner && leaf) {
      int user_class = 0;
      for (int ci = 0; ci < g->c->nclasses; ci++)
        if (sp_streq(g->c->classes[ci].name, owner)) { user_class = 1; break; }
      if (cr_const_remap(owner, leaf, b)) return;
      const char *why = user_class ? NULL : cr_missing_const(owner, leaf);
      if (why) {
        char msg[176];
        snprintf(msg, sizeof msg, "the constant '%s::%s' (%s)", owner, leaf, why);
        cr_mark_unsupported(g, id, msg);
        buf_puts(b, "nil");
        return;
      }
    }
  }
  if (parent >= 0) {
    if (nt_kind(g->nt, parent) == NK_ConstantPathNode) cr_emit_const_path(g, parent, b);
    else { const char *n = nt_str(g->nt, parent, "name"); buf_puts(b, n ? n : ""); }
  }
  buf_puts(b, "::");
  const char *nm = nt_str(g->nt, id, "name");
  buf_puts(b, nm ? nm : "");
}

/* ---- expressions ---- */
static void cr_emit_expr(CrGen *g, int id, Buf *b) {
  if (id < 0) { buf_puts(b, "nil"); return; }
  /* nil_ok describes the node being emitted, never a subtree: only an index read
     consults it, and a parent sets it for that one child. Clearing it here means
     `p(h[k] + 1)` cannot reach through the `+` and make its operand a nilable
     read, while `p(h[k])` still gets Ruby's nil. */
  int saved_nil_ok = g->nil_ok;
  g->nil_ok = 0;
  NodeKind k = nt_kind(g->nt, id);
  TyKind t = comp_ntype(g->c, id);
  if (t == TY_POLY || t == TY_UNKNOWN) g->poly_sites++;

  /* An integer the inference promoted past 64 bits. The C backend auto-promotes
     these to Bigint; Crystal's BigInt needs a shard this emitter does not pull in,
     so such a value cannot be represented -- refuse it (a Phase 2 gap). */
  if (t == TY_BIGINT) {
    cr_mark_unsupported(g, id, "an integer that overflows 64 bits");
    buf_puts(b, "nil");
    return;
  }

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
      const char *s = nt_str(g->nt, id, "value");
      buf_printf(b, ":%s", s ? s : "");
      return;
    }
    case NK_TrueNode:  buf_puts(b, "true");  return;
    case NK_FalseNode: buf_puts(b, "false"); return;
    case NK_NilNode:   buf_puts(b, "nil");   return;
    case NK_LocalVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");
      buf_puts(b, cr_local(n));
      return;
    }
    case NK_InstanceVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");   /* includes the leading @ */
      buf_puts(b, n ? n : "");
      return;
    }
    case NK_ConstantReadNode: {
      const char *n = nt_str(g->nt, id, "name");
      if (n) {
        /* A name Crystal has with the same value, under a different spelling:
           translate it rather than refuse it. Checked first, because a remap is
           the better answer when one exists. */
        if (cr_const_remap(NULL, n, b)) return;
        /* A Ruby stdlib name with no Crystal counterpart. Refusing it here names
           the gap at the Ruby line, instead of shipping `Data` or `RUBY_ENGINE`
           into the output where it becomes an "undefined constant" build error
           with no hint that the name is the problem. Skipped when the program
           defines a class of the same name: that is a different binding, and it
           resolves. */
        int user_class = 0;
        for (int ci = 0; ci < g->c->nclasses; ci++)
          if (sp_streq(g->c->classes[ci].name, n)) { user_class = 1; break; }
        const char *why = user_class ? NULL : cr_missing_const(NULL, n);
        if (why) {
          char msg[176];
          snprintf(msg, sizeof msg, "the constant '%s' (%s)", n, why);
          cr_mark_unsupported(g, id, msg);
          buf_puts(b, "nil");
          return;
        }
        /* Ruby's core class names are spelled differently in Crystal, and the
           Ruby spellings are not defined there (`is_a?(Integer)` is a
           compile-time "undefined constant"). A user class of the same name
           wins, so this only fires for the names Crystal actually has. */
        const char *m = cr_core_class_name(g, n);
        buf_puts(b, m ? m : n);
      } else buf_puts(b, "");
      return;
    }
    case NK_ConstantPathNode:
      cr_emit_const_path(g, id, b);
      return;
    case NK_SelfNode:
      /* Crystal's self has the same instance/class meaning as Ruby's inside a
         method body. */
      buf_puts(b, "self");
      return;
    case NK_ArrayNode: {
      int n = 0; const int *els = nt_arr(g->nt, id, "elements", &n);
      if (n == 0) {
        /* Crystal cannot infer an empty literal's element type. `[] of T` is the
           annotation form and only parses as a whole expression, so it is a
           syntax error the moment the array is a method receiver
           (`[] of T.zip()`); `Array(T).new` is the same value in expression
           position. A poly element type has no Crystal spelling at all. */
        Buf el; memset(&el, 0, sizeof el);
        if (!cr_array_elem_type(g, comp_ntype(g->c, id), &el)) {
          cr_mark_unsupported(g, id, "empty array with a poly/unknown element type "
                                    "(Crystal 1.21 has no untyped type)");
          free(el.p);
          buf_puts(b, "[] of Int64");
          return;
        }
        buf_printf(b, "Array(%s).new", el.p);
        free(el.p);
        return;
      }
      buf_puts(b, "[");
      for (int i = 0; i < n; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, els[i], b); }
      buf_puts(b, "]");
      return;
    }
    case NK_HashNode: {
      int n = 0; const int *els = nt_arr(g->nt, id, "elements", &n);
      if (n == 0) {
        /* As with an empty array literal, Crystal has no element type to infer
           and its `{} of K => V` spelling is annotation-only, so use the
           expression-position constructor. A poly hash has no Crystal type. */
        Buf ht; memset(&ht, 0, sizeof ht);
        if (!cr_map_type(g, comp_ntype(g->c, id), &ht)) {
          cr_mark_unsupported(g, id, "empty hash with a poly/unknown key or value type "
                                    "(Crystal 1.21 has no untyped type)");
          free(ht.p);
          buf_puts(b, "{ }");
          return;
        }
        buf_printf(b, "%s.new", ht.p);
        free(ht.p);
        return;
      }
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
      buf_puts(b, "(");
      /* `x || y` is exactly the shape that reads a possibly-missing value. */
      int sav = g->nil_ok; g->nil_ok = 1;
      cr_emit_expr(g, l, b);
      g->nil_ok = sav;
      buf_puts(b, " || "); cr_emit_expr(g, r, b); buf_puts(b, ")");
      return;
    }
    case NK_RescueModifierNode: {
      /* `expr rescue fallback`. Crystal has no modifier form, so this becomes a
         begin/rescue block -- which is a value, so the parens keep it usable
         anywhere an expression is (including as an argument or a receiver). */
      int e = nt_ref(g->nt, id, "expression");
      int r = nt_ref(g->nt, id, "rescue_expression");
      buf_puts(b, "(begin\n");
      cr_emit_expr(g, e, b);
      /* The block is catch-all, so it needs a name for a bare `raise` in the
         handler; `_sp_rm` is fresh in this body for the same reason as the
         synthesized clause names in cr_emit_rescue. */
      const char *save = g->rescue_var;
      g->rescue_var = "_sp_rm";
      buf_puts(b, "\nrescue _sp_rm\n");
      cr_emit_expr(g, r, b);
      buf_puts(b, "\nend)");
      g->rescue_var = save;
      return;
    }
    case NK_BeginNode: {
      /* A begin/rescue used as a VALUE (`v = begin ... rescue ... end`), which
         is the same node the statement form uses. Emitted at indent 0 inside
         parens so the newlines it introduces cannot run into whatever encloses
         the expression. */
      buf_puts(b, "(");
      cr_emit_rescue(g, id, b, 0);
      buf_puts(b, ")");
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
    case NK_CallNode: {
      /* Hand the flag to the call emitter ONLY for an index read -- the one place
         that consults it. Restoring it for any other call would let it reach the
         receiver: in `p(h[k] + 1)`, p's argument is the `+`, and the `+`'s
         receiver must not become a nilable read. */
      const char *cnm = nt_str(g->nt, id, "name");
      if (cnm && sp_streq(cnm, "[]") && nt_ref(g->nt, id, "receiver") >= 0)
        g->nil_ok = saved_nil_ok;
      cr_emit_call(g, id, b);
      g->nil_ok = 0;
      return;
    }
    /* A Ruby /pattern/ literal. Crystal's regex literal is the same syntax --
       trailing option flags (/i /m /x) and inline interpolation included -- so
       the pattern text drops in verbatim (it is regex source, not a Crystal
       string, so it must NOT be string-escaped). The Prism flag bits map to the
       same letters: IGNORE_CASE=4 -> i, MULTI_LINE=16 -> m, EXTENDED=8 -> x. */
    case NK_RegularExpressionNode: {
      const char *pat = nt_str(g->nt, id, "unescaped");
      int flags = nt_int(g->nt, id, "flags", 0);
      buf_puts(b, "/");
      cr_emit_regex_pat(b, pat);
      buf_puts(b, "/");
      if (flags & 4)  buf_puts(b, "i");
      if (flags & 16) buf_puts(b, "m");
      if (flags & 8)  buf_puts(b, "x");
      return;
    }
    case NK_InterpolatedRegularExpressionNode: {
      int n = 0; const int *parts = nt_arr(g->nt, id, "parts", &n);
      int flags = nt_int(g->nt, id, "flags", 0);
      buf_puts(b, "/");
      for (int i = 0; i < n; i++) {
        NodeKind pk = nt_kind(g->nt, parts[i]);
        if (pk == NK_StringNode) {
          const char *s = nt_str(g->nt, parts[i], "content");
          cr_emit_regex_pat(b, s);   /* regex text: escape only the delimiter */
        } else if (pk == NK_EmbeddedStatementsNode) {
          int stmts = nt_ref(g->nt, parts[i], "statements");
          buf_puts(b, "#{");
          int e = cr_single_expr(g->nt, stmts);
          if (e >= 0) cr_emit_expr(g, e, b); else cr_emit_stmts(g, stmts, b, 0);
          buf_puts(b, "}");
        }
      }
      buf_puts(b, "/");
      if (flags & 4)  buf_puts(b, "i");
      if (flags & 16) buf_puts(b, "m");
      if (flags & 8)  buf_puts(b, "x");
      return;
    }
    default: {
      /* An expression node outside the subset (SelfNode, GlobalVariableReadNode,
         LambdaNode, ...). Emit a placeholder and record it; the run is refused at
         the end so this never ships as a silent `nil`. */
      const char *ty = nt_type(g->nt, id);
      cr_mark_unsupported(g, id, ty ? ty : "expression");
      buf_puts(b, "nil");
      return;
    }
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

/* ---- case / when / else ----
   Crystal's case/when matches with `===` exactly like Ruby's, so literal, range,
   and class conditions translate directly. A condition that is itself unsupported
   is refused by cr_emit_expr. */
static void cr_emit_case(CrGen *g, int id, Buf *b, int indent) {
  int subj = nt_ref(g->nt, id, "predicate");
  int wn = 0; const int *whens = nt_arr(g->nt, id, "conditions", &wn);
  int els = nt_ref(g->nt, id, "else_clause");

  cr_indent(b, indent);
  buf_puts(b, "case ");
  cr_emit_expr(g, subj, b);
  buf_puts(b, "\n");
  for (int i = 0; i < wn; i++) {
    int cn = 0; const int *conds = nt_arr(g->nt, whens[i], "conditions", &cn);
    int body = nt_ref(g->nt, whens[i], "statements");
    cr_indent(b, indent + 1);
    buf_puts(b, "when ");
    for (int j = 0; j < cn; j++) { if (j) buf_puts(b, ", "); cr_emit_expr(g, conds[j], b); }
    buf_puts(b, "\n");
    cr_emit_stmts(g, body, b, indent + 2);
  }
  if (els >= 0 && nt_kind(g->nt, els) == NK_ElseNode) {
    int ebody = nt_ref(g->nt, els, "statements");
    cr_indent(b, indent + 1);
    buf_puts(b, "else\n");
    cr_emit_stmts(g, ebody, b, indent + 2);
  }
  cr_indent(b, indent);
  buf_puts(b, "end\n");
}

/* ---- blocks: recv.<name> do |p| ... end ---- */
/* A Ruby block over a one-value-yielding iterator with 2+ params auto-splats the
   single yielded value: `[[1,2],[3,4]].each { |a, b| }` binds a and b to the
   inner array's elements. Crystal does not: it yields exactly one parameter, so
   `do |a, b|` is "too many block parameters (given 2, expected maximum 1)".

   Crystal's own `|(a, b)|` syntax would be the literal translation, but it is
   wrong for the iterators that really do yield two values -- `Hash#each` and
   `each_with_index` -- which are already correct. So the split is made on the
   yield arity instead: `ty_block_yield` (types.c) is the single source of truth
   for builtin block protocols, and it returns 1 for `each`/`map`/`sort_by`/...
   and 2 for the pair-yielding ones. Only the 1-yield case is desugared.

   The destructure is emitted as an assignment INSIDE the body
   (`do |__cr_elem| ; a, b = __cr_elem ; ...`) rather than as `|(a, b)|`. That
   form is element-shape agnostic -- it binds a Tuple from `zip` and an Array
   from `each_slice(2)` equally well -- so it cannot regress an iterator that was
   already building, and it mirrors what the shared analyzer already does for a
   parenthesized `|(a, b)|` source param (analyze_pass.c, block_destr_params). */
static void cr_emit_block(Buf *b, CrGen *g, int block, int indent, int recv,
                          const char *meth) {
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
  /* Ruby auto-splats only when the block asks for more values than the iterator
     yields. A parenthesized `|(a, b)|` was already desugared upstream into a
     single param, so seeing 2+ REQUIRED params here is the flat `|a, b|` form
     and the auto-splat is what Ruby would do. */
  TyKind yields[4];
  int splat = 0;
  if (pn >= 2 && recv >= 0 && meth && ty_block_yield(comp_ntype(g->c, recv),
                                                      meth, yields, 4) == 1)
    splat = 1;
  char elem[48] = "";
  if (splat) snprintf(elem, sizeof elem, "__cr_elem%d", g->tmp_seq++);
  buf_puts(b, (splat || pn > 0) ? " do |" : " do");
  if (splat) buf_puts(b, elem);
  for (int i = 0; i < pn && !splat; i++) {
    const char *rn = nt_str(g->nt, preqs[i], "name");
    if (i) buf_puts(b, ", ");
    buf_puts(b, cr_local(rn));
  }
  /* The real param names are declared by the destructuring assignment below, but
     they must be visible to the nil-safety name set either way: the assignment is
     an ordinary write, and the body reads them exactly as before. */
  for (int i = 0; i < pn; i++)
    nameset_add(&g->locals, nt_str(g->nt, preqs[i], "name"));
  if (pn > 0) buf_puts(b, "|");
  buf_puts(b, "\n");
  if (splat) {
    cr_indent(b, indent + 1);
    for (int i = 0; i < pn; i++) {
      if (i) buf_puts(b, ", ");
      buf_puts(b, cr_local(nt_str(g->nt, preqs[i], "name")));
    }
    buf_printf(b, " = %s\n", elem);
  }
  /* The last statement of a block body is the block's return value, not a
     discarded read, so record it: the "value thrown away" rule must not turn its
     trailing index read into a nilable one. */
  int saved_tail = g->tail_value;
  g->tail_value = cr_tail_stmt(g, body);
  cr_emit_stmts(g, body, b, indent + 1);
  g->tail_value = saved_tail;
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
  /* unary -x / +x on a value: Prism names these "-@" / "+@" with the operand as
     the receiver. Crystal has the same unary operators. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "-@") || sp_streq(name, "+@"))) {
    buf_printf(b, "%c(", name[0]); cr_emit_expr(g, recv, b); buf_puts(b, ")");
    return;
  }
  /* Ruby's bare `raise` re-raises `$!`. Crystal has no `$!`: inside a rescue
     block the current exception is the clause's bound name, and re-raising means
     `raise e`. Outside any clause there is nothing to re-raise, and Ruby would
     be raising RuntimeError from nil, so refuse rather than emit a no-arg
     `raise` that fails to compile ("wrong number of arguments, expected 1"). */
  if (recv < 0 && sp_streq(name, "raise") && argc == 0) {
    if (g->rescue_var) { buf_printf(b, "raise(%s)", g->rescue_var); return; }
    cr_mark_unsupported(g, id, "a bare `raise` outside a rescue clause "
                                "(Crystal re-raises with `raise e`)");
    buf_puts(b, "nil");
    return;
  }

  /* Ruby's `raise Class, message` builds and raises the exception in one step;
     Crystal's `raise` takes a single value, so the message has to become the
     constructor argument. Emitting the two-argument call straight through
     produces a valid-looking file that fails with "wrong number of arguments
     for 'raise' (given 2, expected 1)" -- and since `raise` is how most rescue
     clauses get exercised, that would hide the rescue translation's own bugs
     behind a raise bug. */
  if (recv < 0 && sp_streq(name, "raise") && argc == 2) {
    int cls = cr_peek(g, argv[0]);
    /* Only a class reference becomes `Klass.new(msg)`; `raise obj` and
       `raise SomeFactory.call` already pass a value, and `raise SomeClass`
       with no message stays a plain one-argument raise. */
    if (cls >= 0 && (nt_kind(g->nt, cls) == NK_ConstantReadNode ||
                     nt_kind(g->nt, cls) == NK_ConstantPathNode)) {
      const char *nm = cr_exception_operand(g, cls);
      const char *cn = nm ? cr_exception_name(g, nm) : NULL;
      if (cn && *cn) {
        buf_printf(b, "raise(%s.new(", cn);
        cr_emit_expr(g, argv[1], b);
        buf_puts(b, "))");
        return;
      }
    }
    /* A class with no Crystal equivalent cannot be constructed; refuse it here
       rather than emitting an undefined constant. */
    if (cls >= 0 && (nt_kind(g->nt, cls) == NK_ConstantReadNode ||
                     nt_kind(g->nt, cls) == NK_ConstantPathNode)) {
      cr_mark_unsupported(g, cls,
        "an exception class with no Crystal equivalent: " CR_NO_CRYSTAL_EXC);
    }
  }

  /* Ruby's Array#at is `[]` with a nil sentinel out of range; Crystal's `at`
     raises IndexError, so it maps onto the `[]?` form. */
  if (recv >= 0 && sp_streq(name, "at") && argc == 1 && ty_is_array(comp_ntype(g->c, recv))) {
    cr_emit_expr(g, recv, b);
    buf_puts(b, "[");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, "]?");
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
    /* `x == nil` / `x != nil` is the other spelling of "does this read have a
       value", so the left operand is a nil-tolerant position too. */
    int savb = g->nil_ok;
    if ((sp_streq(name, "==") || sp_streq(name, "!=")) &&
        nt_kind(g->nt, argv[0]) == NK_NilNode)
      g->nil_ok = 1;
    cr_emit_expr(g, recv, b);
    g->nil_ok = savb;
    buf_printf(b, " %s ", op);
    cr_emit_expr(g, argv[0], b);
    return;
  }

  /* index read: Crystal uses bracket syntax, not a .[]() call.
     Ruby's `h[k]` / `a[i]` answer nil for a missing key or an out-of-range index;
     Crystal's `[]` raises KeyError/IndexError. The `?` form is the translation
     that keeps Ruby's answer. Restricted to a single scalar index on a receiver
     the inference knows is a Hash or Array, so a Range slice (`a[1..2]`) or a
     non-container receiver keeps the raising form rather than a guess. */
  if (recv >= 0 && sp_streq(name, "[]")) {
    TyKind rt = comp_ntype(g->c, recv);
    int nilable = g->nil_ok && argc == 1 &&
                  nt_kind(g->nt, argv[0]) != NK_RangeNode &&
                  (ty_is_hash(rt) || ty_is_array(rt));
    /* The flag is about THIS read only. Clear it for the receiver and the index
       so a nested read is not silently made nilable: in `t[k][j]`, making the
       inner `t[k]` nilable would change the type of the outer receiver. */
    int savr = g->nil_ok; g->nil_ok = 0;
    cr_emit_expr(g, recv, b);
    buf_puts(b, "[");
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
    g->nil_ok = savr;
    buf_puts(b, nilable ? "]?" : "]");
    return;
  }

  /* index write: `a[i] = v` reaches the analyzer as a .[]= call. Crystal's
     bracket assignment is an expression that evaluates to the stored value,
     the same as Ruby's, so this holds in statement and value position alike. */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    cr_emit_expr(g, recv, b);
    buf_puts(b, "[");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, "] = ");
    cr_emit_expr(g, argv[1], b);
    return;
  }

  /* method call: refuse a name we cannot verify, so an unknown method never
     ships as a silent build error. A nil placeholder keeps emission going; the
     run is refused at the end (codegen_program_crystal). */
  {
    /* A call into a compiler-synthesized "__" helper: the analyzer desugars some
       Ruby methods (between?, <=>, Enumerable#sum, ...) into __cmp_/__enumw_/
       __bam_ wrappers that the C backend emits but this one deliberately skips
       (Crystal's stdlib answers them). We never emit those bodies, so a call to
       one would dangle -- refuse it rather than ship a reference to nothing. */
    if (name[0] == '_' && name[1] == '_') {
      cr_mark_unsupported(g, id, "a compiler-synthesized helper call");
      buf_puts(b, "nil");
      return;
    }
    TyKind rt = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;
    int bare = (recv < 0);
    /* is_a?(Const): Crystal's is_a? takes a TYPE, so a constant argument (a
       builtin or user class name) is valid and emitted as-is. A non-constant
       argument (a variable holding a class) has no Crystal form -- refused. */
    int isa_const = sp_streq(name, "is_a?") && argc == 1 &&
                    nt_kind(g->nt, argv[0]) == NK_ConstantReadNode;
    int user_meth = cr_user_method_ok(g, name, rt, bare);
    /* A method Crystal does not have on THIS receiver, whatever its name is in
       the allowlist. Checked after the user-method test, so a method the program
       defines always wins, and before the allowlist gate, which cannot see a
       receiver-dependent difference like String#<< versus Array#<<. */
    const char *missing = user_meth ? NULL : cr_missing_method(g, id, recv, name);
    if (missing) {
      char why[176];
      snprintf(why, sizeof why, "method '%s' is not available on this receiver (%s)",
               name, missing);
      cr_mark_unsupported(g, id, why);
      buf_puts(b, "nil");
      return;
    }
    if (!cr_builtin_ok(name) && !isa_const && !user_meth) {
      /* A receiverless call, or a call on a CONCRETE type, is verifiable: an
         unknown method there would be a Crystal build error, so refuse it. A call
         on an untyped (poly/unknown) receiver defers to Crystal's dynamic dispatch
         -- neither we nor Crystal can verify it statically, so allow it through. */
      int concrete = 0;
      if (!bare) { Buf probe; memset(&probe, 0, sizeof probe); concrete = cr_map_type(g, rt, &probe); free(probe.p); }
      if (bare || concrete) {
        char why[128];
        snprintf(why, sizeof why, "method '%s' is not in the Crystal subset", name);
        cr_mark_unsupported(g, id, why);
        buf_puts(b, "nil");
        return;
      }
    }
  }
  /* A call whose Crystal equivalent has a different shape, not just a different
     name (`Time.at` -> `Time.unix(..).to_local`). Emitted whole, so it runs
     before the generic receiver/method/argument form below. */
  if (block < 0 && cr_call_remap(g, id, recv, name, b)) return;
  const char *mname = cr_translate_name(g, recv, name);
  if (recv >= 0) {
    /* `x.nil?` asks whether x is nil, and `x.inspect` / `x.to_s` are defined on
       Nil in Crystal, so all three want Ruby's nilable read rather than one that
       raises before the question can be asked. */
    int sav = g->nil_ok;
    if (sp_streq(name, "nil?") || sp_streq(name, "inspect") || sp_streq(name, "to_s"))
      g->nil_ok = 1;
    cr_emit_expr(g, recv, b);
    g->nil_ok = sav;
    buf_printf(b, ".%s", mname);
  }
  else buf_puts(b, mname);
  buf_puts(b, "(");
  /* Ruby's output helpers all accept nil (p prints it, puts/print skip it), and
     Crystal's do too, so a possibly-missing read must not raise before the print. */
  int sav2 = g->nil_ok;
  if (sp_streq(name, "p") || sp_streq(name, "puts") || sp_streq(name, "print") ||
      sp_streq(name, "pp"))
    g->nil_ok = 1;
  for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
  g->nil_ok = sav2;
  buf_puts(b, ")");

  if (block >= 0) cr_emit_block(b, g, block, 0, recv, name);
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
    case NK_ConstantWriteNode: {
      /* FOO = value: Crystal constants have the same top-level / class scope. */
      const char *name = nt_str(g->nt, id, "name");
      int val = nt_ref(g->nt, id, "value");
      cr_indent(b, indent);
      buf_printf(b, "%s = ", name ? name : "");
      cr_emit_expr(g, val, b);
      buf_puts(b, "\n");
      return;
    }
    case NK_MultiWriteNode: {
      /* a, b = 1, 2 : Crystal destructures the same way. Only simple local
         targets are supported; a nested/ivar target is refused. */
      int tn = 0; const int *targets = nt_arr(g->nt, id, "lefts", &tn);
      int val = nt_ref(g->nt, id, "value");
      int ok = (tn > 0);
      for (int i = 0; i < tn && ok; i++)
        if (nt_kind(g->nt, targets[i]) != NK_LocalVariableTargetNode) ok = 0;
      if (!ok) { cr_mark_unsupported(g, id, "a multi-assign with a non-local target"); return; }
      cr_indent(b, indent);
      for (int i = 0; i < tn; i++) {
        const char *nm = nt_str(g->nt, targets[i], "name");
        if (i) buf_puts(b, ", ");
        buf_puts(b, cr_local(nm));
        nameset_add(&g->locals, nm);
      }
      buf_puts(b, " = ");
      cr_emit_expr(g, val, b);
      buf_puts(b, "\n");
      return;
    }
    case NK_LocalVariableWriteNode: {
      const char *name = nt_str(g->nt, id, "name");
      int val = nt_ref(g->nt, id, "value");
      /* A bare `Hash.new` bound to a new local: Crystal cannot infer K/V for an
         argument-less Hash constructor, so pin them from the first `h[k] = v`
         store and emit an explicitly parameterized local. */
      if (!nameset_has(&g->locals, name) && name &&
          nt_kind(g->nt, val) == NK_CallNode) {
        const char *cn = nt_str(g->nt, val, "name");
        int recv = nt_ref(g->nt, val, "receiver");
        int argc0 = 0; call_args(g->nt, val, &argc0);
        if (cn && sp_streq(cn, "new") && argc0 == 0 && recv >= 0 &&
            nt_kind(g->nt, recv) == NK_ConstantReadNode) {
          const char *cn2 = nt_str(g->nt, recv, "name");
          if (cn2 && sp_streq(cn2, "Hash")) {
            TyKind kt, vt;
            Buf kb, vb; memset(&kb, 0, sizeof kb); memset(&vb, 0, sizeof vb);
            if (cr_find_stored_kv(g, name, &kt, &vt) &&
                cr_map_type(g, kt, &kb) && cr_map_type(g, vt, &vb)) {
              cr_indent(b, indent);
              buf_printf(b, "%s : Hash(%s, %s) = Hash(%s, %s).new",
                         cr_local(name), kb.p, vb.p, kb.p, vb.p);
              free(kb.p); free(vb.p);
              nameset_add(&g->locals, name);
              buf_puts(b, "\n");
              return;
            }
            free(kb.p); free(vb.p);
          }
        }
      }
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
              Buf el; memset(&el, 0, sizeof el);
              if (cr_array_elem_type(g, et, &el)) {
                cr_indent(b, indent);
                buf_printf(b, "%s : Array(%s) = [] of %s", cr_local(name), el.p, el.p);
                free(el.p);
                nameset_add(&g->locals, name);
                buf_puts(b, "\n");
                return;
              }
              free(el.p);
              cr_mark_unsupported(g, val, "poly array: the pushed element type has no "
                                          "Crystal 1.21 type (it has no untyped type)");
            }
          }
        }
      }
      cr_indent(b, indent);
      if (!nameset_has(&g->locals, name)) {
        /* Deliberately NO type annotation in the general case. Spinel's TyKind
           carries no nilability, so an annotation like `a : String` is a claim
           Crystal cannot honour: `a = f.gets` is `String?` there, and the
           annotation turns a correct program into a build error. Leaving it off
           lets Crystal's own inference flow nilability through, which is the same
           answer Ruby gives. Integer/float/string literals already pin their own
           type (`1_i64`, `0.5`, `"s"`), so the annotation is not what keeps
           arithmetic monomorphic.

           The one case that DOES need an annotation is a binding from a
           possibly-missing read: `v = h[k]` is `T?` in Crystal, and without a
           declared type the local is pinned to non-nil and every later `v.nil?`
           becomes a build error. Declaring `T?` states what the read actually
           produces, so nilability reaches the uses that ask about it. */
        TyKind vt = comp_ntype(g->c, val);
        Buf ann; memset(&ann, 0, sizeof ann);
        /* Declare `T?` only when a later expression asks whether this local is
           nil. Otherwise the reads are ordinary present-key reads and `T` is the
           honest type; that is what keeps `x = c.last; p x + 1.0` monomorphic. */
        int missing = cr_maybe_missing(g, val) && cr_all_reads_nil_safe(&g->nil_uses, name);
        if (missing && cr_map_type(g, vt, &ann)) {
          buf_printf(b, "%s : %s? = ", cr_local(name), ann.p);
        } else {
          buf_printf(b, "%s = ", cr_local(name));
        }
        free(ann.p);
        nameset_add(&g->locals, name);
        int savv = g->nil_ok;
        if (missing) g->nil_ok = 1;   /* the RHS read is itself a maybe-missing read */
        cr_emit_expr(g, val, b);
        g->nil_ok = savv;
        buf_puts(b, "\n");
        return;
      } else {
        buf_printf(b, "%s = ", cr_local(name));
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
        buf_printf(b, "%s %s= ", cr_local(name), op);
        cr_emit_expr(g, val, b);
      } else {
        /* no compound form in Crystal (e.g. **=): expand to a plain assignment */
        buf_printf(b, "%s = %s", cr_local(name), cr_local(name));
        if (op) buf_printf(b, " %s ", op);
        cr_emit_expr(g, val, b);
      }
      buf_puts(b, "\n");
      return;
    }
    case NK_IfNode:
      cr_emit_if(g, id, b, indent);
      return;
    case NK_CaseNode:
      cr_emit_case(g, id, b, indent);
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
      /* `begin/rescue/else/ensure`, and the implicit form a method body gets
         when it has bare `rescue`/`ensure` clauses of its own. */
      cr_emit_rescue(g, id, b, indent);
      return;
    }
    case NK_StatementsNode:
      cr_emit_stmts(g, id, b, indent);
      return;
    case NK_PostExecutionNode:
    case NK_PreExecutionNode:
      /* BEGIN/END blocks run before/after all top-level code regardless of their
         position -- no Crystal equivalent at an arbitrary site. Refuse rather than
         silently drop the body (which would change the program's output). */
      cr_mark_unsupported(g, id, "a BEGIN/END block");
      return;
    default: {
      /* A bare expression used as a statement (a call for its side effect, an
         assignment-shaped node we don't special-case, ...). Emit it as an
         expression; if it is not one, count it unsupported.

         An index read here has its value discarded, so nothing can observe the
         difference between "found" and "missing" -- except the raise itself.
         `a[-100]` on its own line answers nil in Ruby and must not abort the
         program in Crystal, so this is a nil-tolerant position. */
      cr_indent(b, indent);
      const char *snm = (nt_kind(g->nt, id) == NK_CallNode)
                        ? nt_str(g->nt, id, "name") : NULL;
      int savs = g->nil_ok;
      if (snm && sp_streq(snm, "[]") && nt_ref(g->nt, id, "receiver") >= 0 &&
          id != g->tail_value)
        g->nil_ok = 1;
      cr_emit_expr(g, id, b);
      g->nil_ok = savs;
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

  /* Read the parameter list straight from the DefNode. Required and optional
     positional params have a direct Crystal form (`a : T`, `b : T = dflt`);
     keyword, rest, post-rest params do not (Crystal's named/positional model
     differs), so their presence is still a refusal. An optional that precedes a
     required one (legal Ruby: `def f(a = 1, b)`) would be reordered by emitting
     requireds-first, so that shape is refused too. */
  int reqn = 0, opn = 0; const int *reqs = NULL, *opts = NULL;
  int params = nt_ref(g->nt, s->def_node, "parameters");
  if (params >= 0 && sp_streq(nt_type(g->nt, params), "ParametersNode")) {
    reqs = nt_arr(g->nt, params, "requireds", &reqn);
    opts = nt_arr(g->nt, params, "optionals", &opn);
    int kn = 0, pn2 = 0;
    nt_arr(g->nt, params, "keywords", &kn);
    nt_arr(g->nt, params, "posts", &pn2);
    if (kn || pn2 || nt_ref(g->nt, params, "rest") >= 0 ||
        nt_ref(g->nt, params, "keyword_rest") >= 0)
      cr_mark_unsupported(g, s->def_node, "keyword/rest parameters");
    else if (s->nrequired != reqn)
      cr_mark_unsupported(g, s->def_node, "an optional parameter before a required one");
  }

  buf_printf(b, "def %s%s(", s->is_cmethod ? "self." : "", s->name);
  int first = 1;
  for (int i = 0; i < reqn; i++) {
    const char *pn = nt_str(g->nt, reqs[i], "name");
    LocalVar *lv = scope_local(s, pn);
    TyKind pt = lv ? lv->type : TY_UNKNOWN;
    if (!first) buf_puts(b, ", "); first = 0;
    buf_printf(b, "%s", cr_local(pn));
    cr_emit_type_ann(g, pt, b);
  }
  for (int i = 0; i < opn; i++) {
    const char *pn = nt_str(g->nt, opts[i], "name");
    int dflt = nt_ref(g->nt, opts[i], "value");
    LocalVar *lv = scope_local(s, pn);
    TyKind pt = lv ? lv->type : TY_UNKNOWN;
    if (!first) buf_puts(b, ", "); first = 0;
    buf_printf(b, "%s", cr_local(pn));
    cr_emit_type_ann(g, pt, b);
    buf_puts(b, " = ");
    cr_emit_expr(g, dflt, b);
  }
  /* Block parameter: Ruby treats a captured block as fully dynamic, and Crystal
     cannot. Declaring `&b` untyped looks plausible but is a trap -- Crystal
     infers the proc type from the *declaration*, so `block.call` on a block
     passed at the call site silently evaluates to nil instead of invoking it.
     Spelling it properly needs the block's arity and parameter types, which is
     the receiver-type-aware work that has not landed yet. Refuse it, so a block
     parameter is never shipped as a silently wrong answer. */
  int blkparam = params >= 0 ? nt_ref(g->nt, params, "block") : -1;
  if (blkparam >= 0) {
    const char *bn = nt_str(g->nt, blkparam, "name");
    char why[96];
    if (!bn || !bn[0]) snprintf(why, sizeof why, "an anonymous block parameter (&)");
    else snprintf(why, sizeof why, "a block parameter (&%s)", bn);
    cr_mark_unsupported(g, blkparam, why);
  }
  buf_puts(b, ")\n");

  int saved = cr_push_scope(g);
  for (int i = 0; i < reqn; i++) nameset_add(&g->locals, nt_str(g->nt, reqs[i], "name"));
  for (int i = 0; i < opn; i++) nameset_add(&g->locals, nt_str(g->nt, opts[i], "name"));
  int saved_class = g->cur_class;
  g->cur_class = s->class_id;   /* receiverless calls resolve against this class */
  int saved_uses = g->nil_uses.n;   /* pre-pass: which locals are only read nil-safely */
  cr_scan_nil_uses(g, s->body, &g->nil_uses, 0);
  int saved_tail = g->tail_value;
  g->tail_value = cr_tail_stmt(g, s->body);
  cr_emit_stmts(g, s->body, b, 1);
  g->tail_value = saved_tail;
  g->cur_class = saved_class;
  g->nil_uses.n = saved_uses;
  cr_pop_scope(g, saved);

  buf_puts(b, "end\n");
}

/* ---- classes ---- */
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b, int is_module) {
  g->nclasses++;
  buf_printf(b, "%s %s\n", is_module ? "module" : "class", ci->name);

  /* typed ivar declarations (classes only; modules carry none). Crystal needs the
     annotation: a bare `@d` in a class body parses as a read, not a declaration,
     and is rejected as an instance variable at the top level. An ivar whose type
     the inference left poly has no annotation to write, so refuse it. */
  int all_ivars_typed = 1;
  if (!is_module)
    for (int i = 0; i < ci->nivars; i++) {
      Buf tmp; memset(&tmp, 0, sizeof tmp);
      if (!cr_map_type(g, ci->ivar_types[i], &tmp)) {
        free(tmp.p);
        all_ivars_typed = 0;
        cr_mark_unsupported(g, ci->def_node,
                            "an instance variable with a poly/unknown type: Crystal "
                            "requires a declared ivar type and has no untyped type");
        continue;
      }
      buf_printf(b, "  %s : %s\n", ci->ivars[i], tmp.p);
      free(tmp.p);
    }

  /* A Struct.new(:a, :b) has no user `initialize`: the constructor takes the
     members positionally and assigns them. Emit it as a Crystal
     `def initialize(@a : T, @b : T)` whose ivar-params assign on entry. Only when
     every member type is concrete (a poly member already refused above). Data is
     excluded (is_data): it is a value type needing member-wise ==, not this ctor. */
  if (ci->is_struct && !ci->is_data && !is_module && all_ivars_typed) {
    Buf sig; memset(&sig, 0, sizeof sig);
    for (int i = 0; i < ci->nivars; i++) {
      if (i) buf_puts(&sig, ", ");
      Buf tmp; memset(&tmp, 0, sizeof tmp);
      cr_map_type(g, ci->ivar_types[i], &tmp);   /* known to succeed */
      buf_printf(&sig, "@%s : %s", ci->ivars[i] + 1, tmp.p);  /* skip leading '@' */
      free(tmp.p);
    }
    buf_printf(b, "  def initialize(%s)\n  end\n", sig.p ? sig.p : "");
    free(sig.p);
  }

  /* methods: walk the body's DefNodes (and class-level constants) */
  int cbody = -1;
  if (ci->def_node >= 0) {
    const char *cty = nt_type(g->nt, ci->def_node);
    if (cty && (sp_streq(cty, "ClassNode") || sp_streq(cty, "ModuleNode")))
      cbody = nt_ref(g->nt, ci->def_node, "body");
  }
  if (cbody >= 0) {
    int n = 0; const int *arr = nt_arr(g->nt, cbody, "body", &n);
    for (int i = 0; i < n; i++) {
      NodeKind kk = nt_kind(g->nt, arr[i]);
      if (kk == NK_DefNode) {
        Scope *s = cr_scope_for_def(g->c, arr[i]);
        if (s) cr_emit_method(g, s, b);
      } else if (kk == NK_ClassNode || kk == NK_ModuleNode) {
        /* nested class/module: emit recursively (Crystal is lenient about the
           indentation of nested type bodies). */
        const char *nm = NULL;
        int cp = nt_ref(g->nt, arr[i], "constant_path");
        if (cp >= 0) nm = nt_str(g->nt, cp, "name");
        int ci = nm ? comp_class_index(g->c, nm) : -1;
        if (ci >= 0) cr_emit_class(g, &g->c->classes[ci], b, kk == NK_ModuleNode);
        else cr_mark_unsupported(g, arr[i], "a nested class/module");
      } else if (kk == NK_ConstantWriteNode) {
        cr_emit_stmt(g, arr[i], b, 1);   /* pure declaration; hoists safely */
      } else if (kk == NK_CallNode &&
                 (sp_streq(nt_str(g->nt, arr[i], "name"), "attr_reader") ||
                  sp_streq(nt_str(g->nt, arr[i], "name"), "attr_writer") ||
                  sp_streq(nt_str(g->nt, arr[i], "name"), "attr_accessor"))) {
        continue;   /* resolved into ci->readers/writers, emitted below */
      } else {
        /* Any other statement (a call with side effects, a local write, ...) runs
           at definition time in Ruby. The two-pass emission hoists types before
           top-level code, so it would run out of order -- refuse rather than
           silently reorder the program's output. */
        cr_mark_unsupported(g, arr[i], "a side-effecting statement in a class/module body");
      }
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

/* A top-level `Name = Struct.new(...)` / `Data.define(...)` definition. The
   analyzer models it as a user class (is_struct/is_data) whose members are the
   positional ivars; we emit it as a Crystal reference-type `class` in Pass 1 and
   suppress the verbatim write in Pass 2. Returns the class index, or -1 if the
   node is not such a definition. */
static int cr_struct_def_ci(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, node) != NK_ConstantWriteNode) return -1;
  const char *nm = nt_str(nt, node, "name");
  int val = nt_ref(nt, node, "value");
  if (!nm || !is_struct_call(c, val)) return -1;
  int ci = comp_class_index(c, nm);
  if (ci < 0) return -1;
  ClassInfo *cls = &c->classes[ci];
  /* Struct is a reference type, so a Crystal `class` matches it. Data sets BOTH
     is_struct and is_data (register_struct_members); it is a VALUE type with
     member-wise ==, which a plain class answers by identity -- silently wrong.
     So only a pure Struct (is_struct && !is_data) is emitted here; Data stays
     refused until value semantics land. */
  return (cls->is_struct && !cls->is_data) ? ci : -1;
}

/* ---- entry point ---- */
char *codegen_program_crystal(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  CrGen g; memset(&g, 0, sizeof g);
  g.c = c; g.nt = nt;
  g.first_unsup = -1;
  g.cur_class = -1;

  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "# Generated by Spinel (Crystal proof-of-concept backend)\n");
  buf_puts(&b, "# This file is machine-generated; do not edit.\n\n");

  int top_body = c->scopes[0].body;
  const int *tstmts = NULL; int tn = 0;
  if (top_body >= 0 && nt_kind(nt, top_body) == NK_StatementsNode)
    tstmts = nt_arr(nt, top_body, "body", &tn);

  /* Pass 1: declarations (classes, modules and top-level defs), so they are
     available to the executable statements that follow -- matching Ruby, where a
     def/class/module is visible throughout regardless of its position. */
  if (tstmts) {
    for (int k = 0; k < tn; k++) {
      NodeKind kk = nt_kind(nt, tstmts[k]);
      if (kk == NK_ClassNode || kk == NK_ModuleNode) {
        const char *nm = NULL;
        int cp = nt_ref(nt, tstmts[k], "constant_path");
        if (cp >= 0) nm = nt_str(nt, cp, "name");
        int ci = nm ? comp_class_index(c, nm) : -1;
        if (ci >= 0) cr_emit_class(&g, &c->classes[ci], &b, kk == NK_ModuleNode);
      } else if (kk == NK_DefNode) {
        Scope *s = cr_scope_for_def(c, tstmts[k]);
        if (s) cr_emit_method(&g, s, &b);
      } else if (kk == NK_ConstantWriteNode) {
        int sci = cr_struct_def_ci(c, tstmts[k]);
        if (sci >= 0) cr_emit_class(&g, &c->classes[sci], &b, 0);
      }
    }
  }

  /* Pass 2: executable top-level statements, in source order. */
  int saved = cr_push_scope(&g);
  cr_scan_nil_uses(&g, top_body, &g.nil_uses, 0);
  if (tstmts) {
    for (int k = 0; k < tn; k++) {
      NodeKind kk = nt_kind(nt, tstmts[k]);
      if (kk == NK_ClassNode || kk == NK_ModuleNode || kk == NK_DefNode) continue;
      if (cr_struct_def_ci(c, tstmts[k]) >= 0) continue;   /* emitted as a class in Pass 1 */
      cr_emit_stmt(&g, tstmts[k], &b, 0);
    }
  }
  cr_pop_scope(&g, saved);

  fprintf(stderr, "[spinel-crystal] classes=%d methods=%d poly_sites=%d unsupported=%d\n",
          g.nclasses, g.nmethods, g.poly_sites, g.unsupported);

  /* A construct outside the subset was seen: refuse rather than ship a binary
     whose `nil` placeholders stand in for real code. Anchored to the first
     offending node's Ruby line, matching the C backend's rejection discipline. */
  if (g.unsupported > 0) {
    char msg[256];
    snprintf(msg, sizeof msg, "%s (%d total outside the subset)", g.first_why, g.unsupported);
    unsupported_feature(c, g.first_unsup, msg);
  }

  free(g.locals.names);
  comp_free(c);
  return b.p;
}
