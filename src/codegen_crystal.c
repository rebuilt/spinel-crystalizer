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
#include "crystal_runtime.h"   /* CR_RUNTIME: the boxed poly-value runtime */
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
/* True when the class is a module (analyze_scope.c). */
int comp_class_is_module(Compiler *c, ClassInfo *ci);
/* True when `name` is a reopen of a builtin class (codegen_util.c): it inherits
   Crystal's own type, so it gets neither a per-class sp_box overload nor a
   generated dispatch arm. */
int is_builtin_reopen(const char *name);

/* ---- emitter state ---- */
typedef struct { char **names; int n, cap; } NameSet;

typedef struct { char **names; int *reads; int *safe; int n, cap; } NilUseTally;

typedef struct {
  Compiler *c;
  const NodeTable *nt;
  NameSet locals;     /* declared local names in the current scope (stacked) */
  NameSet poly_arrays;/* locals declared Array(SpValue) (a widened empty `[]`):
                         a push to one boxes, but a push to a concrete array --
                         even one whose analyzer kind is TY_POLY_ARRAY before the
                         object-array narrowing -- must not. Keyed by name so the
                         decision matches the container actually emitted. */
  NameSet boxed_locals;/* locals declared `x : SpValue` by the P22b boxing path.
                         A read of one is unboxed where the analyzer narrowed it
                         to a concrete type. A poly PARAMETER is not here: it is
                         not boxed (CR_POLY_BOXED is off), so it must not be
                         unboxed as if it were an SpValue. Scope-stacked with
                         `locals`. */
  int poly_sites;     /* metric: expression nodes inferred as TY_POLY/TY_UNKNOWN */
  int unsupported;    /* metric: constructs outside the PoC subset */
  int first_unsup;    /* node id of the first unsupported construct (-1 if none);
                         anchors the end-of-run refusal to a Ruby source line */
  char first_why[160];/* what that first construct was, in words for the diagnostic */
  int cur_class;      /* class_id of the method being emitted (-1 at top level);
                         lets a receiverless call resolve against the enclosing
                         class's own methods, like Ruby's implicit self */
  Scope *cur_meth;    /* the method whose body is being emitted, NULL at top level
                         and outside a def. `super` needs its name and arity to
                         check the call against the superclass chain. */
  int nclasses;       /* classes emitted */
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
  int value_ctx;        /* 1 when the expression about to be emitted has its
                           value used (an argument, an RHS, a condition); 0 when
                           it is a discarded statement. cr_emit_expr moves it into
                           cur_value and resets it to 1 for the node's own
                           children, which are always value positions. */
  int cur_value;        /* the value_ctx of the node currently being emitted,
                           read by cr_emit_call to tell a used call (a
                           self-returning iterator, a setter) from a discarded
                           one. */
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

/* Same as cr_mark_unsupported, for a reason that has a name in it. Building the
   message here keeps the call sites free of the `char msg[N]; snprintf(...)`
   dance and centralises the truncation (a long name can never overflow). */
static void cr_mark_unsupportedf(CrGen *g, int id, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
static void cr_mark_unsupportedf(CrGen *g, int id, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  cr_mark_unsupported(g, id, msg);
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
/* Save the current depth so a nested scope (method/block) can be popped back.
   Both the declared-local and the boxed-local sets are stacked together. */
typedef struct { int locals_n, boxed_n; } ScopeMark;
static ScopeMark cr_push_scope(CrGen *g) {
  ScopeMark m = { g->locals.n, g->boxed_locals.n };
  return m;
}
static void cr_pop_scope(CrGen *g, ScopeMark m) {
  g->locals.n = m.locals_n;
  g->boxed_locals.n = m.boxed_n;
}

/* ---- staged poly-runtime seam ----

   The boxed-value runtime (src/crystal_runtime.h: SpValue, sp_box, the scalar
   operators, SpObject#sp_send) is emitted into every program. The emitter side
   that USES it is staged behind two independently-flippable switches, because
   the container half and the method-boundary half have different prerequisites:

     CR_POLY_CONTAINERS (P21) -- widened containers become
       Array(SpValue)/Hash(..., SpValue); their literals and writes box, and a
       block iterating one unboxes the element to the block parameter's type.
       This does not need a callee body to cooperate, so it can land first.

     CR_POLY_BOXED (P22) -- widened locals/ivars/parameters become SpValue, a
       concrete argument is boxed into a widened parameter, a call on a widened
       receiver routes through sp_send, and a widened predicate is guarded with
       truthy?. This cannot land before the callee-body half (dispatch +
       unboxing inside a widened method), because boxing an argument to a
       widened parameter without it turns working translations into build
       errors. The corpus gate holds every phase to zero PASS regressions.

   The code for both is kept in place so each phase is a one-line flip (plus
   that phase's own work), not a re-derivation. */
#define CR_POLY_CONTAINERS 1
#define CR_POLY_BOXED 0
#define CR_POLY_LOCALS 0
#define CR_POLY_DISPATCH 0
/* A widened (poly) instance variable is the boxed runtime value. Unlike a local
   -- where Crystal's own union already resolves the methods (P22b) -- an ivar
   must be DECLARED, and a poly one has no Crystal spelling, so it is currently a
   hard refusal. Boxing it behind SpValue unblocks that shape; staged off until
   measured. */
#define CR_POLY_IVARS 1
/* Boundary boxing: a COMPARISON between two method parameters the analyzer left
   a raw UNION (`def less(a, b); a <= b; end` called with Int, String, Bool, nil,
   so the params are `Int64 | String | ...`) boxes both sides to SpValue, routing
   the operator through the runtime's comparator instead of asking Crystal to
   resolve `Int64#<=(String)`. Unlike CR_POLY_LOCALS this boxes no LOCAL slot: the
   value keeps its native union everywhere else, and only a comparison is touched
   (it answers Bool, so the box stays inside the expression). This is the
   "boundary" the P22b evidence pointed at. */
#define CR_POLY_BOUNDARY 1

/* ---- forward declarations ---- */
static void cr_emit_expr(CrGen *g, int id, Buf *b);
static TyKind cr_ivar_type(CrGen *g, const char *name);
static int cr_user_method_ok(CrGen *g, const char *name, TyKind rt, int bare);
/* True when some visible call site of a scope omits positional param `pi`
   (defined below, near the class-body scans). */
static int cr_param_omitted_anywhere(CrGen *g, Scope *s, int pi);
static void cr_emit_stmts(CrGen *g, int body_id, Buf *b, int indent);
static void cr_emit_stmt(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_call(CrGen *g, int id, Buf *b);
/* The widest `yield` arity in a user method's body, or 0 when it has none. A
   block passed to such a method must declare exactly that many parameters --
   unlike the builtin iterators, which `ty_block_yield` describes. The arity is
   the largest argument count of any YieldNode in the method's scope. */
static int cr_scope_yield_arity(CrGen *g, Scope *s) {
  if (!s || !s->yields || s->body < 0) return 0;
  int maxn = 0;
  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_YieldNode) continue;
    if (comp_scope_of(g->c, id) != s) continue;
    int argc = 0; call_args(g->nt, id, &argc);
    if (argc > maxn) maxn = argc;
  }
  return maxn;
}

/* True when a user method's `yield` calls disagree on arity (a `yield a, b`
   beside a `yield a`). Ruby nil-fills the missing block parameters; Crystal
   infers one block signature and rejects a later `yield` of a different arity,
   so such a method is refused rather than emitted as a build error. */
static int cr_scope_yield_arity_uniform(CrGen *g, Scope *s) {
  if (!s || !s->yields) return 1;
  int seen = -1;
  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_YieldNode) continue;
    if (comp_scope_of(g->c, id) != s) continue;
    int argc = 0; call_args(g->nt, id, &argc);
    if (seen < 0) seen = argc;
    else if (seen != argc) return 0;
  }
  return 1;
}

static void cr_emit_block(Buf *b, CrGen *g, int block, int indent, int recv,
                          const char *meth);
static int cr_has_unverified_poly_call(CrGen *g);
static void cr_emit_if(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_case(CrGen *g, int id, Buf *b, int indent);
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b, int is_module);
static void cr_emit_method(CrGen *g, Scope *s, Buf *b);
static int cr_single_expr(const NodeTable *nt, int stmts_id);
static void cr_scan_nil_uses(CrGen *g, int id, NilUseTally *out, int safe);
static void cr_emit_recv(CrGen *g, int recv, Buf *b);

/* A widened (poly) value: the boxed runtime type, never `untyped`. */
static int cr_is_poly(TyKind t) { return t == TY_POLY; }

/* ---- type mapping: TyKind -> Crystal type name ----
   Writes the Crystal type to b. Returns 1 when the result is a CONCRETE type
   (usable as an annotation), 0 when there is no Crystal spelling.

   TY_POLY maps to SpValue: the boxed runtime value a widened slot takes. A
   poly container maps to a container OF SpValue, which is a real Crystal type
   (Crystal 1.21 removed `untyped`, so `Array(untyped)` was never a spelling --
   it only ever produced a parse error). TY_UNKNOWN stays non-concrete: it is
   "not inferred / unsupported", not a value with a boxed representation. */
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
    case TY_POLY_ARRAY:
      /* P21 keeps this non-concrete here on purpose. Mapping every poly-array
         EXPRESSION to Array(SpValue) would widen reads too, and reads are
         deferred to P22 (unboxing at the read site). The boxed container type
         is written explicitly where it is created -- the empty-literal pin --
         and written into where it is mutated (<< / push / []=). */
      buf_puts(b, "Array(untyped)"); return 0;
    case TY_STR_INT_HASH:   buf_puts(b, "Hash(String, Int64)"); return 1;
    case TY_STR_STR_HASH:   buf_puts(b, "Hash(String, String)"); return 1;
    case TY_INT_INT_HASH:   buf_puts(b, "Hash(Int64, Int64)"); return 1;
    case TY_INT_STR_HASH:   buf_puts(b, "Hash(Int64, String)"); return 1;
    /* A symbol/string-keyed hash with poly values is the common `{a: 1}` /
       `{"x" => 1}` shape: the C backend boxes those values, but Crystal can
       type them concretely from the literal, and treating them as
       Hash(K, SpValue) over-widens every ordinary hash. Left non-concrete: the
       literal emits its own element types, and a use that needs the poly form
       is handled where it is read. */
    case TY_STR_POLY_HASH:
    case TY_SYM_POLY_HASH:
      break;
    /* A fully heterogeneous hash is the boxed Hash(SpValue, SpValue); as with
       the poly array above, the type is written where the container is created
       or mutated, not for every expression of the kind. */
    case TY_POLY_POLY_HASH:
      buf_puts(b, "Hash(untyped, untyped)"); return 0;
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
  /* Crystal rejects `_` as a method-parameter name (only block params may be
     `_`), so rename a lone underscore consistently at every binding site. */
  if (sp_streq(n, "_")) return "_cr__";
  if (!cr_is_reserved(n)) return n;
  static char bufs[4][96];
  static int turn;
  char *out = bufs[turn++ & 3];
  snprintf(out, 96, "_cr_%s", n);
  return out;
}

/* The ELEMENT Crystal type of an array kind (for empty array literals).
   Returns 1 when the result is a CONCRETE Crystal type, 0 when there is no
   Crystal spelling. A poly element has NO spelling here: P21 widens only a
   heterogeneous SCALAR array to Array(SpValue), and that pin is written by the
   local-write path itself (it knows the pushes are scalars). Reaching this
   function with a poly/container element means a homogeneous container-of-
   containers or an unresolvable element, which P21 refuses rather than emit an
   Array(SpValue) whose pushes have no sp_box overload to answer them. */
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
    case TY_POLY:
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

/* A scalar element kind that boxes into SpValue's Raw union with no wrapper:
   Int/Float/String/Symbol/Bool/Nil. These are exactly the members of Raw that
   a widened array may hold in P21. Anything else -- a user object, Struct,
   Range, Array, Hash, BigInt -- would need SpBox/SpObject (P22), so a widened
   array refuses it rather than emit a push with no sp_box overload. */
static int cr_is_scalar_elem(TyKind t) {
  return t == TY_INT || t == TY_FLOAT || t == TY_STRING || t == TY_STRBUF ||
         t == TY_SYMBOL || t == TY_BOOL || t == TY_NIL;
}

/* An element kind that has an `sp_box` overload in the emitted runtime, so a
   heterogeneous array holding it can be widened to Array(SpValue) and the
   element boxed on the way in (P22). A scalar rides SpValue::Raw directly; a
   user object hits its per-class overload (P22a); a stdlib container hits the
   Array/Hash/Range overloads; a poly value is either already an SpValue
   (identity) or a union of Raw members. Anything else -- BigInt, Regexp, Proc,
   a bare unknown -- has no overload, so the array keeps the honest P21 refusal
   rather than emitting a push that cannot resolve. */
static int cr_boxable_elem(TyKind t) {
  if (cr_is_scalar_elem(t)) return 1;
  if (t == TY_POLY) return 1;
  if (ty_is_object(t) || ty_is_obj_array(t)) return 1;
  switch (t) {
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY:
    case TY_INT_ARRAY_ARRAY:
    case TY_FLOAT_ARRAY_ARRAY:
    case TY_STR_INT_HASH:
    case TY_STR_STR_HASH:
    case TY_INT_INT_HASH:
    case TY_INT_STR_HASH:
    case TY_SYM_POLY_HASH:
    case TY_STR_POLY_HASH:
    case TY_POLY_POLY_HASH:
    case TY_RANGE:
    case TY_FLOAT_RANGE:
    case TY_STR_RANGE:
      return 1;
    default:
      return 0;
  }
}

/* For an empty array literal whose element type the inference left poly (the
   common `a = []` then `a << x` inside a block), scan the writes to that
   variable and decide how to pin it. Scans the whole table -- fine for the PoC
   corpus, where names are unambiguous.

   Every pushed argument counts, not just the first: `push`/`unshift` can take
   several, so `a.push(1, "x")` is seen as heterogeneous instead of being pinned
   from `1` alone and then failing to build on the `"x"`.

   Three outcomes:
   * homogeneous (every pushed element the same kind) -> *elem is that kind; the
     caller pins `Array(<elem>)` as before. A single object class pins to its
     name; a container element has no Crystal spelling and the caller refuses it.
   * heterogeneous, every element boxable -> *widen=1; the caller pins
     `Array(SpValue)` and boxes each push on the way in. A scalar already lives
     in SpValue's Raw union (P21); a user object or a stdlib container is carried
     by SpBox through the per-class / Array/Hash/Range `sp_box` overloads (P22).
   * heterogeneous with an element that has no `sp_box` overload -> *refuse_id is
     that push's node (the first one, in source order) and *refuse_kind its
     element kind; the caller refuses there rather than emit a push that cannot
     resolve. BigInt is the common case: it has no Crystal spelling in the boxed
     runtime.

   Index writes (`a[i] = v`) are handled conservatively. Ruby extends a short
   array on assignment where Crystal's `[]=` raises, so a write is safe only when
   its value already matches the pinned element type; a disagreeing write is
   refused at the write (the baseline emitted `Array(<T>)` it could not store
   into -- a build failure). A widened array cannot box an index write without
   proving the index in bounds, so any `[]=` beside a widening push is refused
   too. A lone `a[0] = v` with no push is left to the empty-literal refusal: it
   has no element type to pin from. */
