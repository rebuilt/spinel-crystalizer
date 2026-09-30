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
static void cr_emit_block(Buf *b, CrGen *g, int block, int indent);
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
  return n;
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
    /* Hash */
    "keys", "values", "has_key?", "key?", "each_pair", "merge", "delete",
    /* String */
    "upcase", "downcase", "capitalize", "strip", "lstrip", "rstrip", "gsub",
    "sub", "split", "chars", "to_s", "to_i", "to_f", "start_with?", "end_with?",
    "bytes", "bytesize",
    /* Numeric */
    "abs", "ceil", "floor", "round", "truncate", "div", "gcd", "lcm", "clamp",
    "odd?", "even?",
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
  return 0;
}

/* Emit a constant path (C::CONST, A::B::C) by walking the parent chain. */
static void cr_emit_const_path(CrGen *g, int id, Buf *b) {
  int parent = nt_ref(g->nt, id, "parent");
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
      if (n) {
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
    default: {
      /* An expression node outside the subset (SelfNode, GlobalVariableReadNode,
         LambdaNode, RegularExpressionNode, ...). Emit a placeholder and record it;
         the run is refused at the end so this never ships as a silent `nil`. */
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
    if (!cr_builtin_ok(name) && !isa_const && !cr_user_method_ok(g, name, rt, bare)) {
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
        buf_puts(b, nm ? nm : "");
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
                         name, kb.p, vb.p, kb.p, vb.p);
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
                buf_printf(b, "%s : Array(%s) = [] of %s", name, el.p, el.p);
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
          buf_printf(b, "%s : %s? = ", name ? name : "", ann.p);
        } else {
          buf_printf(b, "%s = ", name ? name : "");
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
    buf_printf(b, "%s", pn);
    cr_emit_type_ann(g, pt, b);
  }
  for (int i = 0; i < opn; i++) {
    const char *pn = nt_str(g->nt, opts[i], "name");
    int dflt = nt_ref(g->nt, opts[i], "value");
    LocalVar *lv = scope_local(s, pn);
    TyKind pt = lv ? lv->type : TY_UNKNOWN;
    if (!first) buf_puts(b, ", "); first = 0;
    buf_printf(b, "%s", pn);
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
  if (!is_module)
    for (int i = 0; i < ci->nivars; i++) {
      Buf tmp; memset(&tmp, 0, sizeof tmp);
      if (!cr_map_type(g, ci->ivar_types[i], &tmp)) {
        free(tmp.p);
        cr_mark_unsupported(g, ci->def_node,
                            "an instance variable with a poly/unknown type: Crystal "
                            "requires a declared ivar type and has no untyped type");
        continue;
      }
      buf_printf(b, "  %s : %s\n", ci->ivars[i], tmp.p);
      free(tmp.p);
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