static void cr_find_pushed_elem(CrGen *g, const char *varname,
                                TyKind *elem, int *widen,
                                int *refuse_id, TyKind *refuse_kind,
                                const char **refuse_why) {
  TyKind first = TY_UNKNOWN;
  int heterogeneous = 0, all_scalar = 1, npush = 0;
  int all_boxable = 1;
  int unboxable_id = -1; TyKind unboxable_kind = TY_UNKNOWN;

  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(g->nt, id, "name");
    if (!nm || !(sp_streq(nm, "<<") || sp_streq(nm, "push") || sp_streq(nm, "unshift"))) continue;
    int recv = nt_ref(g->nt, id, "receiver");
    if (recv < 0 || nt_kind(g->nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(g->nt, recv, "name");
    if (!rn || !sp_streq(rn, varname)) continue;
    int argc = 0; const int *argv = call_args(g->nt, id, &argc);
    for (int a = 0; a < argc; a++) {
      TyKind et = comp_ntype(g->c, argv[a]);
      npush++;
      if (!cr_is_scalar_elem(et)) all_scalar = 0;
      if (!cr_boxable_elem(et)) {
        all_boxable = 0;
        if (unboxable_id < 0) { unboxable_id = id; unboxable_kind = et; }
      }
      if (first == TY_UNKNOWN) first = et;
      else if (et != first) heterogeneous = 1;
    }
  }

  /* Index writes to the same local: remember whether one exists and whether its
     value kind disagrees with the pushed element. */
  int has_aset = 0, aset_id = -1, aset_disagrees = 0; TyKind aset_kind = TY_UNKNOWN;
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
    TyKind vt = comp_ntype(g->c, argv[1]);
    if (!has_aset) { has_aset = 1; aset_id = id; aset_kind = vt; }
    if (first != TY_UNKNOWN && vt != first) aset_disagrees = 1;
  }

  *elem = first;
  *widen = 0;
  *refuse_id = -1;
  *refuse_kind = TY_UNKNOWN;
  *refuse_why = NULL;
  if (!CR_POLY_CONTAINERS || npush == 0) return;   /* pre-P21, or no push to pin from */

  if (heterogeneous) {
    if (!all_scalar && !all_boxable) {
      *refuse_id = unboxable_id; *refuse_kind = unboxable_kind;
      *refuse_why = "a heterogeneous array whose element has no boxed representation";
      return;
    }
    if (!has_aset) { *widen = 1; return; }   /* heterogeneous (boxable) -> widen */
    *refuse_id = aset_id; *refuse_kind = aset_kind;
    *refuse_why = "an index write to an array widened by heterogeneous pushes";
    return;
  }
  if (has_aset && aset_disagrees) {
    *refuse_id = aset_id; *refuse_kind = aset_kind;
    *refuse_why = "an index write whose value type disagrees with the array's element type";
  }
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

/* The inferred type of local `name` in the scope that owns node `id`, or
   TY_UNKNOWN. A local's widened type lives on its LocalVar, not on the read or
   write node, so a poly binding is only visible through the analyzer's scope. */
static TyKind cr_local_type(CrGen *g, int id, const char *name) {
  Scope *sc = comp_scope_of(g->c, id);
  LocalVar *lv = name ? scope_local(sc, name) : NULL;
  return lv ? lv->type : TY_UNKNOWN;
}

/* True when a receiver node emits an SpValue: a read of a poly local, which the
   local-slot half of P22 boxes. A poly COMPOUND (`if`/`case`) emits a raw union,
   not an SpValue, so `cr_is_poly(comp_ntype)` alone cannot decide whether
   boxed dispatch applies -- using it there called `sp_send`/`sp_nil?` on a
   union (`undefined method 'sp_send' for Array(String)`). */
static int cr_recv_boxed(CrGen *g, int recv) {
  if (recv < 0 || !CR_POLY_LOCALS) return 0;
  if (nt_kind(g->nt, recv) != NK_LocalVariableReadNode) return 0;
  const char *rn = nt_str(g->nt, recv, "name");
  return rn && cr_is_poly(cr_local_type(g, recv, rn));
}

/* True when `id` emits an SpValue already, so a boundary box must NOT wrap it
   again. That is a boxed local read (CR_POLY_LOCALS) or a boxed ivar read whose
   declared type is poly and is therefore declared SpValue (CR_POLY_IVARS). */
static int cr_node_is_spvalue(CrGen *g, int id) {
  if (id < 0) return 0;
  if (cr_recv_boxed(g, id)) return 1;
  if (CR_POLY_IVARS && nt_kind(g->nt, id) == NK_InstanceVariableReadNode) {
    const char *n = nt_str(g->nt, id, "name");
    return n && cr_is_poly(cr_ivar_type(g, n));
  }
  return 0;
}

/* The declared type of ivar `name` (leading @ included) on the class being
   emitted, or TY_UNKNOWN. */
static TyKind cr_ivar_type(CrGen *g, const char *name) {
  if (g->cur_class < 0 || g->cur_class >= g->c->nclasses) return TY_UNKNOWN;
  ClassInfo *ci = &g->c->classes[g->cur_class];
  for (int i = 0; i < ci->nivars; i++)
    if (ci->ivars[i] && name && sp_streq(ci->ivars[i], name)) return ci->ivar_types[i];
  return TY_UNKNOWN;
}

/* Emit `id` as an SpValue, always through `sp_box`. The overloads make this
   correct for every case: a value that is ALREADY an SpValue (a boxed local
   read, a `sp_send` result) hits the identity overload, a scalar or a Crystal
   union of Raw members hits the `Raw -> SpValue` overload, and a user object
   hits its per-class `sp_box` overload.

   It must NOT skip the call for a poly-typed node: a poly-typed *compound*
   expression (`x = if c then 1 else "s" end`) emits a raw `Int64 | String`
   union -- its branches are not boxed -- so assuming "poly means already
   SpValue" left the union assigned to an `SpValue` slot, a Crystal error. Only
   a leaf that is genuinely an SpValue is idempotent here, and the overload
   handles that case. */
static void cr_emit_boxed(CrGen *g, int id, Buf *b) {
  buf_puts(b, "sp_box(");
  cr_emit_expr(g, id, b);
  buf_puts(b, ")");
}

/* The runtime unbox function for a concrete scalar kind, or NULL when the kind
   has no boxed form (a union, a user object, a container). A read of a boxed
   local that the analyzer narrowed to one of these is unboxed through it. */
static const char *cr_unbox_fn(TyKind t) {
  switch (t) {
    case TY_INT:    return "sp_unbox_i";
    case TY_FLOAT:  return "sp_unbox_f";
    case TY_STRING:
    case TY_STRBUF: return "sp_unbox_s";
    case TY_SYMBOL: return "sp_unbox_sym";
    case TY_BOOL:   return "sp_unbox_b";
    default:        return NULL;
  }
}

/* Emit a predicate in a boolean context. Crystal requires a Bool where Ruby
   accepts any value (only nil and false are falsy), so a widened predicate
   asks its SpValue for truthiness. Used for if/unless/while/until only -- `a ||
   b` is NOT a condition: Ruby's `||` answers the operand VALUE, so rewriting it
   to a boolean would change the result. */
static void cr_emit_cond(CrGen *g, int id, Buf *b) {
  /* Only a boxed SpValue needs the runtime truthiness: `if x` on a boxed poly
     local. A poly-typed `&&`/comparison emits a `Bool | Nil` union, and Crystal
     accepts that directly AS A CONDITION while narrowing its operands; boxing it
     (`sp_box((m && m[1]...)).truthy?`) both mis-called truthy? on a Bool union
     and lost the nil-narrowing of `m` in the then-branch. */
  if (cr_recv_boxed(g, id)) {
    cr_emit_recv(g, id, b);
    buf_puts(b, ".truthy?");
    return;
  }
  cr_emit_expr(g, id, b);
}

/* The scope of the user method a call resolves to, or NULL. Mirrors
   cr_user_method_ok's resolution (top-level/bare, or the receiver's class). */
static Scope *cr_callee_scope(CrGen *g, const char *name, TyKind rt, int bare) {
  for (int s = 1; s < g->c->nscopes; s++) {
    Scope *sc = &g->c->scopes[s];
    if (!sc->name || !sp_streq(sc->name, name)) continue;
    if (bare && (sc->class_id == -1 || sc->class_id == g->cur_class)) return sc;
    if (!bare && ty_is_object(rt) && sc->class_id == ty_object_class(rt)) return sc;
  }
  return NULL;
}

/* True when the callee's parameter `idx` has a widened (poly) type, so a
   concrete argument at that position must be boxed. */
static int cr_param_poly(Scope *callee, int idx) {
  if (!callee || idx < 0 || idx >= callee->nparams) return 0;
  const char *pn = callee->pnames ? callee->pnames[idx] : NULL;
  LocalVar *lv = pn ? scope_local(callee, pn) : NULL;
  return lv && cr_is_poly(lv->type);
}

/* True when `id` emits as a value that a Crystal operator will try to resolve
   through a raw UNION: a read of a method PARAMETER the analyzer left TY_POLY.
   Such a parameter unifies across call sites (`def less(a, b); a <= b; end`
   called with Int, String, Bool, nil) and emits as one Crystal union; the
   operator then fails when a member lacks the overload (`Int64#<=(String)`).
   Boxing the parameter routes the operator through the runtime comparator.

   Deliberately NARROW: only a parameter read. A poly LOCAL keeps its native
   union, which resolves operators for the members that share them -- boxing a
   local regressed 21 programs in the P22b attempt. A parameter is the boundary
   the call sites write into, so it is the one place boxing is both necessary and
   confined. Any node already emitted as an SpValue (a boxed ivar) is excluded by
   the caller. */
static int cr_is_union_param(CrGen *g, int id) {
  if (id < 0) return 0;
  if (nt_kind(g->nt, id) != NK_LocalVariableReadNode) return 0;
  const char *n = nt_str(g->nt, id, "name");
  if (!n) return 0;
  Scope *sc = comp_scope_of(g->c, id);
  LocalVar *lv = scope_local(sc, n);
  if (!lv || !lv->is_param) return 0;
  return cr_is_poly(lv->type);
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
    /* Crystal interpolates `#{...}` inside a double-quoted string; a Ruby
       string that holds a literal `#{` (from `'\#{x}'` or an escaped
       interpolation) must have the `#` escaped or the Crystal literal is
       unterminated. */
    if (*p == '#' && p[1] == '{') { buf_puts(b, "\\#"); continue; }
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

/* Emit a Crystal string literal for a Ruby charset argument (the argument of
   tr/delete/count/squeeze). With `expand` set, an `X-Y` run becomes the explicit
   character set: Crystal's delete/count/squeeze parse ranges, but its tr does
   not, so tr needs them spelled out. A backslash escape and every other
   character are copied verbatim, so `\-` stays an escaped hyphen. Only printable
   ASCII endpoints are expanded; anything else is left as written. */
static void cr_emit_charset_literal(Buf *b, const char *s, int expand) {
  buf_puts(b, "\"");
  for (const char *p = s ? s : ""; *p; ) {
    if (*p == '\\' && p[1]) {
      char esc[3] = { '\\', p[1], 0 };
      cr_emit_escaped(b, esc);
      p += 2; continue;
    }
    if (expand && p[1] == '-' && p[2] != 0) {
      unsigned char a = (unsigned char)p[0], z = (unsigned char)p[2];
      if (a >= 32 && a <= 126 && z >= 32 && z <= 126 && a <= z) {
        for (int c = a; c <= z; c++) { char ch[2] = { (char)c, 0 }; cr_emit_escaped(b, ch); }
        p += 3; continue;
      }
    }
    char ch[2] = { *p, 0 };
    cr_emit_escaped(b, ch);
    p++;
  }
  buf_puts(b, "\"");
}

/* Copy a regex pattern into a /.../ literal, escaping only the delimiter. The
   unescaped source resolves \/ and %r{} slashes to a bare /, which would close
   the Crystal literal early; re-escape it. Backslashes are left as-is -- they
   are regex syntax in both Ruby and Crystal, not string escapes. */
static void cr_emit_regex_pat(Buf *b, const char *s) {
  int in_class = 0;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    /* Ruby's \h / \H are the hex-digit class [0-9a-fA-F]; PCRE2 reads \h as
       horizontal whitespace and \H as its negation, so translate. Inside a
       character class the members splice in; outside they form a class. */
    if (*p == '\\' && (p[1] == 'h' || p[1] == 'H')) {
      if (in_class) buf_puts(b, p[1] == 'H' ? "^0-9a-fA-F" : "0-9a-fA-F");
      else          buf_puts(b, p[1] == 'H' ? "[^0-9a-fA-F]" : "[0-9a-fA-F]");
      p++;
      continue;
    }
    if (*p == '\\' && p[1]) { buf_putn(b, (const char *)p, 2); p++; continue; }
    if (*p == '[') in_class = 1;
    else if (*p == ']') in_class = 0;
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

/* True for a call node that emits as an infix or prefix operator rather than a
   `recv.method` form. Crystal binds `.` (and `[`) tighter than these, so such a
   node must be parenthesized wherever it is used as a receiver or as an operand
   (`(a + b).to_s`, `a * (b + c)`, `(-x).abs`). */
static int cr_needs_parens(CrGen *g, int id) {
  if (id < 0 || nt_kind(g->nt, id) != NK_CallNode) return 0;
  const char *n = nt_str(g->nt, id, "name");
  if (!n) return 0;
  return cr_is_binop(n) || sp_streq(n, "-@") || sp_streq(n, "+@") || sp_streq(n, "!");
}

/* A call that carries a `do ... end` block ends with `end`; where such a call
   sits in operand or collection-element position, the `end` would swallow the
   following operator, comma or bracket (`arr.map do ... end >= 0.0`,
   `[f do ... end, g]`). Parenthesize it there. This is deliberately NOT folded
   into cr_needs_parens: a receiver/`[i]` position is left alone, because
   parenthesizing a block call used as the receiver of an index (`arr.map do ...
   end[0]`) is the one shape that would newly BUILD a program whose answer then
   diverges (Float#round's sign of zero, test_issue_3235) -- the honesty rule
   prefers it stay a BUILDFAIL over a silently wrong DIFF. */
static int cr_node_is_blockcall(CrGen *g, int id) {
  return id >= 0 && nt_kind(g->nt, id) == NK_CallNode &&
         nt_ref(g->nt, id, "block") >= 0;
}

/* Emit a subexpression in operand or element position: wrap the operator forms
   cr_needs_parens already knew about, plus a block-bearing call. */
static void cr_emit_operand(CrGen *g, int id, Buf *b) {
  int paren = cr_needs_parens(g, id) || cr_node_is_blockcall(g, id);
  if (paren) buf_puts(b, "(");
  cr_emit_expr(g, id, b);
  if (paren) buf_puts(b, ")");
}
/* Emit a subexpression that sits in receiver or operand position, adding the
   parentheses the operator form needs there. */
static void cr_emit_recv(CrGen *g, int recv, Buf *b) {
  int paren = cr_needs_parens(g, recv);
  if (paren) buf_puts(b, "(");
  cr_emit_expr(g, recv, b);
  if (paren) buf_puts(b, ")");
}

/* Does a Ruby format string use a named reference ("%<name>spec" or "%{name}")?
   `%%` is an escaped percent and is skipped, so a literal "%%<x>" is not one.
   Only the presence matters: the rewrite that follows renames the hash keys for
   every named reference at once, and leaves a purely positional format alone. */
static int cr_str_has_named_format(const char *s) {
  if (!s) return 0;
  for (const char *p = s; *p; p++) {
    if (*p != '%') continue;
    if (p[1] == '%') { p++; continue; }
    if (p[1] == '<' || p[1] == '{') return 1;
  }
  return 0;
}

/* Does a sub/gsub replacement string use a Ruby backreference that Crystal's
   replacement parser answers differently? `\&` is the whole match, `` \` `` the
   pre-match and `\'` the post-match in Ruby but all stay literal in Crystal;
   `\+` is the last matched group, likewise unexpanded; and `\k<name>` expands to
   the empty string in Ruby when the group did not participate while Crystal
   raises IndexError. `\\` is a literal backslash in both and is skipped, so
   "\\\\&" is not a backreference. */
static int cr_str_has_ruby_only_replacement(const char *s) {
  if (!s) return 0;
  for (const char *p = s; *p; p++) {
    if (*p != '\\') continue;
    char c = p[1];
    if (c == '\\') { p++; continue; }
    if (c == '&' || c == '`' || c == '\'' || c == '+') return 1;
    if (c == 'k' && p[2] == '<') return 1;
  }
  return 0;
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
    /* Ruby exception classes Crystal's hierarchy has no name for. A `rescue`
       clause maps StandardError -> Exception and ZeroDivisionError ->
       DivisionByZeroError, but a bare reference (constructing one, is_a?, a
       class value) has no Crystal form, so only the VALUE position is refused
       here. */
    { "StandardError", NULL, "Crystal's base is Exception; StandardError has no Crystal name outside a rescue clause" },
    { "TypeError", NULL, "Crystal has no TypeError exception class" },
    { "NameError", NULL, "Crystal has no NameError exception class" },
    { "ZeroDivisionError", NULL, "Crystal spells it DivisionByZeroError (only in a rescue clause)" },
    { "LocalJumpError", NULL, "Crystal has no LocalJumpError exception class" },
    { "SignalException", NULL, "Crystal has no SignalException exception class" },
    { "UncaughtThrowError", NULL, "Crystal has no UncaughtThrowError exception class" },
    { "ScriptError", NULL, "Crystal has no ScriptError exception class" },
    { "LoadError", NULL, "Crystal has no LoadError exception class" },
    { "SyntaxError", NULL, "Crystal has no SyntaxError exception class" },
    { "SystemExit", NULL, "Crystal has no SystemExit exception class" },
    { "SystemStackError", NULL, "Crystal has no SystemStackError exception class" },
    { "ThreadError", NULL, "Crystal has no ThreadError exception class" },
    { "EOFError", NULL, "Crystal has no EOFError exception class" },
    { "IOError", NULL, "Crystal has no IOError exception class" },
    { "RangeError", NULL, "Crystal has no RangeError exception class" },
    { "RegexpError", NULL, "Crystal has no RegexpError exception class" },
    { "SecurityError", NULL, "Crystal has no SecurityError exception class" },
    { "EncodingError", NULL, "Crystal has no EncodingError exception class" },
    { "StopIteration", NULL, "Crystal has no StopIteration exception class" },
    { "FiberError", NULL, "Crystal has no FiberError exception class" },
    { "ClosedQueueError", NULL, "Crystal has no ClosedQueueError exception class" },
    /* The class exists; the member does not. */
    { "Float", "INFINITY", "Crystal has no Float::INFINITY" },
    { "Float", "NAN", "Crystal has no Float::NAN" },
    { "Float", "MAX", "Crystal spells it Float64::MAX; Ruby's Float::MAX is the same value but the constant has no Ruby-spelled Crystal form here" },
    { "Float", "MIN", "Crystal has no Float::MIN" },
    { "Float", "EPSILON", "Crystal spells it Float64::EPSILON" },
    /* No Crystal counterpart at all. */
    { "Marshal", NULL, "Crystal has no Marshal; its serialization format differs" },
    { "MatchData", NULL, "Crystal spells the match type Regex::MatchData, not MatchData" },
    /* Class-level constants with no Crystal name. */
    { "Process", "CLOCK_MONOTONIC", "Crystal's monotonic clock is Time.monotonic, not a Process constant" },
    { "Process", "CLOCK_REALTIME", "Crystal's wall clock is Time.utc, not a Process constant" },
    { "File", "RDONLY", "Crystal opens with a String mode or File::Flags, which has no Ruby-spelled RDONLY" },
    { "File", "WRONLY", "Crystal opens with a String mode or File::Flags" },
    { "File", "RDWR", "Crystal opens with a String mode or File::Flags" },
    { "File", "CREAT", "Crystal opens with a String mode or File::Flags" },
    { "File", "APPEND", "Crystal opens with a String mode or File::Flags" },
    { "File", "EXCL", "Crystal opens with a String mode or File::Flags" },
    { "File", "TRUNC", "Crystal opens with a String mode or File::Flags" },
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
      /* Refuse the CLASS, not the clause, and name it: the message should point
         at the exception the programmer actually has to deal with. */
      char why[300];
      snprintf(why, sizeof why,
               "the exception class '%s' has no Crystal equivalent: "
               CR_NO_CRYSTAL_EXC, nm);
      cr_mark_unsupported(g, exc[i], why);
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

/* True when ANY user scope or accessor in the program defines a method named `n`.
   Needed where the receiver's own class is not statically known: `cr_user_method_ok`
   matches on the receiver's class, so it cannot answer for a poly value, yet a remap
   that assumes a builtin stdlib spelling would shadow a user method carrying the
   Ruby name. */
static int cr_any_method_named(CrGen *g, const char *n) {
  if (!n) return 0;
  for (int s = 1; s < g->c->nscopes; s++) {
    Scope *sc = &g->c->scopes[s];
    if (sc->name && sp_streq(sc->name, n)) return 1;
  }
  for (int c = 0; c < g->c->nclasses; c++) {
    ClassInfo *cl = &g->c->classes[c];
    for (int i = 0; i < cl->nreaders; i++)
      if (sp_streq(cl->readers[i], n)) return 1;
    for (int i = 0; i < cl->nwriters; i++)
      if (cl->writers[i] && sp_streq(cl->writers[i], n)) return 1;
  }
  return 0;
}

static const char *cr_translate_name(CrGen *g, int id, int recv, const char *n) {
  if (sp_streq(n, "include?")) {
    TyKind rt = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;
    /* Hash#include? checks keys; Crystal's Hash answers that as has_key?. */
    if (ty_is_hash(rt)) return "has_key?";
    /* A concrete user object that defines its own `include?` keeps the Ruby name --
       there is nothing to remap to, and Crystal would reject `includes?`. */
    if (cr_user_method_ok(g, n, rt, 0)) return n;
    /* Crystal's Array/Set spell it `includes?`, so a CONCRETE receiver always
       remaps, even in a program that defines its own `include?` somewhere else
       (`@names.include?(x)` on an `Array(String)` is unaffected).
       A receiver whose class is NOT statically known is the hard case: the value can
       be a user class defining `include?` OR an Array, and the two arms spell the
       method DIFFERENTLY, so no single name resolves the union
       (`(Tags | Array(String))#include?` is `undefined method 'include?' for
       Array(String)`, and `includes?` is undefined for Tags). Renaming cannot fix
       that; it needs boxed dispatch. So it is refused HERE, by name, rather than
       emitted as a build error. */
    if (cr_any_method_named(g, n)) {
      Buf probe; memset(&probe, 0, sizeof probe);
      int concrete = cr_map_type(g, rt, &probe);
      free(probe.p);
      if (!concrete)
        cr_mark_unsupported(g, id, "a call to a user-defined method on a receiver "
                                   "whose class is not statically known (the union's "
                                   "arms spell the method differently)");
    }
    return "includes?";
  }
  if (sp_streq(n, "length")) {
    /* A concrete user object that defines its own `length` keeps the Ruby name --
       the rename is for Array/Hash/String, and Crystal has no `length` on a user
       class (it would ship `undefined method 'size'` as a build error). */
    TyKind rt = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;
    if (cr_user_method_ok(g, n, rt, recv < 0)) return n;
    /* A receiver whose class is not statically known can be a user object that
       defines its own `length`; the union's arms then spell the method two
       different ways and no single rename resolves it, so refuse by name rather
       than ship the build error. A widened ARRAY keeps the rename: Ruby's
       Array#length is the container's, whatever the elements are. */
    if (recv >= 0 && (rt == TY_POLY || rt == TY_UNKNOWN) && cr_any_method_named(g, n))
      cr_mark_unsupported(g, id, "a call to `length` on a receiver whose class is "
                                 "not statically known (the union's arms spell the "
                                 "method differently)");
    return "size";
  }
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
  /* Enumerable#inject is Enumerable#reduce; the shared desugarer rewrites the
     symbol/block forms in place but keeps the `inject` name. */
  if (sp_streq(n, "inject")) return "reduce";
  /* Ruby's Enumerable#entries is an alias of to_a (a fresh Array); Crystal
     spells it to_a on Range/Array/Hash/Set. A user method is emitted verbatim
     (def names are not translated), so it must be kept as-is. */
  if (sp_streq(n, "entries")) {
    TyKind rt = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;
    if (cr_user_method_ok(g, n, rt, recv < 0)) return n;
    return "to_a";
  }
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
  /* Ruby's Array#slice / Hash#slice / String#slice / Symbol#slice have no
     Crystal method at all -- Crystal's Array and Hash have `[]` but not `slice`
     (verified against Crystal 1.21). A user class defining `slice` is matched
     before this table (the caller passes NULL for a user method), so a
     remaining `slice` is a guaranteed "undefined method". Refusing by name also
     covers a union receiver that the container-family checks below cannot name. */
  if (sp_streq(name, "slice"))
    return "Crystal has no slice; index with [] or a range instead";
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
    { "Time",    "mktime", "Crystal spells mktime Time.local" },
    { "Kernel",  "gets",  "Crystal reads stdin with gets, not Kernel.gets" },
    { "Object",  "new",   "Crystal's Object is not instantiable with new" },
    { "Struct",  "new",   "only a top-level S = Struct.new(..) is translated; a Struct factory call has no Crystal form" },
    { "Fiber",   "yield", "Crystal's Fiber.yield takes no value (Ruby's passes one out)" },
    { "Signal",  "trap",  "Crystal has no Signal.trap; a handler is not part of this subset" },
    { "Signal",  "signame", "Crystal has no Signal.signame" },
    { "Random",  "urandom", "Crystal's secure randomness is Random::Secure" },
    { "ENV",     "key?",  "Crystal's ENV has no key?; use has_key?" },
    { "ENV",     "update", "Crystal's ENV has no update" },
  };
  if (mod) {
    /* Class-object reflection -- `Dog.ancestors`, `Dog.method_defined?`,
       `M.const_get`. Ruby's Class/Module query surface has no Crystal form even
       when the class is the program's own, so this is checked BEFORE the
       user-class shadow test (which only protects the *instance* methods a
       program defines). */
    static const char *const refl[] = {
      "ancestors", "superclass", "subclasses", "method_defined?",
      "const_defined?", "const_get", "const_set", "instance_method",
      "singleton_class?", "instance_methods", "public_instance_methods",
      "private_instance_methods", "protected_instance_methods",
      "public_method_defined?", "private_method_defined?", "protected_method_defined?",
      "const_source_location", "define_method", "module_eval", "class_eval",
      "constants", "respond_to?", "include?", "prepend", "include_modules", NULL,
    };
    for (int i = 0; refl[i]; i++)
      if (sp_streq(name, refl[i]))
        return "Ruby's class-object reflection has no Crystal equivalent";
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
    /* Time.utc / Time.gm take at most 6 arguments in Crystal; Ruby's 7th is
       microseconds, for which there is no slot. */
    if (sp_streq(mod, "Time") && (sp_streq(name, "utc") || sp_streq(name, "gm"))) {
      int ac = 0; call_args(g->nt, id, &ac);
      if (ac == 7)
        return "Crystal's Time.utc takes at most 6 arguments; Ruby's 7th "
               "(microseconds) has no Crystal slot";
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
    /* Ruby's green Thread / Fiber / Time carry methods Crystal's types do not.
       The name allowlist cannot see a receiver-typed method like `t.value`, so
       without these the call emits Crystal that fails to compile ("undefined
       method") instead of a line-anchored refusal. The class-receiver shapes
       (Thread.value, Time.mktime) are handled by the `mod` table above. */
    { TY_THREAD, "value", "Crystal's Thread has no value; join then read a shared slot" },
    { TY_THREAD, "join",  "Crystal's Thread#join takes no timeout/limit argument" },
    { TY_THREAD, "alive?", "Crystal's Thread has no alive?; test the Fiber state" },
    { TY_THREAD, "status", "Crystal's Thread has no status" },
    { TY_THREAD, "raise", "Crystal's Thread cannot be raised into" },
    { TY_TIME, "strftime", "Crystal formats Time with to_s(format), whose directive set differs" },
    { TY_TIME, "asctime", "Crystal's Time has no asctime; format it" },
    { TY_TIME, "ctime",   "Crystal's Time has no ctime; format it" },
    { TY_TIME, "xmlschema", "Crystal's Time has no xmlschema" },
    { TY_TIME, "deconstruct_keys", "Ruby's pattern-matching protocol has no Crystal form" },
    { TY_TIME, "between?", "Crystal's Time has no between?" },
    { TY_TIME, "eql?",     "Crystal compares Time with ==" },
    { TY_TIME, "tv_sec",   "Crystal spells epoch seconds to_unix" },
    { TY_TIME, "localtime", "Ruby's localtime mutates the receiver; Crystal's Time is immutable" },
    { TY_TIME, "gmtime",   "Ruby's gmtime mutates the receiver; Crystal's Time is immutable" },
    { TY_TIME, "getlocal", "Ruby's getlocal(offset) has no Crystal form (to_local takes none)" },
    { TY_FIBER, "transfer", "Crystal's Fiber cannot transfer to another fiber" },
    { TY_FIBER, "[]=",      "Crystal's Fiber has no fiber-local []=" },
    { TY_REGEX, "match?",  "Crystal spells it on the String (str.matches?(re))" },
    { TY_REGEX, "eql?",    "Crystal compares Regex with ==" },
    { TY_REGEX, "names",   "Crystal exposes named captures lazily, not as a name list" },
    { TY_REGEX, "freeze",  "Crystal has no freeze" },
    { TY_STRING, "[]=",    "Crystal's String is immutable and has no []=; a Ruby index-assign needs a mutable buffer" },
    { TY_STRING, "sum",    "Crystal's String has no sum (Ruby's byte checksum)" },
    { TY_FLOAT,  "succ",   "Crystal's Float has no succ" },
    { TY_SYMBOL, "size",   "Crystal's Symbol has no size" },
    { TY_TIME,   "min",    "Crystal spells the minute of the hour `minute`" },
    { TY_TIME,   "to_a",   "Crystal's Time has no to_a" },
    { TY_TIME,   "asctime", "Crystal's Time has no asctime; format it" },
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
    if (v[i].type == rt && sp_streq(v[i].meth, name)) return v[i].why;
  /* Container methods Crystal lacks (keyed on the container family, so every
     element type is covered without listing each TyKind). */
  if (ty_is_array(rt)) {
    if (sp_streq(name, "slice!")) return "Crystal's Array has no slice!; it is not mutated in place";
    if (sp_streq(name, "append")) return "Crystal spells Array#append `push`";
    if (sp_streq(name, "prepend")) return "Crystal spells Array#prepend `unshift`";
  }
  if (ty_is_hash(rt)) {
    if (sp_streq(name, "freeze"))   return "Crystal has no freeze; a Hash is not frozen at runtime";
    if (sp_streq(name, "frozen?"))  return "Crystal has no frozen?";
    if (sp_streq(name, "sort"))     return "Hash has no sort; collect the pairs and sort those";
    if (sp_streq(name, "sort_by"))  return "Hash has no sort_by; collect the pairs and sort those";
    if (sp_streq(name, "default=")) return "Crystal's Hash has no default= (use fetch with a block)";
    if (sp_streq(name, "replace"))  return "Crystal's Hash has no replace";
    if (sp_streq(name, "fetch_values")) return "Crystal's Hash has no fetch_values";
    if (sp_streq(name, "flatten"))  return "Crystal's Hash has no flatten";
    if (sp_streq(name, "value?"))   return "Crystal's Hash has no value?";
    if (sp_streq(name, "key?"))     return "Crystal's Hash has no key?; use has_key?";
  }
  if (rt == TY_MUTEX && sp_streq(name, "locked?"))
    return "Crystal's Mutex has no locked?";
  /* Methods Crystal's Object simply has not, whatever the receiver. A concrete
     receiver is already refused by the name allowlist, but a poly/untyped one
     (a Time, a Thread, a container element) reaches dynamic dispatch and then
     fails to compile, so catch them by name here too. */
  {
    static const char *const gone[] = {
      "respond_to?", "eql?", "equal?", "then", "yield_self",
      "freeze", "frozen?", "send", "public_send", NULL,
    };
    for (int i = 0; gone[i]; i++)
      if (sp_streq(name, gone[i])) return "Crystal's Object has no such method";
  }
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

/* The superclass index that cr_emit_class actually carries into the emitted
   header, or -1 when the parent is not an emitted user class. Every
   inheritance-aware lookup must use this SAME relation: the emitted Crystal
   class only inherits from a parent it can name, so a method, reader or ivar
   further up a chain that was never carried is not reachable in the emitted
   program either. */
static int cr_name_reopened(CrGen *g, const char *name);  /* defined below */
static int cr_emitted_parent(CrGen *g, ClassInfo *ci) {
  if (!ci || ci->parent < 0 || ci->parent >= g->c->nclasses) return -1;
  ClassInfo *p = &g->c->classes[ci->parent];
  if (p->is_native_class || !p->name || p->def_node < 0) return -1;
  /* The header spells the parent by its bare name, which only resolves where
     the child is emitted when the parent sits in the same nesting or at the
     top level. A parent nested in a module cannot be named from a top-level
     child: `class Child < M::Sub` would emit `class Child < Sub` and Crystal
     fails with "undefined constant Sub". Decline the carry there -- the chain
     lookups must decline with it, or they accept calls the emitted program
     cannot resolve. */
  if (p->enclosing_class != -1 && p->enclosing_class != ci->enclosing_class)
    return -1;
  /* A class written at more than one site is emitted from ONE body: the other
     site's defs and nested classes are dropped, so a child that inherited from
     it could call methods that were never emitted. Decline the carry. */
  if (cr_name_reopened(g, p->name)) return -1;
  return ci->parent;
}

/* True when `cls` is `target` or an ancestor of it through emitted superclasses
   -- exactly the classes whose defs the emitted `target` inherits. P22n made
   the relation real in the class header; the call path must consult it too, or
   a method defined only on the superclass is refused as "not in the Crystal
   subset" even though Crystal resolves it fine (P22o). */
static int cr_class_in_chain(CrGen *g, int cls, int target) {
  if (cls < 0 || target < 0) return 0;
  for (int c = target; c >= 0 && c < g->c->nclasses;
       c = cr_emitted_parent(g, &g->c->classes[c]))
    if (c == cls) return 1;
  return 0;
}

/* True when `name` is a method the program itself defines and the call can reach:
   either a top-level def, a def on the receiver's user class or one of its
   emitted ancestors, or (for a receiverless call) a def on the enclosing class
   or its ancestors -- Ruby's implicit self. */
static int cr_user_method_ok(CrGen *g, const char *name, TyKind rt, int bare) {
  for (int s = 1; s < g->c->nscopes; s++) {
    Scope *sc = &g->c->scopes[s];
    if (!sc->name || !sp_streq(sc->name, name)) continue;
    if (bare && (sc->class_id == -1 || cr_class_in_chain(g, sc->class_id, g->cur_class))) return 1;
    if (!bare && ty_is_object(rt) && cr_class_in_chain(g, sc->class_id, ty_object_class(rt))) return 1;
  }
  /* attr_accessor/reader/writer and the synthesized Struct methods are recorded in
     the class's readers[]/writers[] (not def scopes), so a call to them must be
     accepted here or it is refused as "not in subset". The target class is the
     receiver's class for a qualified call, or the enclosing class (implicit self)
     for a bare call inside a method body -- Crystal resolves a bare `x` with no
     local of that name to self.x, matching Ruby. */
  int tci = -1;
  if (!bare && ty_is_object(rt)) tci = ty_object_class(rt);
  else if (bare && g->cur_class >= 0) tci = g->cur_class;
  if (tci >= 0 && tci < g->c->nclasses)
    for (int c = tci; c >= 0 && c < g->c->nclasses;
         c = cr_emitted_parent(g, &g->c->classes[c])) {   /* the chain Crystal inherits (P22o) */
      ClassInfo *cl = &g->c->classes[c];
      for (int i = 0; i < cl->nreaders; i++)
        if (sp_streq(cl->readers[i], name)) return 1;
      size_t nl = strlen(name);
      if (nl >= 2 && name[nl - 1] == '=') {   /* setter: "x=" -> base "x" */
        for (int i = 0; i < cl->nwriters; i++) {
          const char *w = cl->writers[i];
          if (strlen(w) == nl - 1 && strncmp(w, name, nl - 1) == 0) return 1;
        }
      }
      /* to_h is a synthesized Struct method not in the name allowlist (which would
         wrongly let it through on non-struct receivers). Accept it only on a struct
         instance so the call reaches the generated method. (to_a/values are already
         allowed; ==/[]/[]= bypass as operators.) */
      if (cl->is_struct && !cl->is_data && sp_streq(name, "to_h")) return 1;
    }
  return 0;
}

/* Emit a constant path (C::CONST, A::B::C) by walking the parent chain. */
static void cr_emit_const_path(CrGen *g, int id, Buf *b) {
  int parent = nt_ref(g->nt, id, "parent");
  /* Crystal resolves `::` only when the receiver is itself a constant path; a
     runtime value (`k::CODE` where k is a local, an ivar or a call) has no
     Crystal form -- `k::CODE` is a parse error, not a lookup. Refuse it by name
     rather than ship the unparseable read. */
  if (parent >= 0) {
    NodeKind pk = nt_kind(g->nt, parent);
    if (pk != NK_ConstantReadNode && pk != NK_ConstantPathNode &&
        pk != NK_SelfNode) {
      cr_mark_unsupported(g, id,
          "a `::` constant read on a runtime value (Crystal resolves `::` only "
          "on a constant path, not on a local, ivar or expression)");
      buf_puts(b, "nil");
      return;
    }
  }
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
        cr_mark_unsupportedf(g, id, "the constant '%s::%s' (%s)", owner, leaf, why);
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

/* Parameter shape of a def as cr_emit_method would write it: requireds,
   optionals, and `odd` when the shape is outside the emitted subset (rest,
   keyword, posts). A method that reached emission has !odd; the check matters
   for the SUPERCLASS side of a `super`, whose def may carry a shape the
   subclass call cannot answer positionally. */
static void cr_def_shape(CrGen *g, Scope *s, int *reqn, int *opn, int *odd) {
  *reqn = *opn = 0; *odd = 0;
  if (!s || s->def_node < 0) { *odd = 1; return; }
  int params = nt_ref(g->nt, s->def_node, "parameters");
  if (params < 0) return;
  if (!sp_streq(nt_type(g->nt, params), "ParametersNode")) { *odd = 1; return; }
  nt_arr(g->nt, params, "requireds", reqn);
  nt_arr(g->nt, params, "optionals", opn);
  int kn = 0, pn2 = 0;
  nt_arr(g->nt, params, "keywords", &kn);
  nt_arr(g->nt, params, "posts", &pn2);
  if (kn || pn2 || nt_ref(g->nt, params, "rest") >= 0 ||
      nt_ref(g->nt, params, "keyword_rest") >= 0)
    *odd = 1;
}

/* Does Crystal's *implicit* parent (Reference) define `name` with this arity, so
   that a `super` in a class with no explicit superclass resolves? Probed against
   Crystal 1.21: `initialize`/`to_s`/`inspect`/`hash` take no argument, `==`/`!=`/
   `===` take one. Ruby's Object defines the same names, so the call means the
   same thing in both languages. A name outside this set (`def hi; super; end`)
   is a Ruby NoMethodError and a Crystal compile error -- refused. */
static int cr_super_builtin_parent(const char *name, int fwd) {
  if (!name) return 0;
  if (sp_streq(name, "initialize") || sp_streq(name, "to_s") ||
      sp_streq(name, "inspect") || sp_streq(name, "hash")) return fwd == 0;
  if (sp_streq(name, "==") || sp_streq(name, "!=") ||
      sp_streq(name, "===")) return fwd == 1;
  return 0;
}

/* Check that a `super` in the method being emitted has a Crystal equivalent.
   P22n made the superclass real in the emitted header, so Crystal's own
   `super` resolves the call and the translation is verbatim -- but only under
   these guards, each of which is a case where Ruby raises at RUN time and
   Crystal fails to COMPILE (the honesty discipline: name it, do not trade a
   refusal for a build error):
   - the enclosing method belongs to a class with an emitted superclass;
   - the superclass chain defines the SAME name (Ruby: NoMethodError; probe:
     "undefined method 'm' for Object");
   - the arity lines up (Ruby: ArgumentError; Crystal: compile error);
   - the block lines up: a bare `super` forwards the call's block, so a
     yielding superclass method reached from a non-yielding method has no
     block to forward (probe: "B#m is not expected to be invoked with a
     block" / "expects a block").
   `argc` is the explicit argument count, or -1 for the bare forwarding form,
   which sends the enclosing method's own params. */
static int cr_super_ok(CrGen *g, int id, int argc) {
  if (!g->cur_meth || g->cur_class < 0 || g->cur_class >= g->c->nclasses) {
    cr_mark_unsupported(g, id, "a `super` outside a class method body");
    return 0;
  }
  Scope *me = g->cur_meth;
  ClassInfo *self = &g->c->classes[g->cur_class];
  if (nt_ref(g->nt, id, "block") >= 0) {
    cr_mark_unsupported(g, id, "a `super` with a block argument");
    return 0;
  }
  int my_req = 0, my_opn = 0, my_odd = 0;
  cr_def_shape(g, me, &my_req, &my_opn, &my_odd);   /* emitted, so !my_odd */
  int fwd = argc < 0 ? my_req + my_opn : argc;
  int par = cr_emitted_parent(g, self);
  if (par < 0) {
    /* A class with no explicit superclass still inherits Reference's builtins,
       so a `super` to one of them resolves in Crystal. Probed: `def to_s;
       super; end` and `def ==(o); super; end` build and match Ruby, while
       `def initialize(x); super; end` is "wrong number of arguments for
       Reference#initialize (given 1, expected 0)" -- so the arity must line up
       with Reference's, not the subclass's. */
    if (cr_super_builtin_parent(me->name, fwd)) return 1;
    /* Name WHY the parent is not carried, so the refusal says what to fix. */
    ClassInfo *p = (self->parent >= 0 && self->parent < g->c->nclasses)
                     ? &g->c->classes[self->parent] : NULL;
    const char *why = !p ? "has no superclass"
                  : p->is_native_class ? "is a builtin class"
                  : !p->name || p->def_node < 0 ? "is not an emitted class"
                  : (p->enclosing_class != -1 &&
                     p->enclosing_class != self->enclosing_class)
                    ? "is nested where the header cannot name it"
                    : "is reopened at more than one site";
    cr_mark_unsupportedf(g, id,
        "a `super` in a class whose superclass %s%s%s",
        p && p->name ? "`" : "", p && p->name ? p->name : why,
        p && p->name ? "`" : "");
    return 0;
  }
  for (int c = par; c >= 0; c = cr_emitted_parent(g, &g->c->classes[c])) {
    ClassInfo *pc = &g->c->classes[c];
    Scope *ps = NULL;
    for (int s = 1; s < g->c->nscopes; s++) {
      Scope *sc = &g->c->scopes[s];
      if (!sc->name || !sp_streq(sc->name, me->name)) continue;
      if (sc->class_id != c || sc->is_cmethod != me->is_cmethod) continue;
      if (sc->cs_synth) continue;   /* compiler-synthesized: no emitted Crystal def */
      ps = sc; break;
    }
    if (!ps) {
      /* attr reader/writer on the ancestor is emitted as a real def too:
         a 0-arg getter / 1-arg setter for the arity check. */
      int is_get = 0, is_set = 0;
      for (int i = 0; i < pc->nreaders; i++)
        if (sp_streq(pc->readers[i], me->name)) is_get = 1;
      size_t nl = strlen(me->name);
      if (nl >= 2 && me->name[nl - 1] == '=') {
        for (int i = 0; i < pc->nwriters; i++) {
          const char *w = pc->writers[i];
          if (strlen(w) == nl - 1 && strncmp(w, me->name, nl - 1) == 0) is_set = 1;
        }
      }
      if (is_get && fwd == 0) return 1;
      if (is_set && fwd == 1) return 1;
      continue;   /* this ancestor does not define it; Ruby keeps walking up */
    }
    int pr = 0, po = 0, odd = 0;
    cr_def_shape(g, ps, &pr, &po, &odd);
    if (odd) {
      cr_mark_unsupported(g, id, "a `super` to a method with rest/keyword parameters");
      return 0;
    }
    if (fwd < pr || fwd > pr + po) {
      cr_mark_unsupported(g, id, "a `super` whose argument count does not match the superclass method");
      return 0;
    }
    if (argc >= 0 && ps->yields) {
      /* `super(args)` does NOT forward the block in Ruby either -- the parent's
         yield would LocalJumpError -- and Crystal rejects the blockless call at
         compile time. Refuse rather than emit either failure. */
      cr_mark_unsupported(g, id, "a `super` with explicit arguments to a yielding superclass method");
      return 0;
    }
    if (ps->yields && !me->yields) {
      /* bare super forwards the CALL's block, but the emitter only gives this
         method a block signature when its own body yields; a non-yielding
         method has no guaranteed block to forward. */
      cr_mark_unsupported(g, id, "a `super` to a yielding method from a method that takes no block");
      return 0;
    }
    if (argc < 0 && me->yields && !ps->yields) {
      cr_mark_unsupported(g, id, "a bare `super` forwarding a block to a method that takes none");
      return 0;
    }
    return 1;
  }
  cr_mark_unsupportedf(g, id, "a `super` call: no superclass of '%s' defines '%s'",
                       g->c->classes[g->cur_class].name ? g->c->classes[g->cur_class].name : "?",
                       me->name ? me->name : "?");
  return 0;
}

/* True when `s` (a Ruby symbol's text, no leading colon) can be written as a
   bare Crystal symbol `:name`. Crystal allows a bare symbol only when it is an
   identifier -- an optional trailing `?`/`!`/`=` over letters, digits and `_`,
   not starting with a digit -- plus the operator forms (`:+`, `:[]`, `:<=>`).
   Anything else (a space, a leading digit, most punctuation) has to be quoted:
   `:"a b"`, `:"1a"`. Emitting one unquoted is a parse error, not a refusal. */
static int cr_sym_is_bare(const char *s) {
  if (!s || !s[0]) return 0;
  size_t n = strlen(s);
  /* An operator symbol is always bare in Crystal. */
  static const char *ops[] = { "+", "-", "*", "/", "%", "**", "==", "===", "!=",
    "<", ">", "<=", ">=", "<=>", "<<", ">>", "&", "|", "^", "~", "!", "=~",
    "!~", "[]", "[]=", "+@", "-@" };
  for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
    if (sp_streq(s, ops[i])) return 1;
  size_t end = n;
  if (s[n - 1] == '?' || s[n - 1] == '!' || s[n - 1] == '=') end = n - 1;
  if (end == 0) return 0;
  for (size_t i = 0; i < end; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x80) continue;              /* a UTF-8 continuation/lead byte: a letter */
    if (c == '_') continue;
    if (i == 0) { if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0; }
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9'))) return 0;
  }
  return 1;
}

/* ---- expressions ---- */
/* Emit the parts of an InterpolatedStringNode, WITHOUT the surrounding quotes.
   A part may itself be an InterpolatedStringNode: adjacent literal
   concatenation (`"a#{x}" "b#{y}"`) nests them, and Ruby joins all the
   fragments into one string, so a nested part is flattened rather than dropped
   (an unhandled part kind used to emit an empty string for the whole node). */
static void cr_emit_interp_parts(CrGen *g, int id, Buf *b) {
  int n = 0; const int *parts = nt_arr(g->nt, id, "parts", &n);
  for (int i = 0; i < n; i++) {
    NodeKind pk = nt_kind(g->nt, parts[i]);
    if (pk == NK_StringNode) {
      const char *s = nt_str(g->nt, parts[i], "content");
      cr_emit_escaped(b, s ? s : "");
    } else if (pk == NK_InterpolatedStringNode) {
      cr_emit_interp_parts(g, parts[i], b);
    } else if (pk == NK_EmbeddedStatementsNode) {
      int stmts = nt_ref(g->nt, parts[i], "statements");
      buf_puts(b, "#{");
      int e = cr_single_expr(g->nt, stmts);
      if (e >= 0) {
        cr_emit_expr(g, e, b);
      } else {
        /* Several statements (run for side effects; the value is the last).
           Crystal's interpolation holds one expression, so `begin ... end`
           wraps the sequence (`"#{a; b}"` is not valid Crystal). */
        buf_puts(b, "begin\n");
        cr_emit_stmts(g, stmts, b, 0);
        buf_puts(b, "end");
      }
      buf_puts(b, "}");
    } else {
      /* A part kind we do not model (an embedded-variable form, say): record it
         and refuse the run rather than silently drop the fragment and emit a
         wrong string. */
      const char *ty = nt_type(g->nt, parts[i]);
      cr_mark_unsupported(g, parts[i], ty ? ty : "interpolated-string part");
    }
  }
}

/* Emit the `k => v, ...` entries of a hash literal for elements [from, to),
   comma-separated and without the surrounding braces. Shared by the plain
   literal and by the plain runs inside a double-splat merge chain. */
static void cr_emit_hash_entries(CrGen *g, const int *els, int from, int to, Buf *b) {
  for (int i = from; i < to; i++) {
    int key = nt_ref(g->nt, els[i], "key");
    int val = nt_ref(g->nt, els[i], "value");
    if (i > from) buf_puts(b, ", ");
    cr_emit_expr(g, key, b);
    buf_puts(b, " => ");
    cr_emit_operand(g, val, b);
  }
}

/* Emit a hash literal that has at least one double-splat element
   (`{ **h, x: v }`). Crystal has no hash-literal double-splat (`**` inside `{}`
   is a parse error), so each run of plain assocs becomes one `{ k => v }`
   literal and every splat merges in, preserving Ruby's left-to-right override
   order. A lone `{ **h }` copies in Ruby, so it gets `.dup`. */
static void cr_emit_hash_splat_chain(CrGen *g, const int *els, int n, Buf *b) {
  int started = 0, nsplat = 0;
  for (int i = 0; i < n; ) {
    if (nt_kind(g->nt, els[i]) == NK_AssocSplatNode) {
      int v = nt_ref(g->nt, els[i], "value");
      if (started) buf_puts(b, ".merge(");
      cr_emit_operand(g, v, b);
      if (started) buf_puts(b, ")");
      started = 1;
      nsplat++;
      i++;
    } else {
      int end = i;
      while (end < n && nt_kind(g->nt, els[end]) != NK_AssocSplatNode) end++;
      if (started) buf_puts(b, ".merge(");
      buf_puts(b, "{ ");
      cr_emit_hash_entries(g, els, i, end, b);
      buf_puts(b, " }");
      if (started) buf_puts(b, ")");
      started = 1;
      i = end;
    }
  }
  if (n == 1 && nsplat == 1) buf_puts(b, ".dup");
}

static void cr_emit_expr(CrGen *g, int id, Buf *b) {
  if (id < 0) { buf_puts(b, "nil"); return; }
  /* Record whether THIS node's value is used (read by a call handler: a
     self-returning iterator, a setter), then reset the ambient flag so the
     node's own children -- always value positions -- see 1. */
  g->cur_value = g->value_ctx;
  g->value_ctx = 1;
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
      /* `:"a b"` / `:"1a"` / `:"😀"` are not valid bare symbols; emitting them
         unquoted is a Crystal parse error. Quote exactly the ones that need it,
         so every symbol that already emits stays byte-identical. */
      if (s && cr_sym_is_bare(s)) { buf_printf(b, ":%s", s); return; }
      buf_puts(b, ":\"");
      cr_emit_escaped(b, s ? s : "");
      buf_puts(b, "\"");
      return;
    }
    case NK_TrueNode:  buf_puts(b, "true");  return;
    case NK_FalseNode: buf_puts(b, "false"); return;
    case NK_NilNode:   buf_puts(b, "nil");   return;
    case NK_LocalVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");
      /* A read of a boxed local that the analyzer NARROWED to a concrete type
         (inside `if x.is_a?(String)`, say) is unboxed back to it, so concrete
         code sees the value it expects. A read the analyzer left poly stays an
         SpValue (its own methods answer). */
      if (CR_POLY_LOCALS && n && nameset_has(&g->boxed_locals, n)) {
        const char *uf = cr_unbox_fn(comp_ntype(g->c, id));
        if (uf) { buf_printf(b, "%s(%s)", uf, cr_local(n)); return; }
      }
      buf_puts(b, cr_local(n));
      return;
    }
    case NK_InstanceVariableReadNode: {
      const char *n = nt_str(g->nt, id, "name");   /* includes the leading @ */
      /* A read of a boxed ivar that the analyzer NARROWED to a concrete scalar
         is unboxed back; a read left poly stays the SpValue (its own methods
         answer). Mirrors the boxed-local read in the poly-locals path. */
      if (CR_POLY_IVARS && n && cr_is_poly(cr_ivar_type(g, n))) {
        const char *uf = cr_unbox_fn(comp_ntype(g->c, id));
        if (uf) { buf_printf(b, "%s(%s)", uf, n); return; }
      }
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
          cr_mark_unsupportedf(g, id, "the constant '%s' (%s)", n, why);
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
      /* A non-empty literal keeps its own element types (Crystal types a
         heterogeneous literal as a union), so reads work and no boxing is
         needed. Only an empty `[]` that a later heterogeneous push widens is
         pinned to Array(SpValue) -- see the local-write path. */
      buf_puts(b, "[");
      for (int i = 0; i < n; i++) { if (i) buf_puts(b, ", "); cr_emit_operand(g, els[i], b); }
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
      /* A non-empty hash literal also keeps its own key/value types (Crystal
         types it as a concrete Hash, union-valued if heterogeneous), so reads
         work and no boxing is needed. The boxed Hash(SpValue, SpValue) form is
         reserved for a later phase. A double-splat element has no Crystal
         literal form, so it is emitted through the merge-chain helper. */
      int has_splat = 0;
      for (int i = 0; i < n; i++)
        if (nt_kind(g->nt, els[i]) == NK_AssocSplatNode) { has_splat = 1; break; }
      if (has_splat) {
        cr_emit_hash_splat_chain(g, els, n, b);
        return;
      }
      buf_puts(b, "{ ");
      cr_emit_hash_entries(g, els, 0, n, b);
      buf_puts(b, " }");
      return;
    }
    case NK_InterpolatedStringNode: {
      buf_puts(b, "\"");
      cr_emit_interp_parts(g, id, b);
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
      /* Hand the flag to the call emitter ONLY for the reads that consult it --
         an index read, and `min`/`max` (whose `?` form keeps Ruby's nil-on-empty).
         Restoring it for any other call would let it reach the receiver: in
         `p(h[k] + 1)`, p's argument is the `+`, and the `+`'s receiver must not
         become a nilable read. The call emitter clears it again for its own
         receiver, so the hand-off stops at the call it is meant for. */
      const char *cnm = nt_str(g->nt, id, "name");
      int nil_read = cnm && nt_ref(g->nt, id, "receiver") >= 0 &&
                     (sp_streq(cnm, "[]") || sp_streq(cnm, "min") || sp_streq(cnm, "max"));
      if (nil_read) g->nil_ok = saved_nil_ok;
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
    case NK_YieldNode: {
      /* `yield a, b` invokes the block the method was called with. Crystal's
         `yield` is the same construct, and the signature declares `(&)` when
         the body contains one (see cr_emit_method). Arguments are emitted as a
         comma list; a splat is refused (Crystal's yield takes the same argument
         forms but a Ruby splat's static type is unknown here). */
      int argc = 0; const int *argv = call_args(g->nt, id, &argc);
      buf_puts(b, "yield");
      for (int i = 0; i < argc; i++) {
        if (nt_kind(g->nt, argv[i]) == NK_SplatNode) {
          cr_mark_unsupported(g, argv[i], "a splat argument to yield");
          continue;
        }
        buf_puts(b, i ? ", " : "(");
        cr_emit_expr(g, argv[i], b);
      }
      if (argc > 0) buf_puts(b, ")");
      return;
    }
    case NK_SuperNode: {
      /* `super` with explicit parens (possibly empty): Crystal's `super(args)`
         is the same call against the same ancestor chain P22n put in the class
         header, so the translation is verbatim once cr_super_ok vouches for it. */
      int argc = 0; const int *argv = call_args(g->nt, id, &argc);
      if (!cr_super_ok(g, id, argc)) { buf_puts(b, "nil"); return; }
      buf_puts(b, "super(");
      for (int i = 0; i < argc; i++) {
        if (i) buf_puts(b, ", ");
        if (nt_kind(g->nt, argv[i]) == NK_SplatNode) {
          cr_mark_unsupported(g, argv[i], "a splat argument to super");
          buf_puts(b, "nil");
          continue;
        }
        cr_emit_expr(g, argv[i], b);
      }
      buf_puts(b, ")");
      return;
    }
    case NK_ForwardingSuperNode: {
      /* Ruby's bare `super` forwards the enclosing method's params (and the
         call's block); Crystal's bare `super` does the same, so it drops in
         verbatim once cr_super_ok vouches for the name, arity and block. */
      if (!cr_super_ok(g, id, -1)) { buf_puts(b, "nil"); return; }
      buf_puts(b, "super");
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
  cr_emit_cond(g, pred, b);
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
      cr_emit_cond(g, cp, b);
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
/* Emit the extraction of a possibly-boxed yielded value into a block parameter
   of inferred type `pt`. `src` is the C expression holding the yielded value
   (already `SpValue` when the container is widened). A concrete `pt` unboxes
   through the runtime; a widened `pt` (or an unresolved one) is the identity.
   The types match Crystal's `sp_unbox_*` helpers. */
static void cr_emit_block_unbox(Buf *b, const char *dst, const char *src, TyKind pt) {
  if (pt == TY_INT)         buf_printf(b, "%s = sp_unbox_i(%s)\n", dst, src);
  else if (pt == TY_FLOAT)  buf_printf(b, "%s = sp_unbox_f(%s)\n", dst, src);
  else if (pt == TY_STRING || pt == TY_STRBUF) buf_printf(b, "%s = sp_unbox_s(%s)\n", dst, src);
  else if (pt == TY_SYMBOL) buf_printf(b, "%s = sp_unbox_sym(%s)\n", dst, src);
  else if (pt == TY_BOOL)   buf_printf(b, "%s = sp_unbox_b(%s)\n", dst, src);
  else                      buf_printf(b, "%s = %s\n", dst, src);
}

static void cr_emit_block(Buf *b, CrGen *g, int block, int indent, int recv,
                          const char *meth) {
  int params = nt_ref(g->nt, block, "parameters");
  int body   = nt_ref(g->nt, block, "body");

  ScopeMark saved = cr_push_scope(g);
  const char *pty = params >= 0 ? nt_type(g->nt, params) : NULL;
  int pn = 0; const int *preqs = NULL;
  if (pty && sp_streq(pty, "BlockParametersNode")) {
    int inner = nt_ref(g->nt, params, "parameters");
    if (inner >= 0) {
      preqs = nt_arr(g->nt, inner, "requireds", &pn);
      /* A block with optional (`|a, b=1|`), rest (`|*a|`) or numbered (`_1`)
         parameters has no direct Crystal form here: Crystal block parameters
         take no defaults, and a rest param's element type is unknown. Refuse so
         the shape is named rather than emitted as a wrong-arity block. */
      int on = 0, rn2 = 0;
      nt_arr(g->nt, inner, "optionals", &on);
      nt_arr(g->nt, inner, "posts", &rn2);
      int restp = nt_ref(g->nt, inner, "rest");
      if (on > 0 || rn2 > 0 || restp >= 0)
        cr_mark_unsupported(g, block, "a block with optional or rest parameters");
    }
  } else if (pty && sp_streq(pty, "NumberedParametersNode")) {
    cr_mark_unsupported(g, block, "a block with numbered parameters (_1, _2, ...)");
  } else if (pty && sp_streq(pty, "ParametersNode")) {
    preqs = nt_arr(g->nt, params, "requireds", &pn);
  }

  TyKind recv_ty = recv >= 0 ? comp_ntype(g->c, recv) : TY_UNKNOWN;

  /* A widened array (Array(SpValue)) yields an SpValue per element, so a block
     parameter with a concrete inferred type has to be unboxed. P21 widens only
     arrays -- never Hash -- and only the single-yield iterators (each/map/...);
     each_with_index's second value is a plain Int and its 2-param form falls to
     the auto-splat path below, so the unbox here is always one value. The gate
     is on the receiver being the widened array kind itself, so a future
     Hash(..., SpValue) mapping cannot accidentally enable it. ty_block_yield is
     the source of truth for the yield arity. */
  TyKind yt[4]; memset(yt, 0, sizeof yt);
  int nyield = (recv >= 0 && meth) ? ty_block_yield(recv_ty, meth, yt, 4) : 0;
  int widen_block = CR_POLY_CONTAINERS && pn == 1 && nyield == 1 &&
                    recv_ty == TY_POLY_ARRAY && yt[0] == TY_POLY;

  /* Ruby auto-splats only when the block asks for more values than the iterator
     yields. A parenthesized `|(a, b)|` was already desugared upstream into a
     single param, so seeing 2+ REQUIRED params here is the flat `|a, b|` form
     and the auto-splat is what Ruby would do. A USER method that `yield`s two
     values is the exception: Crystal's `yield a, b` already passes two block
     parameters, so a `|a, b|` block must NOT be desugared into a synthetic
     single param. */
  int user_yarity = 0;
  if (meth) {
    Scope *cs = cr_callee_scope(g, meth, recv_ty, recv < 0);
    if (cs) user_yarity = cr_scope_yield_arity(g, cs);
  }
  TyKind yields[4];
  int splat = 0;
  if (pn >= 2 && user_yarity < 2 && recv >= 0 && meth &&
      ty_block_yield(recv_ty, meth, yields, 4) == 1)
    splat = 1;
  char elem[48] = "";
  if (splat) snprintf(elem, sizeof elem, "__cr_elem%d", g->tmp_seq++);
  buf_puts(b, (splat || pn > 0) ? " do |" : " do");
  if (splat) buf_puts(b, elem);
  if (widen_block && !splat) {
    /* one synthetic param for the single yielded SpValue; unboxed in the body */
    snprintf(elem, sizeof elem, "__cr_elem%d", g->tmp_seq++);
    buf_puts(b, elem);
  } else {
    for (int i = 0; i < pn && !splat; i++) {
      const char *rn = nt_str(g->nt, preqs[i], "name");
      if (i) buf_puts(b, ", ");
      buf_puts(b, cr_local(rn));
    }
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
  } else if (widen_block) {
    /* Unbox the single yielded SpValue to the parameter's inferred type. A
       parameter the inference itself left poly is bound as the SpValue directly. */
    const char *rn = nt_str(g->nt, preqs[0], "name");
    LocalVar *lv = rn ? scope_local(comp_scope_of(g->c, block), rn) : NULL;
    TyKind pt = lv ? lv->type : TY_UNKNOWN;
    cr_indent(b, indent + 1);
    if (yt[0] == TY_POLY) cr_emit_block_unbox(b, cr_local(rn), elem, pt);
    else                  buf_printf(b, "%s = %s\n", cr_local(rn), elem);
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

/* Ruby's `arr.<stage>.with_index(off) { |x, i| }` drives a blockless stage
   enumerator with an index; the blockless form is a first-class Enumerator over
   [element, index] pairs. Crystal has no blockless Array#map/each (a blockless
   `map` is a build error: "expected to be invoked with a block") and no
   index-taking select/reject/sort_by, so the immediate array/range chain is
   rewritten to the indexed forms Crystal does have:

     map/collect   -> map_with_index(off) { |x, i| ... }
     each          -> tap { |r| r.each_with_index(off) { |x, i| ... } }   (each
                      answers the receiver; Crystal's each_with_index answers Nil)
     select/filter -> each_with_index(off).select { |x, i| ... }.map(&.[0]).to_a
     reject        -> each_with_index(off).reject { |x, i| ... }.map(&.[0]).to_a

   A blockless chain becomes each_with_index(off): a lazy Iterator of
   {element, index} Tuples. .to_a on it converts the Tuples to [element, index]
   Arrays (see cr_emit_call), matching Ruby's Enumerator.

   Only an immediate array/range source is handled here; a stored Enumerator's
   `with_index` goes to the Int32-offset cast in the receiver branch below.
   Returns 1 when handled. */
/* Crystal's `*_with_index` offsets are Int32, but an integer literal is emitted
   as Int64 (`1_i64`); the stdlib then computes `offset + i` as an Int64 and the
   yield fails to typecheck ("argument #2 of yield expected to be Int32"). Cast
   the offset back to Int32 at the call. */
static void cr_emit_wi_offset(CrGen *g, int argc, const int *argv, Buf *b) {
  if (argc >= 1) {
    buf_puts(b, "(");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, ").to_i32");
  }
}

static int cr_emit_with_index(CrGen *g, int id, Buf *b) {
  const char *name = nt_str(g->nt, id, "name");
  if (!name || !sp_streq(name, "with_index")) return 0;
  int recv = nt_ref(g->nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(g->nt, id, "block");
  int argc = 0; const int *argv = call_args(g->nt, id, &argc);
  if (nt_kind(g->nt, recv) != NK_CallNode || nt_ref(g->nt, recv, "block") >= 0) {
    /* a stored Enumerator (or any other receiver): Crystal's Iterator#with_index
       takes an Int32 offset, so an Int64 offset literal needs the cast. With no
       offset the generic path is already correct. */
    if (argc < 1) return 0;
    buf_puts(b, "(");
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".with_index((");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, ").to_i32)");
    if (block >= 0) cr_emit_block(b, g, block, 0, recv, "with_index");
    buf_puts(b, ")");
    return 1;
  }
  const char *inner = nt_str(g->nt, recv, "name");
  if (!inner) return 0;
  int is_map = sp_streq(inner, "map") || sp_streq(inner, "collect");
  int is_each = sp_streq(inner, "each");
  int is_sel = sp_streq(inner, "select") || sp_streq(inner, "filter");
  int is_rej = sp_streq(inner, "reject");
  if (!is_map && !is_each && !is_sel && !is_rej) return 0;
  int arr = nt_ref(g->nt, recv, "receiver");
  if (arr < 0) return 0;
  TyKind at = comp_ntype(g->c, arr);
  if (!ty_is_array(at) && at != TY_RANGE && at != TY_POLY_ARRAY) return 0;

  Buf recvb; memset(&recvb, 0, sizeof recvb);
  cr_emit_recv(g, arr, &recvb);
  const char *r = recvb.p ? recvb.p : "";

  if (block < 0) {
    /* first-class pair Enumerator: Crystal's each_with_index Iterator yields
       {element, index} Tuples. Ruby's yields [element, index] Arrays; a Tuple
       auto-splats in a later block (`|x, i|`) where an Array would not, so the
       pair stays a Tuple here and only the `.to_a` terminal converts it (see
       the to_a case in cr_emit_call). */
    buf_printf(b, "%s.each_with_index(", r);
    cr_emit_wi_offset(g, argc, argv, b);
    buf_puts(b, ")");
    free(recvb.p);
    return 1;
  }
  if (is_map) {
    buf_printf(b, "%s.map_with_index(", r);
    cr_emit_wi_offset(g, argc, argv, b);
    buf_puts(b, ")");
    cr_emit_block(b, g, block, 0, arr, "with_index");
  } else if (is_each) {
    buf_printf(b, "%s.tap do |__wi_recv%d|\n  __wi_recv%d.each_with_index(",
               r, g->tmp_seq, g->tmp_seq);
    g->tmp_seq++;
    cr_emit_wi_offset(g, argc, argv, b);
    buf_puts(b, ")");
    cr_emit_block(b, g, block, 1, arr, "with_index");
    buf_puts(b, "end");
  } else {
    buf_printf(b, "%s.each_with_index(", r);
    cr_emit_wi_offset(g, argc, argv, b);
    buf_puts(b, is_rej ? ").reject" : ").select");
    cr_emit_block(b, g, block, 0, arr, "with_index");
    buf_puts(b, ".map(&.[0]).to_a");
  }
  free(recvb.p);
  return 1;
}

/* True when `id` is a blockless `arr.<stage>.with_index(...)` chain, whose
   Crystal value is an Iterator of {element, index} Tuples (cr_emit_with_index). */
static int cr_is_blockless_wi(CrGen *g, int id) {
  if (id < 0 || nt_kind(g->nt, id) != NK_CallNode) return 0;
  const char *nm = nt_str(g->nt, id, "name");
  if (!nm || !sp_streq(nm, "with_index") || nt_ref(g->nt, id, "block") >= 0) return 0;
  int recv = nt_ref(g->nt, id, "receiver");
  if (recv < 0 || nt_kind(g->nt, recv) != NK_CallNode) return 0;
  if (nt_ref(g->nt, recv, "block") >= 0) return 0;
  const char *inner = nt_str(g->nt, recv, "name");
  if (!inner || !(sp_streq(inner, "map") || sp_streq(inner, "collect") ||
                  sp_streq(inner, "each") || sp_streq(inner, "select") ||
                  sp_streq(inner, "filter") || sp_streq(inner, "reject"))) return 0;
  int arr = nt_ref(g->nt, recv, "receiver");
  if (arr < 0) return 0;
  TyKind at = comp_ntype(g->c, arr);
  return ty_is_array(at) || at == TY_RANGE || at == TY_POLY_ARRAY;
}

/* P22r: a desugared Enumerable builtin call. A program that calls a method from
   builtins/enumerable.rb (flat_map, each_with_object, minmax, ...) has the
   analyzer rewrite `recv.m(args) { blk }` into a bare helper call
   `__enum_m__<id>(recv, args) { blk }` -- the receiver becomes argument 0 and
   the call carries no receiver of its own. Crystal's stdlib has almost all of
   these under the same name, so the emitter reconstructs the original call.
   Returns the Crystal method name and writes the Ruby method name into
   ruby[0..ruby_sz) (the block emitter's yield oracle `ty_block_yield` is keyed
   on Ruby names -- `drop_while`, not `skip_while`), or NULL to keep the
   refusal. Only names whose Crystal answer is verified identical are listed:
   filter_map (Crystal compact_map drops only nil, Ruby drops false too),
   grep/grep_v and each_with_index (Crystal answers nil, Ruby the receiver) are
   deliberately absent. */
static const char *cr_enum_passthrough(const char *name, TyKind recv_ty, char *ruby, size_t ruby_sz) {
  if (!name || strncmp(name, "__enum_", 7) != 0) return NULL;
  const char *p = name + 7;
  const char *sep = strrchr(p, '_');       /* the second '_' of "__<id>" */
  if (!sep || sep <= p || sep[-1] != '_') return NULL;
  for (const char *d = sep + 1; *d; d++)
    if (*d < '0' || *d > '9') return NULL;
  size_t n = (size_t)(sep - 1 - p);
  if (n == 0 || n >= ruby_sz) return NULL;
  memcpy(ruby, p, n); ruby[n] = 0;
#define CR_ENUM_IS(s) (n == strlen(s) && strncmp(p, s, n) == 0)
  /* Only the names whose Crystal stdlib form is verified identical for the
     corpus call shapes are listed. The rest keep the named refusal: measured,
     max_by/min_by reject the `(n)` argument form, Crystal's Hash has no
     filter/partition/minmax_by of the shape Ruby uses, and blockless forms and
     pair-yielding methods (min_by/find/minmax/...) answer Tuples or take a
     different arity, so passing them through traded a refusal for a build
     error or a silently different answer. `none?`/`drop_while` are
     probe-verified but not yet exercised by the corpus. `count`/`none?` with a
     PATTERN argument inherit Crystal's native `==` predicate (Ruby uses `===`),
     which is already a documented DIFF on the native path, so it is not gated
     here -- the mapping is exactly as safe as the native call it mirrors. */
  if (CR_ENUM_IS("inject"))     return "reduce";
  if (CR_ENUM_IS("drop_while")) {
    /* Crystal's Hash#skip_while answers an Array of Tuples where Ruby's
       Hash#drop_while answers an Array of [k, v] Arrays. Only a receiver whose
       element shape the method preserves (an Array or a Range) is safe; a Hash,
       or a widened/uninferred receiver that could be one at run time, keeps the
       refusal. */
    if (!ty_is_array(recv_ty) && recv_ty != TY_RANGE) return NULL;
    return "skip_while";
  }
  if (CR_ENUM_IS("find_index")) {
    /* Crystal's Array#index / Range#index answer Ruby's find_index (the first
       index where the block holds, or of an equal element). A Hash has no such
       form and a widened receiver could be one, so only an Array/Range receiver
       is mapped. */
    if (!ty_is_array(recv_ty) && recv_ty != TY_RANGE) return NULL;
    return "index";
  }
  static const char *const same[] = { "each_with_object", "count", "none?" };
  for (size_t i = 0; i < sizeof same / sizeof same[0]; i++)
    if (CR_ENUM_IS(same[i])) return same[i];
#undef CR_ENUM_IS
  return NULL;
}

/* The special Ruby call forms -- operators, Kernel raise/exit, poly dispatch,
   stdlib method rewrites, index read/write -- that cr_emit_call recognizes
   before the general method call. They are an ORDERED sequence: each case is
   tried in turn, so the extraction preserves the order. Returns 1 when a case
   emitted the call (the caller must return), 0 to fall through to the general
   path. */
/* Ruby/Crystal binary-operator translation: integer `/` -> `//`, the `%`
   named-format key rewrite, and the boxed comparison at a widened boundary.
   Returns 1 when it emitted the call. */
static int cr_emit_binop_call(CrGen *g, int id, const char *name, int recv,
                              int argc, const int *argv, int block, Buf *b) {
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
    /* Crystal's String#% is sprintf, and its named references ("%<name>spec" /
       "%{name}") index the hash with a STRING key (formatter.cr: `arg[index]`).
       Ruby's format reads the symbol-keyed hash we emit, so a plain `%` raises
       KeyError on the first name. Rename the keys back to Strings. Restricted to
       a literal format, as on the C side: a computed format hides its references,
       and a positional format needs no rewrite at all. */
    if (sp_streq(name, "%") && nt_kind(g->nt, recv) == NK_StringNode &&
        ty_is_hash(comp_ntype(g->c, argv[0])) &&
        cr_str_has_named_format(nt_str(g->nt, recv, "content"))) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, " % (");
      cr_emit_expr(g, argv[0], b);
      buf_puts(b, ".transform_keys(&.to_s))");
      return 1;
    }
    /* `x == nil` / `x != nil` is the other spelling of "does this read have a
       value", so the left operand is a nil-tolerant position too. */
    int savb = g->nil_ok;
    if ((sp_streq(name, "==") || sp_streq(name, "!=")) &&
        nt_kind(g->nt, argv[0]) == NK_NilNode)
      g->nil_ok = 1;
    /* A concrete receiver against a boxed (poly) operand: box the receiver so
       the SpValue operator runs. `Float64#-(SpValue)` does not exist, but
       `SpValue#-(Raw)` does (the operand overloads). A boxed receiver already
       dispatches through those, so it is left as-is.

       The same reasoning applies to a raw-union operand. A method like
       `def less(a, b); a <= b; end` called with mixed types has params typed
       `Int64 | String | ...`, which emits as a Crystal union; `Int64#<=(String)`
       then fails to resolve. Boxing a union operand turns the operator into
       `SpValue#<=`, whose comparator raises ArgumentError on a Ruby-illegal
       pair instead of failing to compile. Both operands are boxed so the
       SpValue/SpValue overload (not the Raw one) is chosen and the boxed
       receiver is never left comparing against a raw union. */
    int recv_box = CR_POLY_LOCALS && cr_is_poly(comp_ntype(g->c, recv));
    int arg_box  = CR_POLY_LOCALS && cr_is_poly(comp_ntype(g->c, argv[0]));
    if (CR_POLY_BOUNDARY && !cr_node_is_spvalue(g, recv) && cr_is_union_param(g, recv))
      recv_box = 1;
    if (CR_POLY_BOUNDARY && !cr_node_is_spvalue(g, argv[0]) && cr_is_union_param(g, argv[0]))
      arg_box = 1;
    /* Only a COMPARISON is boxed. A comparison answers Bool, so nothing escapes
       the operator and no result unbox is needed -- the box is fully local to
       this expression. An arithmetic operator is NOT: its SpValue result would
       escape into the surrounding code, where an Int-typed slot (`Array.new`,
       `while i < n`) or a chained call (`s.size()`) rejects it. Boxing those cost
       three corpus programs with no gain, so they are left to their native union.
       `%` has no SpValue operator at all, which is the same case from the other
       side. This is what `test_poly_equality_mixed_sites` needs: `a <= b` and
       `a == b` on a parameter the call sites widened to a mixed union. */
    int is_cmp = sp_streq(op, "==") || sp_streq(op, "!=") || sp_streq(op, "<") ||
                 sp_streq(op, ">") || sp_streq(op, "<=") || sp_streq(op, ">=");
    if (!is_cmp) { recv_box = 0; arg_box = 0; }
    /* A union/poly side is boxed; the concrete peer is taken by SpValue's operand
       overload (`SpValue#<=(Raw)`). When only the ARG is a union the receiver is
       concrete, so the receiver is boxed instead -- it must be the SpValue for the
       overload to exist at all. */
    if (arg_box && !recv_box) recv_box = 1;
    /* A boxed comparison answers Bool, so the box stays inside this expression:
       nothing about the enclosing code changes type. */
    if (recv_box) {
      buf_puts(b, "sp_box(");
      cr_emit_recv(g, recv, b);
      buf_puts(b, ")");
    } else {
      cr_emit_operand(g, recv, b);
    }
    g->nil_ok = savb;
    buf_printf(b, " %s ", op);
    if (arg_box) {
      buf_puts(b, "sp_box(");
      cr_emit_recv(g, argv[0], b);
      buf_puts(b, ")");
    } else {
      cr_emit_operand(g, argv[0], b);
    }
    return 1;
  }
  return 0;
}

/* A call on a widened (poly) receiver, and the boxed push into a widened
   array: nil?, the sp_send dynamic dispatch with its unbox, and the boxing
   push. Returns 1 when it emitted the call. */
static int cr_emit_poly_call(CrGen *g, int id, const char *name, int recv,
                     int argc, const int *argv, int block, Buf *b) {
  /* Crystal's `nil?` is a pseudo-method that cannot be redefined, so a widened
     value answers through the runtime's sp_nil? instead. */
  if ((CR_POLY_DISPATCH || cr_recv_boxed(g, recv)) && recv >= 0 && argc == 0 &&
      sp_streq(name, "nil?") && cr_is_poly(comp_ntype(g->c, recv))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".sp_nil?");
    return 1;
  }

  /* A call on a widened receiver: the run-time value's own kind decides what
     answers, so it goes through the boxed dispatch. This is the seam that lets
     `x.m` work when x is a poly slot -- an Integer, a String, an Array, a user
     object -- which before was refused. `!` and the operators have their own
     SpValue forms and are handled above. `call` is the block/proc protocol,
     answered by SpValue#call. */
  if ((CR_POLY_DISPATCH || cr_recv_boxed(g, recv)) && recv >= 0 &&
      cr_is_poly(comp_ntype(g->c, recv)) && !cr_user_method_ok(g, name, TY_POLY, 0) &&
      !sp_streq(name, "!") && !sp_streq(name, "[]") && !sp_streq(name, "[]=") &&
      !cr_is_binop(name) && !sp_streq(name, "-@") && !sp_streq(name, "+@")) {
    if (block >= 0) {
      cr_mark_unsupported(g, id, "a block passed to a call on a widened (poly) receiver");
      buf_puts(b, "nil");
      return 1;
    }
    /* The result comes back boxed; unbox it to the type this call site expects
       (the node's inferred type) so it flows into concrete code unchanged.
       A node the inference left poly keeps the SpValue. */
    TyKind nt = comp_ntype(g->c, id);
    const char *unbox = sp_streq(name, "call") ? "sp_unbox_call" : "sp_unbox";
    if (nt == TY_INT)         buf_puts(b, "sp_unbox_i(");
    else if (nt == TY_FLOAT)  buf_puts(b, "sp_unbox_f(");
    else if (nt == TY_STRING) buf_puts(b, "sp_unbox_s(");
    else if (nt == TY_BOOL)   buf_puts(b, "sp_unbox_b(");
    (void)unbox;
    if (sp_streq(name, "call")) { cr_emit_recv(g, recv, b); buf_puts(b, ".call("); }
    else { cr_emit_recv(g, recv, b); buf_printf(b, ".sp_send(\"%s\", ", name); }
    /* Crystal cannot infer an empty literal's element type, so the no-argument
       case names it (`[] of SpValue`); a non-empty literal types itself. */
    if (argc == 0) {
      buf_puts(b, "[] of SpValue");
    } else {
      buf_puts(b, "[");
      for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_boxed(g, argv[i], b); }
      buf_puts(b, "]");
    }
    buf_puts(b, ")");
    if (nt == TY_INT || nt == TY_FLOAT || nt == TY_STRING || nt == TY_BOOL) buf_puts(b, ")");
    return 1;
  }

  /* A widened array's elements are SpValue, so a push into it boxes. This is
     the `a = []` then `a << 1; a << "x"` shape, where the empty-literal pin
     made `a` an Array(SpValue); the element is boxed on the way in. Only the
     push names are handled -- an index write (`a[i] = x`) and a
     symbol/string-keyed hash write are left to the general path, because a
     receiver that reads back as TY_POLY_ARRAY / TY_SYM_POLY_HASH need not have
     been pinned to the boxed form, and boxing a concrete container's write is
     the read/write coupling that P22 (read-side unboxing) resolves. */
  if (CR_POLY_CONTAINERS && recv >= 0 &&
      (sp_streq(name, "<<") || sp_streq(name, "push") || sp_streq(name, "unshift"))) {
    /* Box only when the receiver is a local that was declared Array(SpValue) --
       the widened empty `[]` of P21. A concrete array (including one the
       analyzer calls TY_POLY_ARRAY before object-array narrowing) keeps its own
       element type and must not receive a boxed value. */
    int box_push = 0;
    if (nt_kind(g->nt, recv) == NK_LocalVariableReadNode) {
      const char *rn = nt_str(g->nt, recv, "name");
      box_push = rn && nameset_has(&g->poly_arrays, rn);
    }
    if (box_push) {
      cr_emit_recv(g, recv, b);
      buf_printf(b, ".%s(", name);
      for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_boxed(g, argv[i], b); }
      buf_puts(b, ")");
      return 1;
    }
  }
  return 0;
}

/* Kernel call forms with no Crystal equivalent: bare `raise` (re-raise),
   `raise Cls, msg`, `raise Cls`, `exit(true/false)`, and blockless `loop`.
   Returns 1 when it emitted the call. */
static int cr_emit_kernel_call(CrGen *g, int id, const char *name, int recv,
                     int argc, const int *argv, int block, Buf *b) {
  /* Ruby's bare `raise` re-raises `$!`. Crystal has no `$!`: inside a rescue
     block the current exception is the clause's bound name, and re-raising means
     `raise e`. Outside any clause there is nothing to re-raise, and Ruby would
     be raising RuntimeError from nil, so refuse rather than emit a no-arg
     `raise` that fails to compile ("wrong number of arguments, expected 1"). */
  if (recv < 0 && sp_streq(name, "raise") && argc == 0) {
    if (g->rescue_var) { buf_printf(b, "raise(%s)", g->rescue_var); return 1; }
    cr_mark_unsupported(g, id, "a bare `raise` outside a rescue clause "
                                "(Crystal re-raises with `raise e`)");
    buf_puts(b, "nil");
    return 1;
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
        return 1;
      }
    }
    /* A class with no Crystal equivalent cannot be constructed; refuse it here
       rather than emitting an undefined constant, and name the class. */
    if (cls >= 0 && (nt_kind(g->nt, cls) == NK_ConstantReadNode ||
                     nt_kind(g->nt, cls) == NK_ConstantPathNode)) {
      const char *nm = cr_exception_operand(g, cls);
      char why[300];
      snprintf(why, sizeof why,
               "the exception class '%s' has no Crystal equivalent: "
               CR_NO_CRYSTAL_EXC, nm ? nm : "?");
      cr_mark_unsupported(g, cls, why);
    }
  }

  /* Ruby's `raise SomeError` with no message raises a new instance of the
     class; Crystal's `raise` needs the instance, so a bare exception-class
     constant becomes `raise(Cls.new)`. A non-class argument (`raise obj`) is
     left alone. */
  if (recv < 0 && sp_streq(name, "raise") && argc == 1) {
    int cls = cr_peek(g, argv[0]);
    if (cls >= 0 && (nt_kind(g->nt, cls) == NK_ConstantReadNode ||
                     nt_kind(g->nt, cls) == NK_ConstantPathNode)) {
      const char *nm = cr_exception_operand(g, cls);
      const char *cn = nm ? cr_exception_name(g, nm) : NULL;
      if (cn && *cn) { buf_printf(b, "raise(%s.new)", cn); return 1; }
    }
  }

  /* Ruby's exit(true/false) maps success/failure to a status code; Crystal's
     exit takes an Int or a Process::Status, not a Bool. */
  if (recv < 0 && sp_streq(name, "exit") && argc == 1 &&
      (nt_kind(g->nt, argv[0]) == NK_TrueNode || nt_kind(g->nt, argv[0]) == NK_FalseNode)) {
    buf_printf(b, "exit(%d)", nt_kind(g->nt, argv[0]) == NK_TrueNode ? 0 : 1);
    return 1;
  }

  /* A blockless Kernel#loop is an infinite Enumerator (yielding nil) in Ruby;
     Crystal's loop requires a block and has no equivalent enumerator. */
  if (recv < 0 && sp_streq(name, "loop") && block < 0) {
    cr_mark_unsupported(g, id, "a blockless `loop` (an infinite Enumerator in Ruby "
                              "with no Crystal form)");
    buf_puts(b, "nil");
    return 1;
  }
  return 0;
}

/* Does the subtree rooted at `id` contain a node of kind `k`? A generic
   structural walk: ref fields and array-field elements are the children. */
static int cr_subtree_has_kind(CrGen *g, int id, NodeKind k) {
  if (id < 0) return 0;
  if (nt_kind(g->nt, id) == k) return 1;
  for (int i = 0; i < nt_num_refs(g->nt, id); i++)
    if (cr_subtree_has_kind(g, nt_ref_at(g->nt, id, i), k)) return 1;
  for (int i = 0; i < nt_num_arrs(g->nt, id); i++) {
    int n = 0; const int *a = nt_arr_at(g->nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (cr_subtree_has_kind(g, a[j], k)) return 1;
  }
  return 0;
}

/* Ruby's element iterators answer the receiver from a block call; Crystal's
   answer nil. `each_with_object` (the memo) and `cycle` (nil) are not here, and
   `step` is owned by the Numeric#step rewrite. */
static int cr_iter_returns_self(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_with_index") ||
         sp_streq(n, "each_slice") || sp_streq(n, "each_cons") ||
         sp_streq(n, "each_entry") || sp_streq(n, "each_index") ||
         sp_streq(n, "each_char") || sp_streq(n, "each_byte") ||
         sp_streq(n, "each_line") || sp_streq(n, "each_key") ||
         sp_streq(n, "each_value") || sp_streq(n, "each_pair") ||
         sp_streq(n, "reverse_each") || sp_streq(n, "upto") ||
         sp_streq(n, "downto") || sp_streq(n, "times") || sp_streq(n, "step");
}

/* A self-returning iterator whose value is used: wrap it as
   `recv.tap { |r| r.m(args) { blk } }`, which answers the receiver after one
   evaluation of it. A `break` in the block makes the iterator answer the break
   value (nil or the value) in BOTH languages, so such a call is left alone.
   Returns 1 when it emitted the call. */
static int cr_emit_self_return_wrap(CrGen *g, int id, const char *name, int recv,
                                    int argc, const int *argv, int block, Buf *b) {
  if (g->cur_value == 0 || recv < 0 || block < 0) return 0;
  if (!cr_iter_returns_self(name)) return 0;
  TyKind rt = comp_ntype(g->c, recv);
  /* Only a receiver emitted verbatim (an Array or Hash): a Range or String
     receiver is rewritten to a materialized Array by the emitter, so `tap`
     would answer the Array rather than Ruby's receiver. `step` on a Range is
     the exception -- the emitter keeps the Range, so `tap` answers it. */
  int range_step = (rt == TY_RANGE || rt == TY_STR_RANGE) && sp_streq(name, "step");
  if (!ty_is_array(rt) && !ty_is_hash(rt) && !range_step) return 0;
  if (cr_is_poly(rt)) return 0;
  if (cr_user_method_ok(g, name, rt, 0)) return 0;
  const char *cop = nt_str(g->nt, id, "call_operator");
  if (cop && sp_streq(cop, "&.")) return 0;   /* safe-nav has its own path */
  if (cr_subtree_has_kind(g, block, NK_BreakNode)) return 0;
  const char *mname = cr_translate_name(g, id, recv, name);
  char tmp[48];
  snprintf(tmp, sizeof tmp, "__cr_ret%d", g->tmp_seq++);
  cr_emit_recv(g, recv, b);
  buf_printf(b, ".tap { |%s| %s.%s(", tmp, tmp, mname);
  for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
  buf_puts(b, ")");
  cr_emit_block(b, g, block, 0, recv, name);
  buf_puts(b, " }");
  return 1;
}

static int cr_emit_call_special(CrGen *g, int id, const char *name, int recv,
                                int argc, const int *argv, int block, Buf *b) {
  /* unary - / + : no receiver, one argument */
  if (recv < 0 && argc == 1 && (sp_streq(name, "-") || sp_streq(name, "+"))) {
    buf_printf(b, "%c(", name[0]); cr_emit_expr(g, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* unary ! : receiver present, no arguments. Ruby's ! answers a Bool (the
     truthiness negation), so a widened operand asks its SpValue. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "!")) {
    buf_puts(b, "!("); cr_emit_cond(g, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* unary -x / +x on a value: Prism names these "-@" / "+@" with the operand as
     the receiver. Crystal has the same unary operators. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "-@") || sp_streq(name, "+@"))) {
    buf_printf(b, "%c(", name[0]); cr_emit_expr(g, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* `arr.<stage>.with_index(...)` chains (map/each/select/reject) and their
     blockless pair-Enumerator form. */
  if (cr_emit_with_index(g, id, b)) return 1;
  /* `arr.<stage>.with_index(...).to_a`: Ruby's Enumerator yields [element,
     index] Arrays; the Crystal chain yields {element, index} Tuples. Convert at
     the terminal so the rendering matches -- a Tuple auto-splats in a block and
     an Array does not, so the conversion cannot be pushed any earlier. */
  if (recv >= 0 && block < 0 && sp_streq(name, "to_a") && cr_is_blockless_wi(g, recv)) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".map(&.to_a).to_a");
    return 1;
  }
  /* A blockless each_with_index is a pair Enumerator in Ruby. Crystal's is a
     lazy Iterator with no sort_by/size and without Ruby's Enumerable shape, so
     materialize it to an Array of {element, index} Tuples -- what the
     sort_by.with_index desugar and the to_a/to_h/reduce chains expect. */
  if (recv >= 0 && block < 0 && sp_streq(name, "each_with_index") &&
      ty_is_array(comp_ntype(g->c, recv))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".each_with_index().to_a");
    return 1;
  }
  if (cr_emit_kernel_call(g, id, name, recv, argc, argv, block, b)) return 1;

  /* Crystal's is_a? / kind_of? / instance_of? take a TYPE, so a variable holding
     a class has no Crystal form (a constant is emitted as-is). A constant naming
     a MODULE is also refused: Crystal's `is_a?` against a mixin is not Ruby's
     `is_a?` against the module (a value whose class merely includes it answers
     false), so the two are not interchangeable. */
  if (recv >= 0 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
    int is_const_arg = argc == 1 &&
                       (nt_kind(g->nt, argv[0]) == NK_ConstantReadNode ||
                        nt_kind(g->nt, argv[0]) == NK_ConstantPathNode);
    int bad = !is_const_arg;   /* a runtime class value has no type form */
    if (is_const_arg) {
      const char *cn = nt_str(g->nt, argv[0], "name");
      int ci = cn ? comp_class_index(g->c, cn) : -1;
      if (ci >= 0 && comp_class_is_module(g->c, &g->c->classes[ci])) bad = 1;
    }
    if (bad) {
      cr_mark_unsupported(g, id, "is_a?/kind_of?/instance_of? with a runtime class "
                                "value or a module (Crystal takes a type)");
      buf_puts(b, "nil");
      return 1;
    }
  }

  if (cr_emit_poly_call(g, id, name, recv, argc, argv, block, b)) return 1;

  /* Ruby's Array#at is `[]` with a nil sentinel out of range; Crystal's `at`
     raises IndexError, so it maps onto the `[]?` form. */
  if (recv >= 0 && sp_streq(name, "at") && argc == 1 && ty_is_array(comp_ntype(g->c, recv))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, "[");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, "]?");
    return 1;
  }

  /* Ruby's String#chars yields an array of one-character Strings; Crystal's
     String#chars yields Chars. The analyzer models the result as a string array
     (a String read out of a container during the poly pass, and TY_STR_ARRAY on
     the concrete pass), so a later use of an element as a String -- a hash
     lookup, `==` against a literal, string concatenation -- would take the Char
     path and raise at runtime (issue 2903). Map each Char back to a String for
     the blockless form. The block form cannot be rewritten without changing what
     the block parameter is, so it is refused rather than silently Char-typed. */
  if (recv >= 0 && sp_streq(name, "chars") && argc == 0 &&
      comp_ntype(g->c, recv) == TY_STRING &&
      !cr_user_method_ok(g, name, TY_STRING, 0)) {
    if (block >= 0) {
      cr_mark_unsupported(g, id, "String#chars with a block (Crystal's chars "
                                "yields Char; Ruby's yields one-character Strings)");
      buf_puts(b, "nil");
      return 1;
    }
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".chars.map(&.to_s)");
    return 1;
  }

  /* Ruby's String#to_i auto-detects the base's prefix for base 0, treats a
     leading zero as octal only there, accepts `_` digit separators, and never
     raises (it answers 0 when no digits parse); Crystal's to_i defaults to
     strict:true (raising ArgumentError) with no prefix/underscore handling
     (string.cr `to_i`). Set the flags that make Crystal answer as Ruby does. */
  if (recv >= 0 && sp_streq(name, "to_i") && argc <= 1 &&
      comp_ntype(g->c, recv) == TY_STRING &&
      !cr_user_method_ok(g, name, TY_STRING, 0)) {
    /* Crystal rejects base 0 outright, so Ruby's autodetect is spelled by
       leaving the (valid) default base in place and letting prefix/octal
       detection override it per string. A bare `s.to_i` keeps Crystal's default
       base 10 and only needs strict/underscore. The `{ 0 }` block is Ruby's "no
       digits parsed means 0": Crystal's to_i otherwise raises on "0", "08" or
       "", where Ruby answers 0. */
    int base0 = argc == 1 && nt_kind(g->nt, argv[0]) == NK_IntegerNode &&
                nt_int(g->nt, argv[0], "value", -1) == 0;
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".to_i(");
    if (base0)          buf_puts(b, "10, ");
    else if (argc == 1) { cr_emit_expr(g, argv[0], b); buf_puts(b, ", "); }
    buf_puts(b, "strict: false, underscore: true");
    if (base0) buf_puts(b, ", prefix: true, leading_zero_is_octal: true");
    buf_puts(b, ") { 0 }");
    return 1;
  }

  /* Crystal's Float#to_i returns an Int32 and raises once the value passes
     2**31; Ruby's returns a full Integer. Crystal's Float#to_i64 keeps Ruby's
     value across the whole Int64 range, so use it. Past Int64 the two still
     differ -- Ruby yields a Bignum, Crystal raises OverflowError -- so a float
     literal outside the range is refused rather than shipped as a crash. */
  if (recv >= 0 && sp_streq(name, "to_i") && argc == 0 &&
      comp_ntype(g->c, recv) == TY_FLOAT &&
      !cr_user_method_ok(g, name, TY_FLOAT, 0)) {
    if (nt_kind(g->nt, recv) == NK_FloatNode) {
      const char *c = nt_content(g->nt, recv);
      double d = c ? strtod(c, NULL) : 0.0;
      if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        cr_mark_unsupported(g, id, "Float#to_i outside the Int64 range (Ruby "
                                  "returns a Bignum; Crystal raises OverflowError)");
        buf_puts(b, "nil");
        return 1;
      }
    }
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".to_i64");
    return 1;
  }

  /* Integer#round/ceil/floor/truncate(ndigits): n >= 0 is self; n < 0 rounds to
     10**(-n) exactly. A literal past 10**18 needs a Bignum, so refuse. */
  if (recv >= 0 && argc == 1 && comp_ntype(g->c, recv) == TY_INT &&
      (sp_streq(name, "round") || sp_streq(name, "ceil") ||
       sp_streq(name, "floor") || sp_streq(name, "truncate"))) {
    int lit = nt_kind(g->nt, argv[0]) == NK_IntegerNode;
    long long n = lit ? nt_int(g->nt, argv[0], "value", 0) : 0;
    if (lit && n <= -19) {
      cr_mark_unsupported(g, id, "Integer#round/ceil/floor/truncate with an "
                                "ndigits below 0 (modulus past 10**18; Ruby "
                                "returns a Bignum)");
      buf_puts(b, "nil");
      return 1;
    }
    if (lit && n >= 0) { cr_emit_recv(g, recv, b); return 1; }   /* ndigits >= 0: self */
    buf_printf(b, "sp_int_%s(", name);
    cr_emit_recv(g, recv, b);
    buf_puts(b, ", ");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }

  /* Float#round/ceil/floor/truncate(ndigits): Crystal has Float#round(n) but no
     digit-taking ceil/floor/truncate, so the scaling helpers. CRuby returns an
     Integer for a literal ndigits <= 0, so append .to_i there. */
  if (recv >= 0 && argc == 1 && comp_ntype(g->c, recv) == TY_FLOAT &&
      (sp_streq(name, "round") || sp_streq(name, "ceil") ||
       sp_streq(name, "floor") || sp_streq(name, "truncate"))) {
    int lit = nt_kind(g->nt, argv[0]) == NK_IntegerNode;
    long long n = lit ? nt_int(g->nt, argv[0], "value", 0) : 0;
    int want_int = lit && n <= 0;
    if (sp_streq(name, "round")) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".round(");
      cr_emit_expr(g, argv[0], b);
      buf_puts(b, ")");
    } else {
      buf_printf(b, "sp_flt_%s(", name);
      cr_emit_recv(g, recv, b);
      buf_puts(b, ", ");
      cr_emit_expr(g, argv[0], b);
      buf_puts(b, ")");
    }
    if (want_int) buf_puts(b, ".to_i");
    return 1;
  }

  /* Ruby's Float#round/ceil/floor/truncate with no argument answer an Integer;
     Crystal's round/ceil/floor answer a Float64 and it has no truncate. */
  if (recv >= 0 && argc == 0 && comp_ntype(g->c, recv) == TY_FLOAT &&
      (sp_streq(name, "round") || sp_streq(name, "ceil") ||
       sp_streq(name, "floor") || sp_streq(name, "truncate"))) {
    cr_emit_recv(g, recv, b);
    if (!sp_streq(name, "truncate")) buf_printf(b, ".%s", name);
    buf_puts(b, ".to_i");
    return 1;
  }

  /* Ruby's min/max answer nil on an empty collection; Crystal's min/max raise
     Enumerable::EmptyError. The `?` form returns the nilable counterpart, so it
     is used only where the surrounding context already tolerates nil -- the same
     gate the `[]` read uses -- to avoid widening an element type that a later
     non-nil use depends on. */
  if (recv >= 0 && (sp_streq(name, "min") || sp_streq(name, "max")) &&
      argc == 0 && block < 0 && g->nil_ok &&
      (ty_is_array(comp_ntype(g->c, recv)) || comp_ntype(g->c, recv) == TY_RANGE)) {
    cr_emit_recv(g, recv, b);
    buf_printf(b, ".%s?()", name);
    return 1;
  }

  /* Ruby's String#split differs from Crystal's on all three of whitespace (awk)
     mode for a single-space separator, trailing-empty dropping under the default
     limit, and the positive-cap / negative-keep limit rule. Route a String (or
     nil, or no) separator through the sp_ruby_split helpers. A Regexp or Char
     separator keeps the plain path. A block yields each field (split's own
     return value is the receiver, which the corpus does not use). */
  int str_sep = argc == 0 || nt_kind(g->nt, argv[0]) == NK_NilNode ||
                (recv >= 0 && comp_ntype(g->c, argv[0]) == TY_STRING);
  if (recv >= 0 && sp_streq(name, "split") && comp_ntype(g->c, recv) == TY_STRING &&
      argc <= 2 && str_sep) {
    buf_puts(b, "sp_ruby_split(");
    cr_emit_recv(g, recv, b);
    buf_puts(b, ", ");
    if (argc >= 1) cr_emit_expr(g, argv[0], b); else buf_puts(b, "nil");
    if (argc == 2) { buf_puts(b, ", "); cr_emit_expr(g, argv[1], b); }
    buf_puts(b, ")");
    if (block >= 0) { buf_puts(b, ".each"); cr_emit_block(b, g, block, 0, recv, "each"); }
    return 1;
  }

  /* Crystal's values_at answers a Tuple, and zip an Array of Tuples, where Ruby
     answers a plain Array (of Arrays for zip). `.to_a` / `.map(&.to_a)`
     restores Ruby's shape; a later `|a, b|` destructuring still works on an
     Array, so this is safe when the result is consumed either way. */
  if (recv >= 0 && block < 0 &&
      (sp_streq(name, "values_at") || sp_streq(name, "zip")) &&
      ty_is_array(comp_ntype(g->c, recv))) {
    /* `arr.values_at` with no index answers []; Crystal's values_at needs at
       least one. `first(0)` is an empty Array of the right element type. */
    if (sp_streq(name, "values_at") && argc == 0) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".first(0)");
      return 1;
    }
    cr_emit_recv(g, recv, b);
    buf_printf(b, ".%s(", name);
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
    buf_puts(b, sp_streq(name, "zip") ? ").map(&.to_a)" : ").to_a");
    return 1;
  }

  /* Ruby's Enumerable#count with no argument and no block is the size; Crystal's
     requires an item or a block (there is no 0-argument count). */
  if (recv >= 0 && block < 0 && argc == 0 && sp_streq(name, "count")) {
    TyKind rt = comp_ntype(g->c, recv);
    if (ty_is_array(rt) || ty_is_hash(rt) || rt == TY_RANGE) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".size");
      return 1;
    }
  }

  /* Ruby's Hash#fetch(key) with no default raises KeyError on a miss, which is
     exactly Crystal's `h[key]`; Crystal's own fetch() requires a default. */
  if (recv >= 0 && block < 0 && argc == 1 && sp_streq(name, "fetch") &&
      ty_is_hash(comp_ntype(g->c, recv))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, "[");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, "]");
    return 1;
  }

  /* Ruby's File.delete / File.unlink with no path delete nothing and answer 0;
     Crystal's require at least one path. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "delete") || sp_streq(name, "unlink")) &&
      nt_kind(g->nt, recv) == NK_ConstantReadNode &&
      sp_streq(nt_str(g->nt, recv, "name"), "File")) {
    buf_puts(b, "0");
    return 1;
  }

  /* Ruby's Numeric#step(limit, step) takes positional bounds; Crystal's step
     takes named `to:` / `by:`. */
  if (recv >= 0 && sp_streq(name, "step") && (argc == 1 || argc == 2)) {
    TyKind rt = comp_ntype(g->c, recv);
    if (rt == TY_INT || rt == TY_FLOAT) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".step(to: ");
      if (argc == 2) { cr_emit_expr(g, argv[0], b); buf_puts(b, ", by: "); cr_emit_expr(g, argv[1], b); }
      else cr_emit_expr(g, argv[0], b);
      buf_puts(b, ")");
      if (block >= 0) cr_emit_block(b, g, block, 0, recv, name);
      return 1;
    }
  }

  /* A blockless each_index / each_cons / each_slice is a lazy Iterator in
     Crystal, so a following map/first/sum runs lazily (or yields an iterator)
     where Ruby materializes an Array. These yield Arrays, not Tuples, so a plain
     .to_a restores Ruby's shape -- and a following block destructuring (`|k, v|`)
     still works on the Array. */
  if (recv >= 0 && block < 0 && sp_streq(name, "each_index")) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".each_index().to_a");
    return 1;
  }

  /* Ruby's Enumerable#first(n) answers an Array; Crystal's Array#first(n) does
     too, but an Iterator's first(n) answers a lazy iterator (so `p` would show
     the iterator). Append .to_a: identity for an Array, materializing for an
     iterator's first n. */
  if (recv >= 0 && sp_streq(name, "first") && argc == 1) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".first(");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, ").to_a");
    return 1;
  }





  /* `reduce(:op)` / `inject(seed, :op)` names the fold by an OPERATOR symbol.
     The shared desugarer rewrites only identifier-named symbols to a block
     (desugar_reduce_method_symbol); operator symbols ride the C backend's typed
     fold emitters, which the Crystal emitter does not have -- so it emitted
     `reduce(:+)`, which Crystal rejects. Spell the fold as a block here. */
  if (recv >= 0 && block < 0 && argc >= 1 &&
      (sp_streq(name, "reduce") || sp_streq(name, "inject"))) {
    int last = argv[argc - 1];
    const char *sym = nt_kind(g->nt, last) == NK_SymbolNode
                      ? nt_str(g->nt, last, "value") : NULL;
    if (sym && cr_is_binop(sym)) {
      /* A symbol fold over a Hash folds Ruby's [k, v] pair Arrays; Crystal's
         Hash#reduce yields {k, v} Tuples, so the fold is a different type (and
         a growing Tuple at that). Refuse rather than ship it. */
      if (ty_is_hash(comp_ntype(g->c, recv))) {
        cr_mark_unsupported(g, id, "a symbol fold over a Hash (Ruby folds [k, v] pairs)");
        buf_puts(b, "nil");
        return 1;
      }
      char ra[32], rb[32];
      int t = g->tmp_seq++;
      snprintf(ra, sizeof ra, "__sp_rd%d_a", t);
      snprintf(rb, sizeof rb, "__sp_rd%d_b", t);
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".reduce(");
      for (int i = 0; i < argc - 1; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
      buf_printf(b, ") do |%s, %s|\n  %s %s %s\nend", ra, rb, ra, sym, rb);
      return 1;
    }
  }

  /* Ruby's Array#join flattens nested Arrays recursively before joining, with
     the separator between every element; Crystal's join renders a nested Array
     with its inspect form. `flatten` recurses through Arrays only -- the same
     set Ruby's join flattens -- and leaves scalar elements alone. */
  if (recv >= 0 && sp_streq(name, "join") && block < 0 &&
      ty_is_array(comp_ntype(g->c, recv))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".flatten.join(");
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
    buf_puts(b, ")");
    return 1;
  }

  /* Ruby's charset arguments use range notation, and a lone "^" is a literal
     caret. Crystal's delete/count/squeeze parse both the same way EXCEPT the
     lone "^" (there it negates the empty set and matches every character), and
     Crystal's tr expands no ranges at all. Rewrite the literal charset(s). */
  if (recv >= 0 && comp_ntype(g->c, recv) == TY_STRING) {
    if (sp_streq(name, "tr") && argc == 2 &&
        nt_kind(g->nt, argv[0]) == NK_StringNode && nt_kind(g->nt, argv[1]) == NK_StringNode) {
      cr_emit_recv(g, recv, b);
      buf_puts(b, ".tr(");
      cr_emit_charset_literal(b, nt_str(g->nt, argv[0], "content"), 1);
      buf_puts(b, ", ");
      cr_emit_charset_literal(b, nt_str(g->nt, argv[1], "content"), 1);
      buf_puts(b, ")");
      return 1;
    }
    const char *cs = argc == 1 && nt_kind(g->nt, argv[0]) == NK_StringNode
                     ? nt_str(g->nt, argv[0], "content") : NULL;
    if (cs && sp_streq(cs, "^") &&
        (sp_streq(name, "delete") || sp_streq(name, "count") || sp_streq(name, "squeeze"))) {
      cr_emit_recv(g, recv, b);
      buf_printf(b, ".%s(", name);
      cr_emit_charset_literal(b, "\\^", 0);
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Regexp#options in Ruby is an Integer bitmask (1=IGNORECASE, 2=EXTENDED,
     4=MULTILINE); Crystal's `options` returns its own Options enum, which the
     stdlib uses internally, so it cannot be overridden. Answer through the
     shim's ruby_options helper instead. */
  if (recv >= 0 && sp_streq(name, "options") && argc == 0 &&
      comp_ntype(g->c, recv) == TY_REGEX) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".ruby_options");
    return 1;
  }

  /* Ruby's Time#utc / #getlocal (no argument) answer a Time in the UTC / local
     zone; Crystal spells those to_utc / to_local. The argument-taking
     getlocal(offset) and the in-place gmtime/localtime are refused by the
     receiver-typed table below. */
  if (recv >= 0 && argc == 0 && comp_ntype(g->c, recv) == TY_TIME &&
      (sp_streq(name, "utc") || sp_streq(name, "getlocal"))) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, sp_streq(name, "utc") ? ".to_utc" : ".to_local");
    return 1;
  }

  /* Ruby's Math.lgamma answers [log|Gamma(x)|, sign]; Crystal's answers the log
     alone. Pair it with the computed sign; the argument is bound to a temp so a
     side-effecting expression is evaluated once. */
  if (recv >= 0 && sp_streq(name, "lgamma") && argc == 1 &&
      nt_kind(g->nt, recv) == NK_ConstantReadNode &&
      sp_streq(nt_str(g->nt, recv, "name"), "Math")) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "__sp_lg%d", g->tmp_seq++);
    buf_printf(b, "(begin %s = ", tmp);
    cr_emit_expr(g, argv[0], b);
    buf_printf(b, "; [Math.lgamma(%s), sp_lgamma_sign(%s)] end)", tmp, tmp);
    return 1;
  }

  /* Ruby's sub/gsub replacement strings expand `\&` (the whole match) and
     `\k<name>` (empty when the group did not participate); Crystal's replacement
     parser leaves `\&` literal and raises IndexError for a non-participating
     `\k<name>`. The two languages are not interchangeable, and a computed
     replacement hides its contents, so only a literal one is inspected. */
  if (recv >= 0 && (sp_streq(name, "sub") || sp_streq(name, "gsub")) &&
      argc == 2 && nt_kind(g->nt, argv[1]) == NK_StringNode &&
      cr_str_has_ruby_only_replacement(nt_str(g->nt, argv[1], "content"))) {
    cr_mark_unsupported(g, id, "a sub/gsub replacement using a Ruby-only "
                              "backreference (whole match, pre/post match, "
                              "last group, or named group)");
    buf_puts(b, "nil");
    return 1;
  }

  /* Ruby's Range#% is an alias for Range#step (`(1..10) % 3`); Crystal's Range
     has no `%`, so emit the step call. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "%") &&
      (comp_ntype(g->c, recv) == TY_RANGE || comp_ntype(g->c, recv) == TY_STR_RANGE)) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".step(");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  /* Ruby's Enumerable#minmax answers a 2-element Array (nil-padded when empty);
     Crystal's minmax raises on an empty collection and answers a Tuple.
     `minmax?` answers a nil-padded Tuple for the same value, and `.to_a`
     restores Ruby's Array shape and the [nil, nil] empty answer. Array/Range
     only -- a Hash's minmax folds [k, v] pairs (a different shape). */
  if (recv >= 0 && block < 0 && sp_streq(name, "minmax") &&
      (ty_is_array(comp_ntype(g->c, recv)) || comp_ntype(g->c, recv) == TY_RANGE)) {
    cr_emit_recv(g, recv, b);
    buf_puts(b, ".minmax?.to_a");
    return 1;
  }
  if (cr_emit_binop_call(g, id, name, recv, argc, argv, block, b)) return 1;

  /* index read: Crystal uses bracket syntax, not a .[]() call.
     Ruby's `h[k]` / `a[i]` answer nil for a missing key or an out-of-range index;
     Crystal's `[]` raises KeyError/IndexError. The `?` form is the translation
     that keeps Ruby's answer. Restricted to a single scalar index on a receiver
     the inference knows is a Hash or Array, so a Range slice (`a[1..2]`) or a
     non-container receiver keeps the raising form rather than a guess. */
  if (recv >= 0 && sp_streq(name, "[]")) {
    TyKind rt = comp_ntype(g->c, recv);
    /* Ruby's Integer#[] is a bit read; Crystal's Int has no []. (`[]` is
       special-cased before the missing-method table, so it needs its own guard.)
       A Symbol#[] slice is likewise rejected below by the plain emission. */
    if (rt == TY_INT || rt == TY_BIGINT) {
      cr_mark_unsupported(g, id, "Ruby's Integer#[] bit read has no Crystal form");
      buf_puts(b, "nil");
      return 1;
    }
    int nilable = g->nil_ok && argc == 1 &&
                  nt_kind(g->nt, argv[0]) != NK_RangeNode &&
                  (ty_is_hash(rt) || ty_is_array(rt));
    /* `s[regexp, name]` is the named-capture read. Ruby answers nil when the
       group did not participate; Crystal's `[]` instead declares a non-nilable
       String and asserts a match, so the nil answer is the `[]?` form. This is
       always correct -- the group may or may not have matched -- so it does not
       depend on the surrounding nil-tolerance. */
    if (argc == 2 && comp_ntype(g->c, argv[0]) == TY_REGEX) nilable = 1;
    /* ENV is a String-keyed Hash in Ruby: a missing key answers nil rather than
       raising. Crystal's `ENV[]?` is that form (and `ENV[]=` already accepts nil
       to delete), so this is always the nil-returning read, not a guess. */
    if (nt_kind(g->nt, recv) == NK_ConstantReadNode &&
        sp_streq(nt_str(g->nt, recv, "name"), "ENV")) nilable = 1;
    /* The flag is about THIS read only. Clear it for the receiver and the index
       so a nested read is not silently made nilable: in `t[k][j]`, making the
       inner `t[k]` nilable would change the type of the outer receiver. */
    int savr = g->nil_ok; g->nil_ok = 0;
    cr_emit_recv(g, recv, b);
    buf_puts(b, "[");
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); cr_emit_expr(g, argv[i], b); }
    g->nil_ok = savr;
    buf_puts(b, nilable ? "]?" : "]");
    return 1;
  }

  /* index write: `a[i] = v` reaches the analyzer as a .[]= call. Crystal's
     bracket assignment is an expression that evaluates to the stored value,
     the same as Ruby's, so this holds in statement and value position alike. */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    /* Crystal's String is immutable; Ruby's String#[]= mutates in place. (`[]=`
       is special-cased before the missing-method table, so guard it here.) */
    if (comp_ntype(g->c, recv) == TY_STRING) {
      cr_mark_unsupported(g, id, "Crystal's String is immutable and has no []=; a Ruby index-assign needs a mutable buffer");
      buf_puts(b, "nil");
      return 1;
    }
    cr_emit_recv(g, recv, b);
    buf_puts(b, "[");
    cr_emit_expr(g, argv[0], b);
    buf_puts(b, "] = ");
    cr_emit_expr(g, argv[1], b);
    return 1;
  }
  return 0;
}

static void cr_emit_call(CrGen *g, int id, Buf *b) {
  const char *name = nt_str(g->nt, id, "name");
  int recv  = nt_ref(g->nt, id, "receiver");
  int argc  = 0; const int *argv = call_args(g->nt, id, &argc);
  int block = nt_ref(g->nt, id, "block");
  if (!name) { g->unsupported++; buf_puts(b, "nil"); return; }
  if (cr_emit_call_special(g, id, name, recv, argc, argv, block, b)) return;
  /* Ruby's self-returning element iterators (each, times, upto, ...) answer the
     receiver; Crystal's answer nil. When the value is used, wrap in `tap`. */
  if (cr_emit_self_return_wrap(g, id, name, recv, argc, argv, block, b)) return;
  /* method call: refuse a name we cannot verify, so an unknown method never
     ships as a silent build error. A nil placeholder keeps emission going; the
     run is refused at the end (codegen_program_crystal). */
  Scope *callee = NULL;
  {
    /* A call into a compiler-synthesized "__" helper: the analyzer desugars some
       Ruby methods (between?, <=>, Enumerable#sum, ...) into __cmp_/__enumw_/
       __bam_ wrappers that the C backend emits but this one deliberately skips
       (Crystal's stdlib answers them). We never emit those bodies, so a call to
       one would dangle -- refuse it rather than ship a reference to nothing. */
    if (name[0] == '_' && name[1] == '_') {
      /* P22r: a desugared Enumerable call whose Crystal stdlib form is
         identical. The helper is `__enum_m(recv, args)`, the receiver first;
         reconstruct `recv.m(args) { blk }`. A `reduce`/`inject` op-symbol
         argument (`arr.inject(:+)`) has no Crystal form, so it stays refused.
         The block is emitted with the RUBY name (the yield oracle is keyed on
         Ruby names), the call with the Crystal name. */
      char ruby[32];
      TyKind erty = argc >= 1 ? comp_ntype(g->c, argv[0]) : TY_UNKNOWN;
      const char *real = cr_enum_passthrough(name, erty, ruby, sizeof ruby);
      int sym_op = 0;
      for (int i = 1; i < argc; i++)
        if (nt_kind(g->nt, argv[i]) == NK_SymbolNode) sym_op = 1;
      if (real && !(sym_op && sp_streq(real, "reduce")) && recv < 0 && argc >= 1) {
        cr_emit_recv(g, argv[0], b);
        buf_printf(b, ".%s(", real);
        for (int i = 1; i < argc; i++) {
          if (i > 1) buf_puts(b, ", ");
          cr_emit_expr(g, argv[i], b);
        }
        buf_puts(b, ")");
        if (block >= 0) cr_emit_block(b, g, block, 0, argv[0], ruby);
        return;
      }
      cr_mark_unsupportedf(g, id, "a compiler-synthesized helper call (`%s`)", name);
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
    /* The callee's scope, so an argument at a widened parameter position can be
       boxed to match its SpValue parameter type. */
    if (user_meth) callee = cr_callee_scope(g, name, rt, bare);
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
      /* With the poly runtime on, a widened receiver is not concrete for this
         gate: its methods are answered by the runtime's SpValue, and an unknown
         one is a gap the value's own dispatch reports. With it off, a widened
         receiver keeps the baseline behaviour (refuse an unknown name). */
      if (!bare && !(CR_POLY_DISPATCH && cr_is_poly(rt))) {
        Buf probe; memset(&probe, 0, sizeof probe);
        concrete = cr_map_type(g, rt, &probe); free(probe.p);
      }
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
  const char *mname = cr_translate_name(g, id, recv, name);
  if (recv >= 0) {
    /* `x.nil?` asks whether x is nil, and `x.inspect` / `x.to_s` are defined on
       Nil in Crystal, so all three want Ruby's nilable read rather than one that
       raises before the question can be asked. */
    int sav = g->nil_ok;
    if (sp_streq(name, "nil?") || sp_streq(name, "inspect") || sp_streq(name, "to_s"))
      g->nil_ok = 1;
    int safe_nav = 0;
    {
      /* `&.` is in the node's own `call_operator` and the emitter ignored it, so
         every safe navigation became a plain `.`. That is right only when the
         receiver cannot be nil; on a nilable one it is `undefined method 'm' for
         Nil`. Rewrite it below as `try`, which is Ruby's `&.` on a nilable AND a
         plain call on a non-nilable, so no nilability knowledge is needed --
         Spinel's TyKind carries none. */
      const char *cop = nt_str(g->nt, id, "call_operator");
      if (cop && sp_streq(cop, "&.")) safe_nav = 1;
    }
    if (safe_nav) {
      /* `x&.m` as a parenthesised ARGUMENT cannot use `&.` -- `p(x&.m())` is
         `unterminated call`, because `&.` binds looser than the call's own parens.
         `try` is the exact rewrite: it evaluates the receiver once, skips the block
         when it is nil, and answers nil -- which is Ruby's `&.`. (A nil test in a
         ternary does NOT narrow in Crystal, so `(x.nil? ? nil : x.m)` still
         reported `undefined method 'm' for Nil`.) */
      char tmp[48];
      snprintf(tmp, sizeof tmp, "__cr_sn%d", g->tmp_seq++);
      cr_emit_recv(g, recv, b);
      buf_printf(b, ".try { |%s| %s.%s(", tmp, tmp, mname);
      int sav2 = g->nil_ok;
      if (sp_streq(name, "p") || sp_streq(name, "puts") || sp_streq(name, "print") ||
          sp_streq(name, "pp"))
        g->nil_ok = 1;
      for (int i = 0; i < argc; i++) {
        if (i) buf_puts(b, ", ");
        if (CR_POLY_BOXED && callee && cr_param_poly(callee, i)) cr_emit_boxed(g, argv[i], b);
        else cr_emit_expr(g, argv[i], b);
      }
      g->nil_ok = sav2;
      buf_puts(b, ")");
      /* The block belongs INSIDE the `try` braces: `x&.m { }` is a call of `m`
         with a block, so it must read `try { |t| t.m() { ... } }`. Appending it
         after the closing brace gave `try { |t| t.m() } do ... end`, which is a
         parse error (`expecting identifier 'end', not 'do'`). */
      if (block >= 0) cr_emit_block(b, g, block, 0, recv, name);
      buf_puts(b, " }");
      return;
    }
    cr_emit_recv(g, recv, b);
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
  for (int i = 0; i < argc; i++) {
    if (i) buf_puts(b, ", ");
    if (CR_POLY_BOXED && callee && cr_param_poly(callee, i)) cr_emit_boxed(g, argv[i], b);
    else cr_emit_expr(g, argv[i], b);
  }
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
            TyKind et; int widen = 0, refuse_id = -1; TyKind refuse_kind = TY_UNKNOWN;
            const char *refuse_why = NULL;
            cr_find_pushed_elem(g, name, &et, &widen, &refuse_id, &refuse_kind, &refuse_why);
            /* A non-scalar element (or an unsafe index write) cannot ride the
               P21 scalar-array representation: refuse at the offending write
               rather than emit code that fails to build. */
            if (refuse_id >= 0) {
              const char *kn = ty_is_object(refuse_kind) &&
                               ty_object_class(refuse_kind) < g->c->nclasses
                                ? g->c->classes[ty_object_class(refuse_kind)].name
                                : ty_name(refuse_kind);
              char msg[208];
              snprintf(msg, sizeof msg, "%s (%s)", refuse_why ? refuse_why : "unsupported array write", kn);
              cr_mark_unsupported(g, refuse_id, msg);
            } else if (widen) {
              /* Disagreeing scalar pushes widen the element to SpValue (P21). */
              cr_indent(b, indent);
              buf_printf(b, "%s : Array(SpValue) = Array(SpValue).new", cr_local(name));
              nameset_add(&g->locals, name);
              nameset_add(&g->poly_arrays, name);
              buf_puts(b, "\n");
              return;
            } else if (et != TY_UNKNOWN && et != TY_POLY) {
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
      /* A local the inference widened to poly is the boxed runtime type when
         the poly runtime is active. Every write is boxed and the binding is
         declared, so the local is an SpValue at every read (P22 flips this on
         together with the method-boundary boxing it depends on). */
      TyKind lt = cr_local_type(g, id, name);
      if (CR_POLY_LOCALS && cr_is_poly(lt) && !nameset_has(&g->locals, name)) {
        buf_printf(b, "%s : SpValue = ", cr_local(name));
        nameset_add(&g->locals, name);
        nameset_add(&g->boxed_locals, name);
        cr_emit_boxed(g, val, b);
        buf_puts(b, "\n");
        return;
      }
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
        if (CR_POLY_LOCALS && cr_is_poly(lt)) { cr_emit_boxed(g, val, b); buf_puts(b, "\n"); return; }
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
      if ((CR_POLY_BOXED || CR_POLY_IVARS) && cr_is_poly(cr_ivar_type(g, name)))
        cr_emit_boxed(g, val, b);
      else cr_emit_expr(g, val, b);
      buf_puts(b, "\n");
      return;
    }
    case NK_InstanceVariableOperatorWriteNode: {
      /* `@x op= v`. A CONCRETE SCALAR ivar needs no boxing at all -- Crystal carries
         the compound forms natively -- so it emits exactly like the local case
         below. This node used to be refused outright unless the whole-slot
         `CR_POLY_BOXED` was on, which refused a plain `@pos += 4` on an Int64 ivar
         for no reason at all. Two shapes stay refused, each by name. */
      const char *name = nt_str(g->nt, id, "name");
      const char *op   = nt_str(g->nt, id, "binary_operator");
      int val = nt_ref(g->nt, id, "value");
      TyKind vt = cr_ivar_type(g, name);
      cr_indent(b, indent);
      /* Only the ARITHMETIC operators, where Crystal carries the compound form and
         the operand needs no nil guard. `**=`, `<<=`, `|=`, `&&=`, `||=` are
         refused rather than expanded: `<<` against a nilable Array slot, `|` against
         a nilable Integer slot and `||` against an unset ivar each need a guard the
         expansion does not carry. The old refusal said only the node kind
         (`InstanceVariableOperatorWriteNode`); this one names the construct. */
      int arith = op && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") ||
                         sp_streq(op, "/") || sp_streq(op, "%"));
      if (!arith) {
        cr_mark_unsupported(g, id, op && *op
          ? "a compound assignment with an operator Crystal has no compound form for"
          : "a compound assignment to an instance variable with no operator");
        buf_puts(b, "\n");
        return;
      }
      /* Only a CONCRETE SCALAR slot. A WIDENED ivar is declared `SpValue`, so the
         compound form would need an `SpValue#op=` that does not exist, and the
         arithmetic RESULT would have to be unboxed before it could land in the slot
         -- the escape P22h had to avoid. A CONTAINER slot (`@a *= 2` on
         `Array(Int64)?`) needs a nil guard the compound form does not carry; emitted
         as-is it was `expected argument #1 to 'Array(T)#*' to be Int, not String`. */
      int scalar = vt == TY_INT || vt == TY_BIGINT || vt == TY_FLOAT ||
                   vt == TY_STRING || vt == TY_STRBUF || vt == TY_SYMBOL || vt == TY_BOOL;
      if (!scalar) {
        cr_mark_unsupported(g, id, cr_is_poly(vt)
          ? "a compound assignment to a widened instance variable (the boxed form "
            "has no compound operator)"
          : "a compound assignment to an instance variable whose type is not a "
            "concrete scalar (Crystal's compound form needs a non-nil receiver)");
        buf_puts(b, "\n");
        return;
      }
      buf_printf(b, "%s %s= ", name ? name : "", op);
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
      cr_emit_cond(g, pred, b);
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
      cr_emit_cond(g, pred, b); buf_puts(b, "\n");
      cr_emit_stmts(g, body, b, indent + 1);
      cr_indent(b, indent); buf_puts(b, "end\n");
      return;
    }
    case NK_UntilNode: {
      int pred = nt_ref(g->nt, id, "predicate");
      int body = nt_ref(g->nt, id, "statements");
      cr_indent(b, indent); buf_puts(b, "until ");
      cr_emit_cond(g, pred, b); buf_puts(b, "\n");
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
      /* A discarded expression statement's value is unused -- unless it is the
         body's tail, whose value is the method/block return. */
      g->value_ctx = (id == g->tail_value) ? 1 : 0;
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
  /* Crystal's `!` is a pseudo-method the compiler refuses to let a program
     redefine, so a Ruby `def !` has no equivalent. Refuse it (and do not emit a
     body, which would itself be a Crystal error). */
  if (s->name && sp_streq(s->name, "!")) {
    cr_mark_unsupported(g, s->def_node,
      "a user-defined `!` (Crystal's ! is a pseudo-method that cannot be redefined)");
    return;
  }
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
    /* A `nil` default with a concrete inferred type emits `h : String = nil`.
       Crystal only rejects that when the default is INSTANTIATED -- a call
       that omits the argument -- so check for one: every program whose call
       sites all supply the parameter keeps building (folded_ternary_arm_
       takes_result_type), while a visible omitting call makes the build
       error predictable, so refuse at the def. */
    if (dflt >= 0 && nt_kind(g->nt, dflt) == NK_NilNode &&
        cr_param_omitted_anywhere(g, s, reqn + i)) {
      Buf probe; memset(&probe, 0, sizeof probe);
      int concrete = cr_map_type(g, pt, &probe);
      free(probe.p);
      if (concrete) {
        cr_mark_unsupported(g, s->def_node, "an optional parameter with a nil "
                           "default that a call site omits, whose type is "
                           "inferred non-nilable (Crystal cannot restrict the "
                           "default to that type)");
      }
    }
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
  } else if (s->yields) {
    /* The body contains a `yield`, so the method must accept a block. Crystal
       requires it named in the signature (`def f(&)`), and the anonymous form
       is enough because the body reaches it via `yield`, never a name. A method
       with an explicit `&b` parameter is refused above (Crystal infers the proc
       type from the declaration, which would silently answer nil), so the
       anonymous block is the only shape handled. A body whose `yield` calls
       disagree on arity is refused too: Ruby nil-fills the missing block
       parameters, Crystal rejects the second arity outright. */
    if (!cr_scope_yield_arity_uniform(g, s)) {
      cr_mark_unsupported(g, s->def_node,
                          "a method whose `yield` calls take different numbers of "
                          "arguments (Ruby nil-fills; Crystal infers one block arity)");
    } else if (cr_has_unverified_poly_call(g)) {
      cr_mark_unsupported(g, s->def_node,
                          "a yielding method in a program with a call on a poly "
                          "(union) receiver that has no Crystal form");
    } else {
      if (!first) buf_puts(b, ", ");
      first = 0;
      buf_puts(b, "&");
    }
  }
  buf_puts(b, ")\n");

  ScopeMark saved = cr_push_scope(g);
  for (int i = 0; i < reqn; i++) nameset_add(&g->locals, nt_str(g->nt, reqs[i], "name"));
  for (int i = 0; i < opn; i++) nameset_add(&g->locals, nt_str(g->nt, opts[i], "name"));
  int saved_class = g->cur_class;
  Scope *saved_meth = g->cur_meth;
  g->cur_class = s->class_id;   /* receiverless calls resolve against this class */
  g->cur_meth = s;              /* `super` checks itself against this method */
  int saved_uses = g->nil_uses.n;   /* pre-pass: which locals are only read nil-safely */
  cr_scan_nil_uses(g, s->body, &g->nil_uses, 0);
  int saved_tail = g->tail_value;
  g->tail_value = cr_tail_stmt(g, s->body);
  cr_emit_stmts(g, s->body, b, 1);
  g->tail_value = saved_tail;
  g->cur_meth = saved_meth;
  g->cur_class = saved_class;
  g->nil_uses.n = saved_uses;
  cr_pop_scope(g, saved);

  buf_puts(b, "end\n");
  /* Crystal renders an object through `to_s(io : IO)` / `inspect(io : IO)`; a
     no-arg override (every Ruby `to_s`/`inspect` is no-arg) is what Ruby's
     interpolation and `puts` call, but Crystal's own formatters never reach it,
     so `puts obj` / `"#{obj}"` / `p obj` would use the default. Emit a delegating
     IO form so the program's method is the one they actually use. */
  if (!s->is_cmethod && reqn == 0 && opn == 0 && blkparam < 0 &&
      (sp_streq(s->name, "to_s") || sp_streq(s->name, "inspect")))
    buf_printf(b, "def %s(io : IO) : Nil\n  io << %s\nend\n", s->name, s->name);
}

/* True when every READ of instance variable `@name` is a shape the boxed
   SpValue answers correctly for the value's run-time kind, so widening it to
   SpValue cannot turn a working program into an "undefined method" build error.

   The conservative rule: a read is UNSAFE iff it is the receiver of a call that
   resolves to a concrete Crystal method on the value (not an operator, not a
   name SpValue/SpObject answers, not a user method on a poly receiver). Every
   other position -- returned, passed as an argument, indexed, compared, put,
   interpolated, unboxed to the analyzer's narrowed kind -- lets the SpValue
   travel or render, which the runtime handles. This refuses exactly the shapes
   that would otherwise build-fail (`@units.abs` on an Integer, `@obj.foo`). */
static int cr_call_on_concrete_value(CrGen *g, const char *pn) {
  if (!pn) return 1;
  if (sp_streq(pn, "to_s") || sp_streq(pn, "inspect") || sp_streq(pn, "nil?") ||
      sp_streq(pn, "is_a?") || sp_streq(pn, "kind_of?") || sp_streq(pn, "instance_of?") ||
      sp_streq(pn, "==") || sp_streq(pn, "!=") || sp_streq(pn, "!") ||
      sp_streq(pn, "equal?") || sp_streq(pn, "hash") || sp_streq(pn, "class"))
    return 0;                                   /* SpValue/SpObject answers these */
  if (cr_is_binop(pn)) return 0;
  if (sp_streq(pn, "[]")) return 0;   /* SpValue#[] has no arm for a container kind */
  if (cr_user_method_ok(g, pn, TY_POLY, 0)) return 0;   /* sp_send arm */
  return 1;                                     /* a concrete method: unsafe */
}

/* True when `id` appears as a `receiver` of some CallNode, and returns that
   call's name via *out_name (only when it is a single such call). */
static int cr_ivar_receiver_call(CrGen *g, int id, const char **out_name) {
  for (int p = 0; p < g->nt->count; p++) {
    if (nt_kind(g->nt, p) != NK_CallNode) continue;
    if (nt_ref(g->nt, p, "receiver") != id) continue;
    *out_name = nt_str(g->nt, p, "name");
    return 1;
  }
  return 0;
}

static int cr_ivar_reads_safe(CrGen *g, ClassInfo *ci, const char *name) {
  /* A generated/attr writer (`def b=(v); @b = v; end`) assigns the widened ivar
     directly, bypassing the boxing write path, so the slot would receive a raw
     value and build-fail. Refuse the ivar if the class exposes such a writer. */
  const char *base = name && name[0] == '@' ? name + 1 : name;
  for (int w = 0; w < ci->nwriters; w++)
    if (ci->writers[w] && base && sp_streq(ci->writers[w], base)) return 0;
  /* Refuse if any POLY value in the program -- this ivar's read, or a poly
     parameter/local -- is the receiver of a concrete method Crystal cannot
     resolve. Widening the ivar can be the first thing that lets such a program
     emit, and then that unrelated gap becomes a BUILDFAIL in place of the honest
     ivar refusal. Refusing here keeps the diagnostic at the real cause. */
  for (int id = 0; id < g->nt->count; id++) {
    NodeKind k = nt_kind(g->nt, id);
    if (k == NK_InstanceVariableReadNode) {
      const char *rn = nt_str(g->nt, id, "name");
      if (!rn || !sp_streq(rn, name)) continue;
      if (cr_unbox_fn(comp_ntype(g->c, id))) continue;   /* narrows -> unboxed */
    } else if (k != NK_LocalVariableReadNode) {
      continue;
    }
    TyKind t = comp_ntype(g->c, id);
    if (!cr_is_poly(t)) continue;
    if (cr_unbox_fn(t)) continue;
    const char *pn = NULL;
    if (cr_ivar_receiver_call(g, id, &pn) && cr_call_on_concrete_value(g, pn))
      return 0;
  }
  return 1;
}

/* `module_function`, `private`/`public`/`protected`, and `extend`: declaration-
   only class/module-body statements. Bare, or with symbol arguments (a named
   method list); the modifier forms (`private def m`, `module_function def m`) are
   separate DefNodes the analyzer has already marked. Anything with a non-symbol,
   non-`self` argument is NOT a plain marker and is refused by the caller. */
static int cr_visibility_decl(CrGen *g, int id) {
  const char *nm = nt_str(g->nt, id, "name");
  if (!nm) return 0;
  int is_vis = sp_streq(nm, "module_function") || sp_streq(nm, "private") ||
               sp_streq(nm, "public") || sp_streq(nm, "protected") ||
               sp_streq(nm, "extend");
  if (!is_vis) return 0;
  if (nt_ref(g->nt, id, "receiver") >= 0) return 0;
  int argc = 0; const int *argv = call_args(g->nt, id, &argc);
  if (argc == 0) return 1;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(g->nt, argv[i]);
    /* `module_function :named` / `private :x, :y` list method names. */
    if (k == NK_SymbolNode) continue;
    /* `extend self` names the receiver itself. */
    if (sp_streq(nm, "extend") && k == NK_SelfNode) continue;
    return 0;
  }
  return 1;
}

/* True when a top-level/module `module M` or `class M` body for the same
   constant name appears more than once in the program. Crystal resolves an
   `include M` at the point it is written, so a module whose methods are added
   by a LATER reopen is not seen by an earlier includer -- the include would
   silently drop those methods. Such an include is refused rather than
   mistranslated. */
static int cr_name_reopened(CrGen *g, const char *name) {
  if (!name) return 0;
  int seen = 0;
  for (int id = 0; id < g->nt->count; id++) {
    NodeKind k = nt_kind(g->nt, id);
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int cp = nt_ref(g->nt, id, "constant_path");
    if (cp < 0) continue;
    const char *cn = nt_str(g->nt, cp, "name");
    if (cn && sp_streq(cn, name)) { if (++seen > 1) return 1; }
  }
  return 0;
}

/* `include M` where M is a module the program defines: emit it so Crystal's own
   mixin makes the module's methods available to the includer (the emitter does
   NOT transplant them). A builtin mixin (`Comparable`, `Enumerable`, `Kernel`,
   `Math`, `Forwardable`, ...) is skipped instead: Crystal's versions need a type
   argument or have a different method surface, and the analyzer has already
   resolved the calls. Returns 1 when the statement was consumed. */
static int cr_include_decl(CrGen *g, int id, Buf *b) {
  const char *nm = nt_str(g->nt, id, "name");
  if (!nm || !sp_streq(nm, "include")) return 0;
  if (nt_ref(g->nt, id, "receiver") >= 0) return 0;
  int argc = 0; const int *argv = call_args(g->nt, id, &argc);
  if (argc == 0) return 0;
  int emitted = 0;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(g->nt, argv[i]);
    if (k != NK_ConstantReadNode && k != NK_ConstantPathNode) return 0;
    /* A constant-path include (`include A::B`) names the module through a
       parent chain; the leaf alone is ambiguous with a same-named top-level
       module, and the analyzer's transplant used the flat `A_B` name. Emitting
       the leaf would include the wrong type (or none), so refuse it rather than
       mistranslate. */
    if (k == NK_ConstantPathNode) return 0;
    const char *mn = nt_str(g->nt, argv[i], "name");
    int mod = mn ? comp_class_index(g->c, mn) : -1;
    /* a user module: emit the include so Crystal mixes its methods in. Its
       methods must all be defined before the include textually; a module the
       program reopens later would lose them, so refuse that (see above). */
    if (mod >= 0) {
      if (cr_name_reopened(g, g->c->classes[mod].name)) {
        if (emitted) buf_puts(b, "\n");
        return 0;
      }
      if (!emitted) { cr_indent(b, 1); buf_puts(b, "include "); emitted = 1; }
      else buf_puts(b, ", ");
      buf_puts(b, g->c->classes[mod].name);
      continue;
    }
    /* `Kernel`/`Math` are always present in Crystal and the analyzer resolved
       their calls, so the marker is a no-op; skip it. Any OTHER builtin has a
       Crystal surface that differs from Ruby's, so refuse rather than emit or
       drop it silently. */
    if (mn && (sp_streq(mn, "Kernel") || sp_streq(mn, "Math"))) continue;
    if (emitted) buf_puts(b, "\n");
    return 0;
  }
  if (emitted) buf_puts(b, "\n");
  return 1;
}

/* True when module `ci` has a `module_function` method whose body reads or
   writes an instance variable, and the module is included by some class. CRuby
   runs such a method on the INCLUDER's instance, so `@x` is the instance's ivar
   and `self` is the instance; Spinel registers the method class-level, so
   emitting it would read a module-owned slot instead. Crystal rejects the ivar
   in a module method outright ("@instance_vars are not yet allowed in
   metaclasses"), so this is refused rather than mistranslated. */
static int cr_module_function_self_dependent(CrGen *g, ClassInfo *ci, int is_module) {
  if (!ci || !is_module) return 0;
  int mod = (int)(ci - g->c->classes);
  int included = 0;
  for (int k = 0; k < g->c->nclasses && !included; k++)
    for (int m = 0; m < g->c->classes[k].nincluded_mods; m++)
      if (g->c->classes[k].included_mods[m] == mod) { included = 1; break; }
  if (!included) return 0;
  for (int s = 1; s < g->c->nscopes; s++) {
    Scope *sc = &g->c->scopes[s];
    if (sc->class_id != mod || !sc->is_module_function || sc->def_node < 0) continue;
    for (int id = 0; id < g->nt->count; id++) {
      NodeKind k = nt_kind(g->nt, id);
      if (k != NK_InstanceVariableReadNode && k != NK_InstanceVariableWriteNode &&
          k != NK_InstanceVariableOperatorWriteNode) continue;
      if (comp_scope_of(g->c, id) == sc) return 1;
    }
  }
  return 0;
}

/* True when the program contains a method call on a POLY (union) receiver whose
   name is neither a user method on a poly receiver, nor a name the emitter's
   builtin allowlist / receiver-typed table resolves. Such a call currently slips
   through the "a poly receiver defers to dynamic dispatch" rule and build-fails
   (`String#each` on `(Range | String)`). A program that has one is left refused
   where it was, instead of being unblocked by an unrelated feature (a `yield`
   method) and then failing to build. */
static int cr_has_unverified_poly_call(CrGen *g) {
  for (int id = 0; id < g->nt->count; id++) {
    if (nt_kind(g->nt, id) != NK_CallNode) continue;
    int recv = nt_ref(g->nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = comp_ntype(g->c, recv);
    /* A true UNION (a branch result like `[0..2, "x"][0]`, typed `(Range |
       String)`) reaches Crystal's static union dispatch, which fails when one
       arm lacks the method. TY_POLY is different: it is the boxed SpValue, whose
       `sp_send` answers the method (or raises NoMethodError like Ruby), so it is
       left alone. Only a non-poly type with no concrete spelling is gated. */
    if (rt == TY_POLY) continue;
    Buf probe; memset(&probe, 0, sizeof probe);
    int concrete = cr_map_type(g, rt, &probe);
    free(probe.p);
    if (concrete) continue;
    const char *nm = nt_str(g->nt, id, "name");
    if (!nm) continue;
    if (sp_streq(nm, "[]") || sp_streq(nm, "[]=") || sp_streq(nm, "!") ||
        cr_is_binop(nm)) continue;
    if (sp_streq(nm, "to_s") || sp_streq(nm, "inspect") || sp_streq(nm, "nil?") ||
        sp_streq(nm, "==") || sp_streq(nm, "!=") || sp_streq(nm, "hash") ||
        sp_streq(nm, "class")) continue;
    if (cr_user_method_ok(g, nm, TY_POLY, 0)) continue;
    /* Only the BUILTIN-CONTAINER iterators/predicates fail on a mixed union
       (`(Range | String)#each`, `(Array | String)#join`). Restrict the gate to
       those names so a union of USER classes (`(A | B)#id`, which Crystal
       dispatches fine) is not refused. */
    static const char *const container_meths[] = {
      "each", "each_with_index", "each_pair", "each_index", "map", "flat_map",
      "select", "reject", "filter", "join", "zip", "values_at", "values",
      "members", "sum", "reduce", "inject", "count", "min", "max", "sort",
      "sort_by", "find", "detect", "any?", "all?", "first", "last", "index",
      "include?", "to_h", "to_a", "group_by", "each_slice", "each_cons", NULL };
    for (int i = 0; container_meths[i]; i++)
      if (sp_streq(nm, container_meths[i])) return 1;
  }
  return 0;
}

/* True when a superclass in `ci`'s chain already declares `name`. Now that the
   emitted header carries the superclass, a subclass does NOT re-declare an
   inherited instance variable: Crystal rejects a re-declaration at a different type
   ("instance variable '@tag' of Node, with Sub < Node, is already declared as
   Int64"). This matters for the nilable seeding in cr_emit_class -- a subclass with
   no `initialize` of its own never assigns the ivar, so the seeding would widen an
   INHERITED declaration to `T?`. Before the superclass was emitted the subclass was
   an unrelated class, so the re-declaration was harmless and the bug was invisible. */
static int cr_class_inherits_ivar(CrGen *g, ClassInfo *ci, const char *name) {
  if (!ci || !name) return 0;
  for (int p = cr_emitted_parent(g, ci); p >= 0;
       p = cr_emitted_parent(g, &g->c->classes[p])) {
    ClassInfo *pc = &g->c->classes[p];
    for (int i = 0; i < pc->nivars; i++)
      if (sp_streq(pc->ivars[i], name)) return 1;
  }
  return 0;
}

/* True when the class body itself defines `initialize` (the scan cr_emit_class
   did inline, factored out so the inherited-constructor check can run it per
   ancestor). */
static int cr_class_init_in_body(CrGen *g, ClassInfo *ci) {
  if (!ci || ci->def_node < 0) return 0;
  const char *cty = nt_type(g->nt, ci->def_node);
  if (!cty || !(sp_streq(cty, "ClassNode") || sp_streq(cty, "ModuleNode"))) return 0;
  int cbody0 = nt_ref(g->nt, ci->def_node, "body");
  if (cbody0 < 0) return 0;
  int nn = 0; const int *aa = nt_arr(g->nt, cbody0, "body", &nn);
  for (int i = 0; i < nn; i++)
    if (nt_kind(g->nt, aa[i]) == NK_DefNode &&
        sp_streq(nt_str(g->nt, aa[i], "name") ? nt_str(g->nt, aa[i], "name") : "", "initialize"))
      return 1;
  return 0;
}

/* True when `initialize` assigns `@name` on EVERY path, i.e. a top-level
   statement of its body is a plain write to that ivar. A write nested inside an
   if/unless/while/case/rescue/begin does not count: Ruby leaves the ivar nil on
   the paths that skip it, so the Crystal declaration must be nilable or the class
   is rejected ("must be Combat, not Nil"). A class with no `initialize` at all
   assigns nothing here; the caller covers that with the synthesized seed. */
static int cr_ivar_assigned_in_init(CrGen *g, ClassInfo *ci, const char *name) {
  if (!ci || !name || ci->def_node < 0) return 0;
  const char *cty = nt_type(g->nt, ci->def_node);
  if (!cty || !(sp_streq(cty, "ClassNode") || sp_streq(cty, "ModuleNode"))) return 0;
  int cbody = nt_ref(g->nt, ci->def_node, "body");
  if (cbody < 0) return 0;
  int nn = 0; const int *aa = nt_arr(g->nt, cbody, "body", &nn);
  int init = -1;
  for (int i = 0; i < nn; i++)
    if (nt_kind(g->nt, aa[i]) == NK_DefNode &&
        sp_streq(nt_str(g->nt, aa[i], "name") ? nt_str(g->nt, aa[i], "name") : "", "initialize"))
      init = aa[i];
  if (init < 0) return 0;
  int body = nt_ref(g->nt, init, "body");
  if (body < 0) return 0;
  int bn = 0; const int *bb = nt_arr(g->nt, body, "body", &bn);
  for (int i = 0; i < bn; i++) {
    if (nt_kind(g->nt, bb[i]) != NK_InstanceVariableWriteNode) continue;
    const char *w = nt_str(g->nt, bb[i], "name");
    if (w && sp_streq(w, name)) return 1;
  }
  return 0;
}

/* True when some visible call site of scope `s` OMITS positional parameter `pi`,
   so a `= nil` default there is really instantiated. `initialize` is reached
   through `K.new` on a constant naming the class or a subclass under it -- the
   same reachability rule the analyzer's param-supplied scan uses; a `new` on a
   dynamic receiver (a class value) is invisible here, which is the documented
   class-value limitation, not a refusal this guard can see. Crystal only
   rejects a nil default against a concrete restriction when the default is
   instantiated, so a param every call site supplies stays emittable. */
static int cr_param_omitted_anywhere(CrGen *g, Scope *s, int pi) {
  const NodeTable *nt = g->nt;
  if (!s->name) return 0;
  int is_init = sp_streq(s->name, "initialize");
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    if (!sp_streq(nm, s->name) && !(is_init && sp_streq(nm, "new"))) continue;
    if (is_init && sp_streq(nm, "new")) {
      int recv = nt_ref(nt, id, "receiver");
      NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_CallNode;
      if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) continue;
      const char *rn = nt_str(nt, recv, "name");
      int rci = rn ? comp_class_index(g->c, rn) : -1;
      if (rci < 0 || s->class_id < 0) continue;
      int under = 0;
      for (int k = rci; k >= 0 && k < g->c->nclasses; k = g->c->classes[k].parent)
        if (k == s->class_id) { under = 1; break; }
      if (!under) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    int n = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
    int pos = 0, unknown = 0;
    for (int k = 0; k < n; k++) {
      NodeKind ak = nt_kind(nt, av[k]);
      if (ak == NK_SplatNode) { unknown = 1; break; }
      const char *aty = nt_type(nt, av[k]);
      if (aty && sp_streq(aty, "ForwardingArgumentsNode")) { unknown = 1; break; }
      if (ak == NK_BlockArgumentNode) continue;
      pos++;
    }
    if (unknown) continue;   /* a spread could supply it -- do not claim omission */
    if (pos <= pi) return 1;
  }
  return 0;
}

/* The index of the LAST statement in a class body that (re)defines method
   `name` (writers spelled "x="): a DefNode with that name, or an
   attr_reader/attr_writer/attr_accessor call declaring it. Ruby's rule is
   last-definition-wins, but the emitter writes the body's defs first and the
   attr defs after, so each side must check it is the winner before emitting:
   `attr_reader :x` followed by a later `def x` must run the DEF, not the
   reader (and the reverse order must run the reader). Returns -1 when this
   body defines `name` nowhere (an inherited accessor copy the analyzer put in
   readers[]/writers[] is not a body statement). */
static int cr_body_last_defines(const NodeTable *nt, const int *arr, int n,
                                const char *name) {
  if (!name) return -1;
  size_t nl = strlen(name);
  int last = -1;
  for (int i = 0; i < n; i++) {
    NodeKind kk = nt_kind(nt, arr[i]);
    if (kk == NK_DefNode) {
      /* `def self.x` has a receiver: it defines a CLASS method, which no attr
         call ever replaces, so it is not part of the instance-name contest. */
      if (nt_ref(nt, arr[i], "receiver") >= 0) continue;
      const char *dn = nt_str(nt, arr[i], "name");
      if (dn && sp_streq(dn, name)) last = i;
    } else if (kk == NK_CallNode) {
      const char *cn = nt_str(nt, arr[i], "name");
      if (!cn) continue;
      int reader = sp_streq(cn, "attr_reader") || sp_streq(cn, "attr_accessor") ||
                   sp_streq(cn, "attr") || sp_streq(cn, "attribute") ||
                   sp_streq(cn, "attributes");
      int writer = sp_streq(cn, "attr_writer") || sp_streq(cn, "attr_accessor");
      if (!reader && !writer) continue;
      int args = nt_ref(nt, arr[i], "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      for (int a = 0; a < an; a++) {
        if (nt_kind(nt, av[a]) != NK_SymbolNode) continue;
        const char *base = nt_str(nt, av[a], "value");
        if (!base) continue;
        if (reader && sp_streq(base, name)) last = i;
        if (writer && nl >= 2 && name[nl - 1] == '=' &&
            strlen(base) == nl - 1 && strncmp(base, name, nl - 1) == 0) last = i;
      }
    }
  }
  return last;
}

/* ---- classes ---- */
static void cr_emit_class(CrGen *g, ClassInfo *ci, Buf *b, int is_module) {
  g->nclasses++;
  /* No user class inherits SpObject in P21: a widened array holds only scalars,
     which live in SpValue's Raw union directly, so nothing needs the object base.
     Giving every class `< SpObject` widens its type to `SpObject+`, which broke
     inference on value-in-collection dispatch (test_poly_return_value_class /
     test_cls_value_in_collection_dispatch). P22 reintroduces it together with the
     callee-body work that makes a boxed object actually dispatch. */
  /* A user class whose name is a Crystal stdlib generic (`Box`, `Tuple`,
     `Array`, `Set`, `Range`, `Proc`, ...) collides: Crystal auto-parameterizes
     the reference or rejects the definition outright ("Tuple is not a class"),
     and the emitted file fails with a message about the stdlib type. Name the
     collision at the class so the diagnostic points here rather than there. A
     module name is safe (`module Box` is a plain module). */
  if (!is_module) {
    static const char *const CR_STDLIB_GENERICS[] = {
      "Box", "Tuple", "StaticArray", "Pointer", "Slice", "Array", "Hash",
      "Set", "Deque", "Channel", "Proc", "Iterator", "Range", "Comparable",
      "Enumerable", "Indexable", "Iterable", "Union", "NamedTuple", "Regex",
      "Time", "Mutex", "Fiber", "Thread", "Exception", NULL };
    for (int i = 0; CR_STDLIB_GENERICS[i]; i++)
      if (sp_streq(ci->name, CR_STDLIB_GENERICS[i])) {
        char why[160];
        snprintf(why, sizeof why, "a class named '%s' (a Crystal stdlib type of "
                 "the same name is a generic and cannot be reopened)", ci->name);
        cr_mark_unsupported(g, ci->def_node, why);
      }
  }
  /* Carry the superclass into the emitted header. `ClassInfo.parent` is the
     superclass index the compiler already tracks, but the Crystal emitter never
     wrote it, so `class B < A` emitted as a bare `class B` -- B did not inherit A's
     methods, so every program that called one through a child was a BUILDFAIL that
     no amount of dynamic dispatch could have fixed (the receiver is statically
     concrete; there is no runtime union to dispatch over). Only a parent that is
     itself an emitted user class is carried: a builtin/native parent is left off,
     since `< Object` would drag in a superclass the emitter does not model. */
  {
    int par = is_module ? -1 : cr_emitted_parent(g, ci);
    if (par >= 0) buf_printf(b, "%s %s < %s\n", "class", ci->name,
                             g->c->classes[par].name);
    else         buf_printf(b, "%s %s\n", is_module ? "module" : "class", ci->name);
  }
  /* typed ivar declarations (classes only; modules carry none). Crystal needs the
     annotation: a bare `@d` in a class body parses as a read, not a declaration,
     and is rejected as an instance variable at the top level. An ivar whose type
     the inference left poly has no annotation to write, so refuse it. */
  /* Crystal requires every declared ivar to be assigned directly in each
     `initialize`, else the class fails to compile ("not initialized in all of
     the 'initialize' methods, rendering it nilable"). A class whose ivar is
     assigned only outside `initialize` -- or a class with no `initialize` -- has
     no such assignment, so emit a synthesized `initialize` that seeds each ivar
     to nil and declare it nilable (`T?`). `uninitialized T` would compile but
     leave a reference ivar unreadable (a later read that Ruby answers nil
     segfaults), so nil is the only safe seed. */
  int has_init = cr_class_init_in_body(g, ci);
  /* A subclass with no `initialize` of its own INHERITS the superclass's -- the
     emitted header carries it (P22n), so Crystal resolves `.new(args)` onto it.
     Synthesizing a no-arg seed here would SHADOW that inherited constructor:
     `Derived.new(9)` build-fails "given 1 argument, expected 0" (the P22m
     follow-on, P22o). Only when no class in the emitted chain defines
     `initialize` is the seed needed: Ruby's default constructor takes no args
     and leaves every ivar nil. */
  int inh_init = 0;
  for (int p = cr_emitted_parent(g, ci); p >= 0;
       p = cr_emitted_parent(g, &g->c->classes[p])) {
    ClassInfo *pc = &g->c->classes[p];
    /* a Struct ancestor gets the synthesized positional constructor emitted
       below; its "body" is the Struct.new ConstantWriteNode, which the body
       scan cannot see. (has_init for the struct ITSELF deliberately stays a
       body-only scan: it gates the poly-member boxing path, which must not
       change.) */
    if (cr_class_init_in_body(g, pc) || (pc->is_struct && !pc->is_data)) { inh_init = 1; break; }
  }
  int synth_init = !is_module && !ci->is_struct && !has_init && !inh_init && ci->nivars > 0;

  int all_ivars_typed = 1;
  if (!is_module)
    for (int i = 0; i < ci->nivars; i++) {
      /* A declared ivar is nilable unless `initialize` assigns it
         UNCONDITIONALLY. A class with no initialize is covered by the synthesized
         seed; a class with one that assigns the ivar only inside a conditional
         (`@combat = Combat.new if armed`) leaves it nil on the other path, which
         is exactly Ruby's behaviour -- and Ruby reads such an ivar as nil, so the
         declaration must be `T?` or Crystal rejects the class:
         "instance variable '@combat' of Player must be Combat, not Nil". Declaring
         it `Combat` was P22e's gap. Only TOP-LEVEL statements of the initialize
         body count: a write nested in an if/unless/while/case/rescue does not
         cover every path. */
      int inherited = cr_class_inherits_ivar(g, ci, ci->ivars[i]);
      int nilable = synth_init ||
                    (!ci->is_struct && !inherited &&
                     !cr_ivar_assigned_in_init(g, ci, ci->ivars[i]));
      Buf tmp; memset(&tmp, 0, sizeof tmp);
      if (cr_map_type(g, ci->ivar_types[i], &tmp)) {
        if (inherited) continue;   /* the superclass already declares it */
        buf_printf(b, "  %s : %s%s\n", ci->ivars[i], tmp.p, nilable ? "?" : "");
        free(tmp.p);
      } else if (CR_POLY_IVARS && cr_is_poly(ci->ivar_types[i]) && !has_init &&
                 cr_ivar_reads_safe(g, ci, ci->ivars[i])) {
        /* A widened ivar is the boxed runtime value; every write boxes through
           `sp_box` (see the ivar-write path) and a concrete read unboxes back
           (the ivar-read path). The value flows boxed everywhere it is only
           forwarded, which is the dominant shape. */
        buf_printf(b, "  %s : SpValue%s\n", ci->ivars[i], nilable ? "?" : "");
        free(tmp.p);
      } else {
        free(tmp.p);
        all_ivars_typed = 0;
        cr_mark_unsupported(g, ci->def_node,
                            "an instance variable with a poly/unknown type: Crystal "
                            "requires a declared ivar type and has no untyped type");
        continue;
      }
    }

  if (synth_init) {
    int any = 0;
    for (int i = 0; i < ci->nivars; i++) {
      Buf tmp; memset(&tmp, 0, sizeof tmp);
      int concrete = cr_map_type(g, ci->ivar_types[i], &tmp);
      free(tmp.p);
      if (concrete || (CR_POLY_IVARS && cr_is_poly(ci->ivar_types[i]))) any = 1;
    }
    if (any) {
      buf_puts(b, "  def initialize\n");
      for (int i = 0; i < ci->nivars; i++) {
        Buf tmp; memset(&tmp, 0, sizeof tmp);
        int concrete = cr_map_type(g, ci->ivar_types[i], &tmp);
        free(tmp.p);
        if (concrete || (CR_POLY_IVARS && cr_is_poly(ci->ivar_types[i])))
          buf_printf(b, "    %s = nil\n", ci->ivars[i]);
      }
      buf_puts(b, "  end\n");
    }
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

    /* Struct feature surface: to_a/values gather the member values in declaration
       order; .members lists them as symbols; to_h pairs each symbol with its value
       (the => form, so it is a real Hash, not a NamedTuple). Emitted for every pure
       Struct with at least one typed member so any of them can be called. Data is
       excluded (a value type with a different API). */
    if (ci->nivars > 0) {
      for (int which = 0; which < 2; which++) {
        buf_printf(b, "  def %s\n    [", which == 0 ? "to_a" : "values");
        for (int i = 0; i < ci->nivars; i++)
          buf_printf(b, "%s%s", i ? ", " : "", ci->ivars[i]);
        buf_puts(b, "]\n  end\n");
      }
      buf_puts(b, "  def self.members\n    [");
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "%s:%s", i ? ", " : "", ci->ivars[i] + 1);
      buf_puts(b, "]\n  end\n");
      buf_puts(b, "  def to_h\n    { ");
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "%s:%s => %s", i ? ", " : "", ci->ivars[i] + 1, ci->ivars[i]);
      buf_puts(b, " }\n  end\n");

      /* == is member-wise (Ruby Struct equality), not Crystal's default identity.
         Typed to the class so `a == b` dispatches here while `a == nil` / a foreign
         type still falls through to Object#== (identity) -- matching Ruby's answer. */
      buf_printf(b, "  def ==(other : %s)\n", ci->name);
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "    return false unless %s == other.%s\n", ci->ivars[i], ci->ivars[i] + 1);
      buf_puts(b, "    true\n  end\n");

      /* [] reads a member by symbol, string name, or integer position (negative
         counts from the end). Emitted as bracket syntax at the call site, so no
         allowlist entry is needed. The result is the union of the member types. */
      buf_puts(b, "  def [](key : Symbol)\n    case key\n");
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "    when %s:%s then %s\n", "", ci->ivars[i] + 1, ci->ivars[i]);
      buf_puts(b, "    else raise IndexError.new(\"no member\")\n    end\n  end\n");
      buf_puts(b, "  def [](key : String)\n    case key\n");
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "    when \"%s\" then %s\n", ci->ivars[i] + 1, ci->ivars[i]);
      buf_puts(b, "    else raise IndexError.new(\"no member\")\n    end\n  end\n");
      buf_printf(b, "  def [](key : Int64)\n    i = key < 0 ? key + %d : key\n", ci->nivars);
      buf_printf(b, "    if i < 0 || i >= %d; raise IndexError.new(\"bad index\"); end\n", ci->nivars);
      buf_puts(b, "    case i\n");
      for (int i = 0; i < ci->nivars; i++)
        buf_printf(b, "    when %d then %s\n", i, ci->ivars[i]);
      buf_puts(b, "    else raise IndexError.new(\"bad index\")\n    end\n  end\n");

      /* []= writes a member. One overload set per DISTINCT member type, whose case
         arms list only that type's members, so a heterogeneous struct still gets a
         cast-free signature (no union, no .as): `s[:n] = "x"` fails to resolve when
         :n is an Int member and the program build-fails rather than storing a value
         of the wrong type. Members are grouped by their MAPPED Crystal spelling so
         Int32 and Int64 members share one overload. Each arm ends in the assignment,
         so the setter answers the stored value (Ruby's `s[k] = v` value). */
      for (int gi = 0; gi < ci->nivars; gi++) {
        Buf t; memset(&t, 0, sizeof t);
        cr_map_type(g, ci->ivar_types[gi], &t);         /* known to succeed */
        int seen = 0;
        for (int k = 0; k < gi; k++) {
          Buf p; memset(&p, 0, sizeof p);
          cr_map_type(g, ci->ivar_types[k], &p);
          if (sp_streq(p.p, t.p)) seen = 1;
          free(p.p);
        }
        if (seen) { free(t.p); continue; }               /* already emitted */

        buf_printf(b, "  def []=(key : Symbol, value : %s)\n    case key\n", t.p);
        for (int i = 0; i < ci->nivars; i++) {
          Buf m; memset(&m, 0, sizeof m);
          cr_map_type(g, ci->ivar_types[i], &m);
          if (sp_streq(m.p, t.p))
            buf_printf(b, "    when :%s then %s = value\n", ci->ivars[i] + 1, ci->ivars[i]);
          free(m.p);
        }
        buf_puts(b, "    else raise IndexError.new(\"no member\")\n    end\n  end\n");

        buf_printf(b, "  def []=(key : String, value : %s)\n    case key\n", t.p);
        for (int i = 0; i < ci->nivars; i++) {
          Buf m; memset(&m, 0, sizeof m);
          cr_map_type(g, ci->ivar_types[i], &m);
          if (sp_streq(m.p, t.p))
            buf_printf(b, "    when \"%s\" then %s = value\n", ci->ivars[i] + 1, ci->ivars[i]);
          free(m.p);
        }
        buf_puts(b, "    else raise IndexError.new(\"no member\")\n    end\n  end\n");

        buf_printf(b, "  def []=(key : Int64, value : %s)\n", t.p);
        buf_printf(b, "    i = key < 0 ? key + %d : key\n", ci->nivars);
        buf_printf(b, "    if i < 0 || i >= %d; raise IndexError.new(\"bad index\"); end\n", ci->nivars);
        buf_puts(b, "    case i\n");
        for (int i = 0; i < ci->nivars; i++) {
          Buf m; memset(&m, 0, sizeof m);
          cr_map_type(g, ci->ivar_types[i], &m);
          if (sp_streq(m.p, t.p))
            buf_printf(b, "    when %d then %s = value\n", i, ci->ivars[i]);
          free(m.p);
        }
        buf_puts(b, "    else raise IndexError.new(\"bad index\")\n    end\n  end\n");
        free(t.p);
      }
    }
  }

  /* The class's own instance methods, gathered for the dynamic-dispatch method
     emitted below (only when the poly runtime is on). */
  Scope *dmeth[256]; int ndmeth = 0;

  /* methods: walk the body's DefNodes (and class-level constants) */
  int cbody = -1;
  if (ci->def_node >= 0) {
    const char *cty = nt_type(g->nt, ci->def_node);
    if (cty && (sp_streq(cty, "ClassNode") || sp_streq(cty, "ModuleNode")))
      cbody = nt_ref(g->nt, ci->def_node, "body");
    else if (ci->is_struct && !ci->is_data && cty && sp_streq(cty, "ConstantWriteNode")) {
      /* block-form: Point = Struct.new(:x) do def m; end end -- the methods live
         in the call's block body (the analyzer fixes their class_id to this one). */
      int val = nt_ref(g->nt, ci->def_node, "value");
      int blk = (val >= 0 && nt_kind(g->nt, val) == NK_CallNode) ? nt_ref(g->nt, val, "block") : -1;
      if (blk >= 0) {
        int bb = nt_ref(g->nt, blk, "body");
        if (bb >= 0 && sp_streq(nt_type(g->nt, bb), "StatementsNode")) cbody = bb;
      }
    }
  }
  int bn = 0; const int *barr = NULL;
  if (cbody >= 0) {
    int n = 0; const int *arr = nt_arr(g->nt, cbody, "body", &n);
    bn = n; barr = arr;
    for (int i = 0; i < n; i++) {
      NodeKind kk = nt_kind(g->nt, arr[i]);
      if (kk == NK_DefNode) {
        Scope *s = cr_scope_for_def(g->c, arr[i]);
        /* Ruby: last definition wins. An attr declared LATER in this same body
           replaces an earlier def of the same name, so the def is skipped (the
           attr's def is emitted after the body and takes the win). A `def
           self.x` cmethod is not in that contest -- attrs only ever define
           instance methods -- so it always emits. */
        int win = (s && s->name && !s->is_cmethod) ? cr_body_last_defines(g->nt, arr, n, s->name) : i;
        if (s && win == i) {
          cr_emit_method(g, s, b);
          if ((CR_POLY_DISPATCH || CR_POLY_LOCALS) && !s->is_cmethod && s->name && ndmeth < 256) dmeth[ndmeth++] = s;
        }
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
      } else if (kk == NK_CallNode && cr_visibility_decl(g, arr[i])) {
        /* `module_function`, `private`/`public`/`protected` (bare or listing
           symbols), and `extend self`/`extend M`: declaration-only statements
           whose effect the analyzer already captured (is_module_function,
           include membership, method resolution). Crystal does not need the
           marker for a method to be callable, and its own `private`/`module_function`
           have different scoping, so skip the statement rather than mistranslate
           it. A `private_constant`/`protected` with a non-symbol argument, or any
           other call, is still refused below. */
        if (is_module && cr_module_function_self_dependent(g, ci, is_module))
          cr_mark_unsupported(g, arr[i],
                              "a module_function method that reads or writes an "
                              "instance variable on a module an includer mixes in "
                              "(CRuby runs it on the instance, Crystal on the module)");
        continue;
      } else if (kk == NK_CallNode && cr_include_decl(g, arr[i], b)) {
        /* `include M` / `include M, N`: Crystal's own `include` mixin needs the
           module's methods to already be Crystal type members, and a builtin
           mixin like Comparable needs a type argument (`Comparable(T)`) that
           changes its method surface. The analyzer has already transplanted the
           module's methods into this class, so the statement itself is skipped
           -- the marker was the analysis input, not an emitted behaviour. */
        continue;
      } else {
        /* Any other statement (a call with side effects, a local write, ...) runs
           at definition time in Ruby. The two-pass emission hoists types before
           top-level code, so it would run out of order -- refuse rather than
           silently reorder the program's output. */
        cr_mark_unsupported(g, arr[i], "a side-effecting statement in a class/module body");
      }
    }
  }

  /* attr readers / writers (resolved by the analyzer into ci->readers/writers).
     A same-named def LATER in this body wins in Ruby, so check which side is
     the last definition before emitting the attr's def. */
  for (int i = 0; i < ci->nreaders; i++) {
    int win = cr_body_last_defines(g->nt, barr, bn, ci->readers[i]);
    if (win >= 0 && nt_kind(g->nt, barr[win]) == NK_DefNode) continue;
    buf_printf(b, "  def %s\n    @%s\n  end\n", ci->readers[i], ci->readers[i]);
  }
  for (int i = 0; i < ci->nwriters; i++) {
    char wname[128];
    snprintf(wname, sizeof wname, "%s=", ci->writers[i]);
    int win = cr_body_last_defines(g->nt, barr, bn, wname);
    if (win >= 0 && nt_kind(g->nt, barr[win]) == NK_DefNode) continue;
    buf_printf(b, "  def %s=(v)\n    @%s = v\n  end\n", ci->writers[i], ci->writers[i]);
  }

  /* Dynamic dispatch: a widened value whose run-time kind is this class answers
     a call through SpObject#sp_send. Each user instance method gets an arm that
     unboxes its positional arguments and boxes the result, so a boxed caller
     sees the same value a typed call would. Staged with the poly runtime. */
  if ((CR_POLY_DISPATCH || CR_POLY_LOCALS) && !is_module && !ci->is_struct &&
      (ndmeth > 0 || ci->nreaders > 0 || ci->nwriters > 0)) {
    buf_puts(b, "  def sp_send(name : String, args : Array(SpValue)) : SpValue\n    case name\n");
    for (int i = 0; i < ndmeth; i++) {
      Scope *s = dmeth[i];
      int params = s->def_node >= 0 ? nt_ref(g->nt, s->def_node, "parameters") : -1;
      int reqn = 0; const int *reqs = NULL;
      if (params >= 0 && sp_streq(nt_type(g->nt, params), "ParametersNode"))
        reqs = nt_arr(g->nt, params, "requireds", &reqn);
      if (params >= 0 && nt_ref(g->nt, params, "block") >= 0) continue;
      if (sp_streq(s->name, "to_s") || sp_streq(s->name, "inspect")) continue;
      /* `self.` is required for an operator method: a bare `==(args[0])` is
         parsed as the start of a call and rejected as "unterminated call". */
      buf_printf(b, "    when \"%s\"\n      sp_box(self.%s(", s->name, s->name);
      for (int a = 0; a < reqn; a++) {
        const char *pn = nt_str(g->nt, reqs[a], "name");
        LocalVar *lv = pn ? scope_local(s, pn) : NULL;
        TyKind pt = lv ? lv->type : TY_UNKNOWN;
        if (a) buf_puts(b, ", ");
        buf_printf(b, "args[%d]", a);
        if (pt == TY_INT)         buf_puts(b, ".to_i");
        else if (pt == TY_FLOAT)  buf_puts(b, ".to_f");
        else if (pt == TY_STRING || pt == TY_STRBUF) buf_puts(b, ".to_s");
      }
      buf_puts(b, "))\n");
    }
    for (int i = 0; i < ci->nreaders; i++)
      buf_printf(b, "    when \"%s\"\n      sp_box(%s)\n", ci->readers[i], ci->readers[i]);
    for (int i = 0; i < ci->nwriters; i++) {
      buf_printf(b, "    when \"%s=\"\n      %s = ", ci->writers[i], ci->writers[i]);
      TyKind wt = TY_UNKNOWN;
      for (int v = 0; v < ci->nivars; v++)
        if (ci->ivars[v] && sp_streq(ci->ivars[v] + 1, ci->writers[i])) wt = ci->ivar_types[v];
      if (wt == TY_INT) buf_puts(b, "args[0].to_i");
      else if (wt == TY_FLOAT) buf_puts(b, "args[0].to_f");
      else if (wt == TY_STRING) buf_puts(b, "args[0].to_s");
      else buf_puts(b, "args[0]");
      buf_puts(b, "\n      sp_box(nil)\n");
    }
    buf_puts(b, "    else raise ArgumentError.new(\"undefined method '#{name}'\")\n    end\n  end\n");
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

/* P22a: the per-class `sp_box` overload. A user object is boxed into the value
   union through SpBox rather than by the object's class inheriting SpObject
   (which widened a union of classes to `SpObject+` and erased the dispatch arms
   -- see P22). Emitted only for a class Pass 1 actually emits as a Crystal type,
   and only for a top-level one (a nested class's name is qualified); a module,
   native class or builtin reopen has no single Crystal type to name. Additive:
   nothing calls these until the boxing sites land. */
static void cr_emit_sp_box_overload(ClassInfo *ci, Buf *b, int is_module) {
  if (is_module || ci->enclosing_class != -1 || ci->is_native_class ||
      is_builtin_reopen(ci->name)) return;
  buf_printf(b, "def sp_box(v : %s) : SpValue\n  SpValue.new(SpBox.new(v))\nend\n\n", ci->name);
}

/* One `sp_box_send` arm per top-level user class: the dynamic-dispatch answer for a
   BOXED receiver, reached from `SpBox#sp_send`. The box does not know T's methods, so
   it hands off here, and overload resolution picks the concrete `SpBox(Foo)` at each
   instantiation. That is what lets two arms of ONE union spell the method differently
   and still both resolve -- the `(Tags | Array(String))#include?` shape that no single
   renamed call could ever reach (P22k refuses it for exactly that reason).

   Only ZERO-argument PUBLIC instance methods get an arm. A method with parameters
   would need its arguments boxed and unboxed per arm, and getting that wrong yields a
   silent wrong answer rather than a build error, so it is left to the fallback, which
   refuses the way Ruby's NoMethodError-shaped raise would. A private method is also
   left out: `recv.private_m` is a NoMethodError in Ruby.

   Corpus note: this arm set is currently INERT -- no emitted program routes a receiver
   through `sp_send` (that gate needs CR_POLY_DISPATCH or a boxed local, and both are
   off), and a stdlib container boxed in the union has no arm either. It is emitted to
   close a footgun rather than to move the corpus: before it, `sp_box(obj).sp_send(..)`
   compiled and then raised at RUN time, which cost P22l three PASS programs. */
static void cr_emit_sp_box_send_overload(Compiler *c, ClassInfo *ci, Buf *b,
                                        int is_module) {
  if (is_module || ci->enclosing_class != -1 || ci->is_native_class ||
      is_builtin_reopen(ci->name)) return;
  int cidx = -1;
  for (int i = 0; i < c->nclasses; i++)
    if (&c->classes[i] == ci) { cidx = i; break; }
  if (cidx < 0) return;

  const char *arms[64];
  int narms = 0;
  /* Walk `ci` AND its superclass chain: the arm is keyed on the box's CONCRETE type,
     so a `SpBox(Derived)` must answer a method `Derived` only INHERITS from `Base`.
     Collecting only the class's own scopes gave a subclass no arm at all -- the same
     missing-relation trap P22n removed for the class header. A nearer definition
     wins, so an inherited name already collected is not repeated. */
  for (int anc = cidx; anc >= 0 && anc < c->nclasses; ) {
    ClassInfo *ac = &c->classes[anc];
    for (int s = 1; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      if (sc->class_id != anc || sc->is_cmethod) continue;
      if (!sc->name || !sc->name[0]) continue;
      if (strncmp(sc->name, "__prep_", 7) == 0) continue;   /* compiler shadow helper */
      if (sc->cs_synth) continue;                           /* synthesized method */
      /* Zero-argument only: no positional params, no rest, no post-rest, no keywords,
         and no block parameter (a block-taking method needs the proc protocol). */
      if (sc->nparams != 0 || sc->rest_idx >= 0 || sc->npost_rest > 0 ||
          sc->kwrest_idx >= 0 || sc->blk_param) continue;
      /* Operators and the constructor have their own spellings; a dynamic name must
         not reach them. */
      if (cr_is_binop(sc->name) || sp_streq(sc->name, "==") ||
          sp_streq(sc->name, "!") || sp_streq(sc->name, "initialize")) continue;
      int dup = 0;
      for (int k = 0; k < narms; k++) if (sp_streq(sc->name, arms[k])) { dup = 1; break; }
      if (dup) continue;
      if (narms >= (int)(sizeof arms / sizeof arms[0])) break;
      arms[narms++] = sc->name;
    }
    anc = ac->parent;
  }
  if (!narms) return;

  Buf body; memset(&body, 0, sizeof body);
  for (int k = 0; k < narms; k++)
    buf_printf(&body, "    when \"%s\" then sp_box(v.val.%s())\n", arms[k], arms[k]);

  buf_printf(b, "def sp_box_send(v : SpBox(%s), name : String, args : Array(SpValue)) : SpValue\n",
             ci->name);
  buf_puts(b, "  unless args.empty?\n");
  /* The `#{}` belong to the GENERATED Crystal, so they go through a format specifier:
     writing them into a printf format would interpolate the spinel-side `name` at
     generation time and bake in whatever it happened to be. */
  buf_printf(b, "    raise ArgumentError.new(\"%%s() given %%d arguments, expected 0\" %% "
                "{name, args.size})\n");
  buf_puts(b, "  end\n");
  buf_puts(b, "  case name\n");
  buf_puts(b, body.p);
  buf_puts(b, "    else raise ArgumentError.new(sp_box_undefined(v, name))\n"
              "  end\n"
              "end\n\n");
  free(body.p);
}

/* ---- entry point ---- */
char *codegen_program_crystal(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  CrGen g; memset(&g, 0, sizeof g);
  g.c = c; g.nt = nt;
  g.first_unsup = -1;
  g.value_ctx = 1;
  g.cur_class = -1;

  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "# Generated by Spinel (Crystal proof-of-concept backend)\n");
  buf_puts(&b, "# This file is machine-generated; do not edit.\n\n");
  /* The boxed poly-value runtime: SpValue + its scalar operations. Emitted
     before every user class so SpObject is a visible base and sp_box is in
     scope at every boxing site. */
  buf_puts(&b, CR_RUNTIME);
  /* CRuby's Hash#inspect / #to_s spelling. Crystal prints symbol keys as
     `{:a => 1}`; CRuby (4.0) prints `{a: 1}` for a plain-label key and
     `{"a-b": 1}` for one that is not, and `p` / `puts` / interpolation all go
     through these. The C backend already reproduces CRuby's spelling, so the
     Crystal form must too. A key counts as a label when it is a letter or `_`
     (or any non-ASCII codepoint) followed by letters/digits/`_`, with an
     optional trailing `?`/`!` -- operators, `@x`, `$y` and `1abc` stay quoted. */
  buf_puts(&b,
    "def sp_hash_label?(s : String) : Bool\n"
    "  return false if s.empty?\n"
    "  cs = s.chars\n"
    "  if cs.last == '?' || cs.last == '!'\n"
    "    return false if cs.size == 1\n"
    "    cs = cs[0...-1]\n"
    "  end\n"
    "  return false if cs.empty?\n"
    "  c0 = cs[0]\n"
    "  return false unless c0.ascii_letter? || c0 == '_' || c0.ord > 127\n"
    "  cs[1..].each do |c|\n"
    "    return false unless c.ascii_letter? || c.ascii_number? || c == '_' || c.ord > 127\n"
    "  end\n"
    "  true\n"
    "end\n"
    "class Hash\n"
    "  def inspect(io : IO) : Nil\n"
    "    io << '{'\n"
    "    first = true\n"
    "    each do |k, v|\n"
    "      io << \", \" unless first\n"
    "      first = false\n"
    "      if k.is_a?(Symbol) && sp_hash_label?(k.to_s)\n"
    "        io << k.to_s << \": \"\n"
    "      elsif k.is_a?(Symbol)\n"
    "        k.to_s.inspect(io); io << \": \"\n"
    "      else\n"
    "        k.inspect(io); io << \" => \"\n"
    "      end\n"
    "      v.inspect(io)\n"
    "    end\n"
    "    io << '}'\n"
    "  end\n"
    "  def to_s(io : IO) : Nil\n"
    "    inspect(io)\n"
    "  end\n"
    "end\n\n");
  /* CRuby renders a Regexp as `(?<on>[-]<off>:source)` from #to_s (and
     `"#{/re/}"`), in the flag order m,i,x with the enabled letters first, then
     `-`, then the disabled ones. Crystal prints `(?-imsx:...)` (its own flag set
     and order) and its slash-escaping differs. #inspect already matches
     (`/source/flags`), so only #to_s is replaced. #options answers the CRuby
     Integer bitmask (1=IGNORECASE, 2=EXTENDED, 4=MULTILINE) via ruby_options;
     the emitter rewrites `.options` to it, since Crystal's own `options` returns
     its Options enum and is used internally. */
  buf_puts(&b,
    "class Regex\n"
    "  def to_s(io : IO) : Nil\n"
    "    o = options\n"
    "    io << \"(?\"\n"
    "    io << 'm' if o.multiline?\n"
    "    io << 'i' if o.ignore_case?\n"
    "    io << 'x' if o.extended?\n"
    "    io << '-'\n"
    "    io << 'm' unless o.multiline?\n"
    "    io << 'i' unless o.ignore_case?\n"
    "    io << 'x' unless o.extended?\n"
    "    io << ':'\n"
    "    sp_regex_source(io)\n"
    "    io << ')'\n"
    "  end\n"
    "  def inspect(io : IO) : Nil\n"
    "    io << '/'\n"
    "    sp_regex_source(io)\n"
    "    io << '/'\n"
    "    o = options\n"
    "    io << 'm' if o.multiline?\n"
    "    io << 'i' if o.ignore_case?\n"
    "    io << 'x' if o.extended?\n"
    "  end\n"
    "  private def sp_regex_source(io : IO) : Nil\n"
    "    source.each_char do |c|\n"
    "      if c == '/'\n"
    "        io << '\\\\' << '/'\n"
    "      else\n"
    "        io << c\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  def ruby_options : Int32\n"
    "    o = options\n"
    "    (o.ignore_case? ? 1 : 0) | (o.extended? ? 2 : 0) | (o.multiline? ? 4 : 0)\n"
    "  end\n"
    "end\n\n");
  /* Ruby's Math.lgamma answers [log|Gamma(x)|, sign]; Crystal's answers the log
     alone (and has no lgamma_r). The sign is +1 for x > 0, -1 for x < 0 in the
     interval floor(x) odd (the sign of Gamma flips each unit interval), and +1
     at the non-positive integer poles (CRuby's convention); -0.0 is -1. */
  buf_puts(&b,
    "def sp_lgamma_sign(x : Float64) : Int32\n"
    "  return -1 if x == 0 && (1.0 / x) < 0\n"
    "  return 1 if x >= 0\n"
    "  f = x.floor\n"
    "  return 1 if f == x\n"
    "  (f.to_i % 2 == 0) ? 1 : -1\n"
    "end\n\n");
  /* Ruby's Integer#round/ceil/floor/truncate(ndigits): n >= 0 is self, n < 0
     rounds to 10**(-n) with EXACT integer arithmetic (a float form loses
     precision above 2**53, and round-half is away from zero). */
  buf_puts(&b,
    "def sp_int_round(x : Int64, n : Int64) : Int64\n"
    "  return x if n >= 0\n"
    "  d = 10_i64 ** (-n)\n"
    "  s = x < 0 ? -1_i64 : 1_i64\n"
    "  ((x.abs + d // 2) // d) * d * s\n"
    "end\n"
    "def sp_int_ceil(x : Int64, n : Int64) : Int64\n"
    "  return x if n >= 0\n"
    "  d = 10_i64 ** (-n)\n"
    "  -((-x) // d) * d\n"
    "end\n"
    "def sp_int_floor(x : Int64, n : Int64) : Int64\n"
    "  return x if n >= 0\n"
    "  d = 10_i64 ** (-n)\n"
    "  (x // d) * d\n"
    "end\n"
    "def sp_int_truncate(x : Int64, n : Int64) : Int64\n"
    "  return x if n >= 0\n"
    "  d = 10_i64 ** (-n)\n"
    "  x.tdiv(d) * d\n"
    "end\n\n");
  /* Ruby's Float#ceil/floor/truncate(ndigits). Crystal has Float#round(n) (which
     handles negative n) but no digit-taking ceil/floor/truncate, so scale,
     round, and scale back. The result is Float64; the emitter appends .to_i when
     CRuby returns Integer (a literal ndigits <= 0). */
  buf_puts(&b,
    "def sp_flt_ceil(x : Float64, n : Int64) : Float64\n"
    "  if n >= 0\n"
    "    f = 10.0 ** n\n"
    "    (x * f).ceil / f\n"
    "  else\n"
    "    f = 10.0 ** (-n)\n"
    "    (x / f).ceil * f\n"
    "  end\n"
    "end\n"
    "def sp_flt_floor(x : Float64, n : Int64) : Float64\n"
    "  if n >= 0\n"
    "    f = 10.0 ** n\n"
    "    (x * f).floor / f\n"
    "  else\n"
    "    f = 10.0 ** (-n)\n"
    "    (x / f).floor * f\n"
    "  end\n"
    "end\n"
    "def sp_flt_truncate(x : Float64, n : Int64) : Float64\n"
    "  if n >= 0\n"
    "    f = 10.0 ** n\n"
    "    (x * f).to_i / f\n"
    "  else\n"
    "    f = 10.0 ** (-n)\n"
    "    (x / f).to_i * f\n"
    "  end\n"
    "end\n\n");
  /* CRuby's String#split, ported from the C backend's sp_str_split_*: a
     single-space (or nil) separator is whitespace (awk) mode -- leading
     whitespace stripped, runs collapsed; the limit drops trailing empties
     (nil/0), caps fields with the unsplit remainder last (positive), or keeps
     trailing empties (negative). Crystal's own split differs on all three. */
  buf_puts(&b,
    "def sp_ws_char?(c : Char) : Bool\n"
    "  c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\f' || c == '\\v'\n"
    "end\n"
    "def sp_split_all(s : String, sep : String) : Array(String)\n"
    "  return [] of String if s.empty?\n"
    "  return s.chars.map(&.to_s) if sep.empty?\n"
    "  s.split(sep)\n"
    "end\n"
    "def sp_split_ws(s : String) : Array(String)\n"
    "  return [] of String if s.empty?\n"
    "  s.split\n"
    "end\n"
    "def sp_split_drop_trailing(s : String, sep : String) : Array(String)\n"
    "  return sp_split_ws(s) if sep == \" \"\n"
    "  a = sp_split_all(s, sep)\n"
    "  while !a.empty? && a.last.empty?\n"
    "    a.pop\n"
    "  end\n"
    "  a\n"
    "end\n"
    "def sp_split_ws_limit(s : String, n : Int32) : Array(String)\n"
    "  return sp_split_ws(s) if n == 0\n"
    "  a = [] of String\n"
    "  return a if s.empty?\n"
    "  return [s] if n == 1\n"
    "  cs = s.chars\n"
    "  i = 0\n"
    "  while i < cs.size && sp_ws_char?(cs[i])\n"
    "    i += 1\n"
    "  end\n"
    "  if i >= cs.size\n"
    "    a << \"\"\n"
    "    return a\n"
    "  end\n"
    "  k = 0\n"
    "  while i < cs.size\n"
    "    start = i\n"
    "    while i < cs.size && !sp_ws_char?(cs[i])\n"
    "      i += 1\n"
    "    end\n"
    "    a << cs[start...i].join\n"
    "    k += 1\n"
    "    sep_start = i\n"
    "    while i < cs.size && sp_ws_char?(cs[i])\n"
    "      i += 1\n"
    "    end\n"
    "    if i >= cs.size\n"
    "      a << \"\" if i > sep_start && (n < 0 || (n > 0 && k < n))\n"
    "      break\n"
    "    end\n"
    "    if n > 0 && k == n - 1\n"
    "      a << cs[i..].join\n"
    "      break\n"
    "    end\n"
    "  end\n"
    "  a\n"
    "end\n"
    "def sp_ruby_split(s : String, sep : String?, limit : Int64? = nil) : Array(String)\n"
    "  if sep.nil? || sep == \" \"\n"
    "    return limit.nil? ? sp_split_ws(s) : sp_split_ws_limit(s, limit.to_i32)\n"
    "  end\n"
    "  if limit.nil? || limit == 0\n"
    "    sp_split_drop_trailing(s, sep)\n"
    "  elsif limit < 0\n"
    "    sp_split_all(s, sep)\n"
    "  else\n"
    "    s.split(sep, limit.to_i32)\n"
    "  end\n"
    "end\n\n");

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
        if (ci >= 0) {
          cr_emit_class(&g, &c->classes[ci], &b, kk == NK_ModuleNode);
          cr_emit_sp_box_overload(&c->classes[ci], &b, kk == NK_ModuleNode);
          cr_emit_sp_box_send_overload(c, &c->classes[ci], &b, kk == NK_ModuleNode);
        }
      } else if (kk == NK_DefNode) {
        Scope *s = cr_scope_for_def(c, tstmts[k]);
        if (s) cr_emit_method(&g, s, &b);
      } else if (kk == NK_ConstantWriteNode) {
        int sci = cr_struct_def_ci(c, tstmts[k]);
        if (sci >= 0) {
          cr_emit_class(&g, &c->classes[sci], &b, 0);
          cr_emit_sp_box_overload(&c->classes[sci], &b, 0);
          cr_emit_sp_box_send_overload(c, &c->classes[sci], &b, 0);
        }
      }
    }
  }

  /* Pass 2: executable top-level statements, in source order. */
  ScopeMark saved = cr_push_scope(&g);
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
  free(g.poly_arrays.names);
  free(g.boxed_locals.names);
  comp_free(c);
  return b.p;
}
