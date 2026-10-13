# Crystal proof-of-concept backend

Spinel's pipeline is `parse → analyze → codegen`. The C backend is one
`codegen` target; this document records a **second, experimental target** that
emits [Crystal](https://crystal-lang.org/) instead of C. It exists to answer two
questions, not to replace the C backend:

1. Does the whole-program type inference (`analyze_program`, the `Compiler`)
   carry enough information for a non-C target?
2. Do Ruby's block / control-flow semantics in Spinel's supported subset survive
   translation to Crystal's block model?

**Verdict: yes to both, for a concrete subset.** The parser and analyzer are
reused verbatim; only the emitter is new. An 18-program corpus compiles through
the Crystal path and matches CRuby byte-for-byte on stdout (see
[Results](#results)).

## What is reused vs. new

| Stage | Reused? | Notes |
|---|---|---|
| `src/spinel_parse.c` (libprism frontend) | yes | unchanged |
| `src/node_table.c` (AST loader) | yes | unchanged |
| `src/analyze*.c` (~43.8k LOC, type inference) | yes | unchanged; the `Compiler` it fills is the backend contract |
| `src/codegen_crystal.c` (**new**, ~720 LOC) | new | walks the same `NodeTable` + `Compiler`, emits Crystal text |
| `lib/sp_*.c` (C runtime, ~35k LOC) | **no** | replaced by the Crystal stdlib + GC; not ported |

The C emitter (`src/codegen*.c`, ~88k LOC) is deliberately **not** reused: its
helpers are fused to the `sp_*` runtime ABI (`sp_RbVal` boxing, `SP_GC_ROOT`,
GNU statement expressions, `cls_id` switches). Reusing them would prove nothing
about the analysis contract. The new emitter reads `comp_ntype` / scopes / the
class table directly and maps `TyKind` onto Crystal's own type system.

## How to use it

```sh
make bin/spinel                       # build the compiler (needs `crystal` on PATH to run)
spinel --target crystal app.rb -S     # print the generated .cr
spinel --target crystal app.rb -c     # write app.cr only
spinel --target crystal app.rb        # crystal build --release -> ./app, then report
 scripts/poc_crystal.sh                # run the 18-program corpus vs CRuby and print metrics
 scripts/crystal_reject_test.sh        # assert the honesty gate refuses with a reason
 scripts/crystal_difference_test.sh    # pin each Ruby-vs-Crystal difference: agree / refuse / diverge
```

`--target` accepts `crystal`, `wasm32-wasi`, or `native` (both `--target=X` and
`--target X` spellings). The default (no flag) is unchanged and still emits C.

## Rejection discipline

A construct outside the supported subset is a **hard error**, not a silent
degradation. The emitter scans the whole program, records every out-of-subset
construct, and if any are found it refuses with a diagnostic anchored to the
first one's Ruby source line -- the same "name the feature and stop" discipline
as the C backend (`unsupported_feature`). Nothing that cannot be translated is
shipped as a `nil` placeholder that would run and print the wrong answer.

What is now translated (the supported subset):

- **Methods** whose name is a known-safe Crystal stdlib method (each verified
  present in Crystal 1.21 with matching semantics) or one the program defines
  (top-level, on the receiver's class, or -- for a receiverless call -- on the
  enclosing class). `is_a?(Const)` is accepted because Crystal's `is_a?` takes a
  type. Operators (`+ - * / % ** == != < > <= >= <=> >> & | ^`, unary `- + !`)
  emit as Crystal operators. A handful of names are translated rather than
  passed through, each verified against Crystal 1.21: `length`→`size`,
  `include?`→`has_key?`/`includes?`, `start_with?`→`starts_with?`,
  `end_with?`→`ends_with?`, `exclude_end?`→`excludes_end?`, `cover?`→`covers?`,
  `exist?`→`exists?`, `chdir`→`cd`, and Ruby's core class names where Crystal
  spells them differently (`Integer`→`Int`, `NilClass`→`Nil`,
  `TrueClass`/`FalseClass`→`Bool`, `BasicObject`→`Object`; a user class of the
  same name wins).
- **Optional positional parameters** with defaults (`def f(a, b = 1)`).
- **Multi-assign** to local variables (`a, b = 1, 2`), **index read and write**
  (`a[i]`, `h[k] = v`), **constants** (top-level and class/module-level) and
  **constant paths** (`C::CONST`), **modules** (and nested ones),
  **`case`/`when`/`else`**, and **`self`**.

What is still refused (out of subset):

- **Unknown method calls** on a concrete or receiverless position.
- **Poly containers** — an array or hash whose element/key/value type the
  inference left unresolved. See [no `untyped` in Crystal 1.21](#no-untyped-in-crystal-121).
- **Keyword / rest parameters** and an optional that precedes a required one
  (Crystal's named/positional model differs).
- **Block parameters** (`def m(&blk)`). Ruby's captured block is fully dynamic
  and Crystal's is not. Declaring `&blk` untyped is a trap rather than a fix:
  Crystal infers the proc type from the *declaration*, so `blk.call` on a block
  supplied at the call site silently evaluates to `nil` instead of invoking it.
  Spelling it properly needs the block's arity and parameter types, which is the
  receiver-type-aware work that has not landed yet.
- **Instance variables with a poly type.** Crystal requires a declared ivar
  type; a bare `@x` in a class body parses as a read and is rejected as an
  instance variable at the top level.
- **Integer overflow past 64 bits** (the C backend auto-promotes to Bigint;
  Crystal's `BigInt` needs a shard this emitter does not pull in).
- **Side-effecting statements in a class/module body** -- the two-pass emission
  hoists types before top-level code, so such a statement would run out of order.
- **Everything else the walkers do not model**: global variables, `yield`,
  lambdas/`Proc`, Regexp, `retry` inside a rescue clause, pattern matching
  (`case`/`in`), and compiler-synthesized `__*` helper calls (the analyzer
  desugars some methods into wrappers the C backend emits but this one skips).

The residual gap is *semantic divergence of methods that exist in both languages*
(e.g. `String#to_i` accepting underscores, `ENV[]` returning nil vs raising,
`String#<<` mutating where Crystal strings are immutable, endless/beginless range
slicing) and *nilability inference* — these compile and run but can differ on
edge inputs, and are tracked as [Known gaps](#known-gaps-and-limitations) for the
receiver-type-aware work that follows.

### No `untyped` in Crystal 1.21, and the boxed replacement

Crystal 1.21 **removed the `untyped` type**. `untyped` is no longer a keyword
(`def f(x : untyped)`, `[] of untyped`, and `Array(untyped)` are all parse
errors), and the `Any` alias that replaced it is gone too, as is `Object` as a
generic argument ("can't use Object as generic type argument yet"). The stdlib
source contains no use of `untyped` as a type at all.

The first response was to refuse everything the inference left widened (`TY_POLY`
/ `TY_UNKNOWN`) or contained in a widened container: with no `untyped` there is
no name to write, so the emitter refused rather than emit a parse error. That was
worth doing even though it *lowered* the emit-clean count — every program that
emitted `untyped` was a guaranteed build failure, so converting them to
line-anchored refusals cost no working program and removed a class of
silently-broken output.

The replacement (P20, below) is the C backend's model ported to Crystal: a
**boxed value type**. The C backend represents a widened slot as `sp_RbVal`, a
tagged union; Crystal cannot use `untyped`, so the equivalent is a class
`SpValue` holding a closed union of the scalar types plus `SpObject` (the base
every emitted user class inherits), with `sp_box`/unbox helpers and
CRuby-faithful rendering and arithmetic. `SpValue` is deliberately a **class**,
not a struct, so an expression that is only *sometimes* widened — a method
returning a concrete value on one path and a boxed one on another — type-checks
as a union with `SpValue` in it. A poly container becomes `Array(SpValue)` /
`Hash(SpValue, SpValue)`, real Crystal types. This is the same static-fast-path /
boxed-slow-path split the C backend uses; the typed path never touches it.

## Type mapping

The emitter maps Spinel's inferred `TyKind` onto Crystal types:

| Spinel `TyKind` | Crystal |
|---|---|
| `TY_INT` | `Int64` (literals emitted as `<n>_i64`) |
| `TY_BIGINT` | `BigInt` |
| `TY_FLOAT` | `Float64` |
| `TY_STRING` / `TY_STRBUF` | `String` |
| `TY_SYMBOL` | `Symbol` |
| `TY_BOOL` | `Bool` |
| `TY_INT_ARRAY` / `TY_FLOAT_ARRAY` / `TY_STR_ARRAY` | `Array(Int64)` / `Array(Float64)` / `Array(String)` |
| `TY_STR_INT_HASH`, … | `Hash(String, Int64)`, … |
| user class `i` | the class name (emitted as a Crystal `class`) |
| `TY_POLY` | `SpValue` (the boxed runtime value) — with `CR_POLY_BOXED` (P22); non-concrete otherwise |
| `TY_POLY_ARRAY` | `Array(SpValue)` for an empty `[]` pinned by heterogeneous pushes (P21 scalars; P22 user objects/containers via `SpBox`); no expression spelling otherwise |
| `TY_POLY_POLY_HASH` | `Hash(SpValue, SpValue)` — with `CR_POLY_BOXED` (P22); not created in P21 |
| `TY_UNKNOWN` | no spelling: not inferred / unsupported, counted as a metric |

Locals and method parameters are annotated with their inferred type when it is
concrete, which is what keeps `Int64` arithmetic and container element types in
agreement. Value-type promotion, GC rooting, and the tagged-union machinery have
no Crystal analogue — Crystal's own GC and static unions absorb them.

## P20: the boxed-value runtime

This phase lands the infrastructure for general (non-Rails, non-typed) Ruby: the
runtime the emitter boxes into. It is deliberately **additive and
zero-regression**: the runtime is emitted into every program and compiles, but
the emitter side that *uses* it is staged behind a switch in
`codegen_crystal.c` (`CR_POLY_RUNTIME` at P20; split into
`CR_POLY_CONTAINERS`/`CR_POLY_BOXED` at P21, see below), currently `0`.

**Runtime** (`src/crystal_runtime.h`, emitted as the `CR_RUNTIME` constant):
`SpObject` (base class with the `sp_send` dispatch seam), `SpValue` (a class over
a closed union `Int64 | Float64 | String | Symbol | Bool | Nil | SpObject |
SpValue`), `sp_box` overloads, the scalar operators (`+ - * / < <= > >=`), and
the unbox helpers (`sp_unbox_i/f/s/b`). It is generated ahead of every user class
so `SpObject` is visible and `sp_box` is in scope at every boxing site.

**Staged emitter seam** (the `CR_POLY_RUNTIME` switch, later split): `cr_map_type` maps `TY_POLY` →
`SpValue` and poly containers to `Array/ Hash(SpValue…)`; poly locals and ivars
are declared `SpValue` and boxed on write; a concrete argument to a widened
parameter is boxed; a call on a widened receiver routes through
`SpValue#sp_send`; a widened predicate is guarded with `truthy?`; and each class
with user methods emits an `sp_send` arm per method.

The switch is `0` because boxing an argument to a widened *parameter* without
also dispatching and unboxing inside the callee's body turns working
translations into build errors — exactly the function-boundary half that P22
(the callee-body work) supplies. The corpus gate holds every phase to zero PASS
regressions, so the seam ships inert until that half lands; flipping it is a
one-line change plus the callee-body work, not a re-derivation.

**Measured effect (P20).** The runtime alone, with the emitter seam off, is
byte-neutral: the full 4,398-program corpus is **identical to the committed
baseline** — PASS 327, DIFF 49, BUILDFAIL 268, REFUSED 3754, zero transitions,
zero PASS regressions. The runtime text is emitted and compiled by every
program, so the seam is proven to build while changing nothing.
`scripts/crystal_difference_test.sh` pins that the runtime is present
(`class SpValue`, `def sp_box`) and that a trivial program still builds and
matches CRuby. The staged emitter code is exercised by building the compiler
with the seam on, which is how the P22 failures (boxed-container writes,
the `sp_box` union gaps) were found and recorded.

## P21: widened scalar arrays

P21 is the first half of the boxed-value work: the **container** half, landed
alone because it does not need a callee body to cooperate. The single staged
switch is split into two (`codegen_crystal.c`):

```
CR_POLY_CONTAINERS 1   /* P21: widened containers */
CR_POLY_BOXED      0   /* P22: widened locals/params/receivers */
```

P21 widens **arrays only**, and only when every pushed element is a **scalar**
(`Int`/`Float`/`String`/`Symbol`/`Bool`/`Nil` — exactly the members of
`SpValue::Raw`). The shape it handles is the common `a = []` followed by
disagreeing scalar pushes:

```ruby
a = []
a << 1
a << "x"
p a                     # => [1, "x"], byte-identical to CRuby
```

The empty-literal pin (`cr_find_pushed_elem`) scans the pushes. Homogeneous
pushes pin the concrete element type as before (`Array(Int64)`, an object
class's name, …). Heterogeneous-but-all-scalar pushes pin
`Array(SpValue)` and record the local in `poly_arrays`; every later
`<<`/`push`/`unshift` to it boxes its argument through `sp_box` (a scalar is
already a `Raw` member, so no wrapper is needed), and a block iterating it
unboxes the yielded `SpValue` back to the block parameter's inferred type
(`sp_unbox_i/f/s/sym/b`). Reads of a widened array are still deferred to P22, so
the poly-array *expression* type stays non-concrete — only the binding is
written `Array(SpValue)`.

**P21.1 — every argument, and index writes.** The pin inspects *every* argument
of a `<<`/`push`/`unshift`, not just the first, so `a = []; a.push(1, "x")`
widens to `Array(SpValue)` instead of pinning `Array(Int64)` and then failing to
build on the `"x"`. Index writes (`a[i] = v`) are handled conservatively: Ruby
extends a short array on assignment where Crystal's `[]=` raises, so a write is
safe only when its value already matches the pinned element type. A disagreeing
write, or any `[]=` beside a widening push (whose index cannot be proven in
bounds), is refused at the write with a named reason rather than emitted as an
`Array(<T>)` it cannot be stored into. A lone `a[0] = v` with no push has no
element type to pin from and keeps the empty-literal refusal. P21.1 is
corpus-neutral (the corpus's multi-argument pushes are on already-typed arrays,
not empty-`[]` candidates) but closes those shapes in isolation.

**Refusals, not silent build failures.** A heterogeneous array with any
non-scalar element — a user object, `Struct`, `Range`, `Array`, `Hash` — cannot
ride `SpValue`'s scalar union; boxing it needs the `SpBox`/`SpObject` wrapper
that is P22's work. P21 therefore refuses such a program **at the first
disagreeing push** (the most precise anchor: it names the offending element's
type), rather than emitting an `Array(SpValue)` that would fail to build:

```
spinel: prog.rb:6: a heterogeneous array whose element is a user object or container (Foo); P21 widens only scalar elements to Array(SpValue)
```

For the same reason the runtime reverts to the committed P20 text plus
`sp_unbox_sym`: no `SpBox` class, no `Array/Hash/Range` `sp_box` overloads, and
**no user class inherits `SpObject`**. `Hash` is not widened in P21. Giving every
class `< SpObject` widened its type to `SpObject+` and broke inference on
value-in-collection dispatch (`test_poly_return_value_class`,
`test_cls_value_in_collection_dispatch`), so P22 reintroduces it together with
the callee-body work that makes a boxed object actually dispatch.

**Measured effect (P21).** PASS 327 (unchanged), DIFF 49 (unchanged),
BUILDFAIL **266** (−2), REFUSED **3756** (+2), zero PASS regressions — the two
transitions (`test_block_autosplat`, `test_poly_array_range_aref`) were
BUILDFAILs caused by a heterogeneous array holding a container element, now
honest refusals at the offending push. `scripts/crystal_difference_test.sh`
pins an AGREE entry for the scalar-array boxing and a REFUSE entry for the
object-element heterogeneous array.

## P22: object boxing via SpBox

P22 is the other half of the boxed-value work: widened *slots* (locals, ivars,
parameters, receivers) become `SpValue`, and a call on one dispatches.

The sketch in the P20 notes — "reintroduce `< SpObject` on every user class" —
turned out to be unsound. Crystal collapses a union of `SpObject` subclasses to
the virtual `SpObject+`, for instances and for metaclasses, which erases the
emitter's per-class dispatch. Adding `< SpObject` to user classes broke exactly
the two tests that dispatch surface exists for:

- `test_poly_return_value_class`: `undefined method 'label' for P (compile-time type is SpObject+)`
- `test_cls_value_in_collection_dispatch`: `undefined method 'apply' for Stamp.class (compile-time type is SpObject+.class)`

So P22 boxes an object in a **wrapper** instead: `SpBox(T) < SpObject` carries
the object, and the object's own class keeps no superclass. The concrete `T` is
known at each `SpBox(T)` instantiation, so a per-class dispatch overload the
emitter generates resolves at compile time, and a union of user classes (or of
class objects) stays concrete.

**P22a — the object-boxing infrastructure (additive).** The runtime gains
`SpBox(T) < SpObject` (a getter, `initialize`, and `inspect`/`to_s` forwarding
to the wrapped value), and the emitter emits one
`sp_box(v : Foo) : SpValue` per top-level user class
(`SpValue.new(SpBox.new(v))`), skipping modules, native classes, builtin
reopens and nested classes. Nothing calls them yet — the switch is still off —
so P22a is corpus-neutral: PASS 327, DIFF 49, BUILDFAIL 266, REFUSED 3756, zero
transitions. Only *objects* are wrapped; a stdlib container still has no wrapper.

**P22n — emit the superclass in the class header.** `cr_emit_class` ended with
`buf_printf(b, "%s %s\n", "class", ci->name)`, so `class B < A` emitted as a bare
`class B`: B did not inherit A's methods at all. `ClassInfo.parent` (the superclass
index) has been tracked by the compiler the whole time — the Crystal emitter simply
never read it. The header now carries `< A` when the parent is itself an emitted
user class (a builtin/native parent is left off, since `< Object` would drag in a
superclass the emitter does not model).

Measured **+9 PASS, 0 regressions**: 7 BUILDFAIL→PASS and 2 DIFF→PASS. This is not
a boxing change and it is not a dispatch change — it is the receiver being *statically*
concrete, so no dynamic dispatch could ever have reached these programs. Calling an
inherited method through a child (`def call_x(e) = e.x` with `e` of type `A | B`) is
the shape that fails.

The change also had to teach the ivar seeding about inheritance. P22j widens a
declared ivar to `T?` when `initialize` does not assign it unconditionally, and a
subclass with no `initialize` of its own never assigns an inherited ivar — so the
seeding tried to re-declare it at the wider type, which Crystal rejects once the
superclass is real ("instance variable '@tag' of Node, with Sub < Node, is already
declared as Int64"). `cr_class_inherits_ivar` walks the `parent` chain and skips
both the re-declaration and the widening for an inherited ivar. That bug was
invisible while subclasses were unrelated classes, which is a general caution: a
previously-unrelated emission can hide a latent type error, and making the relation
real can expose it.

**P22o+P22p — the inheritance call path completed; `super` translated.** P22n made
the superclass real in the header, but the call path still pretended classes were
unrelated: `cr_user_method_ok` matched the receiver's *own* class only (an inherited
method was refused as "not in the Crystal subset"), and `synth_init` seeded a no-arg
constructor onto subclasses that had inherited one, shadowing it. Both now walk the
`parent` chain through one predicate, `cr_emitted_parent`: the parent counts only if
it is an emitted user class, is *nameable* where the child's header is written (same
nesting or top level — a parent nested in a module would emit `class Child < Sub`
and Crystal says "undefined constant Sub"), and is *not reopened* at another site
(a reopened class is emitted from ONE body, so the other site's defs are never
emitted and a child must not be able to call them). `cr_emit_class`'s header calls
the same predicate, so the header and every lookup agree by construction.
`synth_init` is skipped when an emitted ancestor defines `initialize`; a Struct
parent is special-cased in that walk (it supplies a real constructor).

P22p emits `SuperNode` as `super(args)` and `ForwardingSuperNode` as bare `super`
(Crystal's own forwarding matches Ruby's shape for the emitted subset), guarded by
`cr_super_ok`: emitted parent, an ancestor def of the same name, arity against the
recorded def shape, and the block-forwarding guards probed against Crystal 1.21.

Measured **+18 PASS, 0 DIFF, −10 BUILDFAIL, −8 REFUSED, zero regressions**
(11 REFUSED→PASS, 7 BUILDFAIL→PASS; 4 of the new PASSes are `super` programs, the
rest inherited defs/readers/constructors). Making the relation real exposed five
latent defects, all fixed rather than shipped:

- The `length`→`size` rename shadowed a user class's own `length` (the rename is
  for Array/Hash/String). The Ruby name is kept when the receiver's class chain
  defines it; a poly/unknown receiver whose union arms spell the method differently
  is refused by name.
- `def initialize(p : String, h : String = nil)` → "can't restrict Nil to String".
  Crystal rejects a nil default against a concrete restriction only when the
  default is *instantiated*, so the refusal fires only when a visible call site
  omits the argument (`cr_param_omitted_anywhere`); defs whose call sites all
  supply it stay emittable (`folded_ternary_arm_takes_result_type` proves it). A
  class-value `k.new(u)` is invisible to that scan, so `class_value_new_optional_
  param` remains the documented class-value-constructor BUILDFAIL.
- `attr_reader :x` plus a later `def x` in one body: the emitter wrote body defs
  first and attr defs after, so the attr always won; Ruby is last-definition-wins.
  A source-order scan (`cr_body_last_defines`) picks the winner on both sides — and
  `def self.x` is *not* in that contest (attrs are instance-level only; conflating
  them regressed `cls_value_in_collection_dispatch` mid-phase, and the corpus gate
  caught it).
- The nested-parent and reopened-parent rules above turned two predictable
  BUILDFAILs back into named refusals.

**P22q — parse-clean emission; implicit-parent `super` (landed).** Four emitter
holes, three of which turned parse-error BUILDFAILs into valid Crystal:

- **Symbol quoting.** A `SymbolNode` was emitted verbatim, so a symbol whose text
  is not a bare Crystal identifier (`:"a b"`, `:"+="`) produced a parse error.
  `cr_sym_is_bare` now quotes anything outside identifier/operator form.
- **do/end-block calls in operand/element position.** `arr.map do … end[0]` parses
  as `arr.map(do…end[0])`; `cr_emit_operand` now parenthesises a block call there.
  Scoped to operand/element position only, **not** folded into `cr_needs_parens`:
  parenthesising the receiver position would newly compile `issue_3235`, whose
  answer then DIFFs on `Float#round`'s sign of zero — the honesty rule keeps the
  BUILDFAIL over a silent wrong answer (see `docs/float-rounding.md`).
- **`super` with an implicit parent.** `def initialize; super; end` in a class with
  no explicit superclass. Crystal rejects a bare `super` with no superclass, but
  the *default* superclass is `Object`/`Reference`, which does define those
  methods; a probe-backed whitelist (`cr_super_builtin_parent`) allows the
  `Object` arities (`initialize`/`to_s`/`inspect`/`hash` at 0, `==`/`!=`/`===`
  at 1) and refuses anything else.
- **`k::CODE` on a runtime value** is refused by name.

Measured **+1 PASS, +0 DIFF, −2 BUILDFAIL, +1 REFUSED**: `test_inline_yield_value_type_result`
BUILDFAIL→PASS, `test_const_path_constant_receiver` BUILDFAIL→REFUSED.

**P22r — desugared-Enumerable passthrough (landed).** The analyzer rewrites
`recv.m(args){blk}` for the 25 `builtins/enumerable.rb` methods into a bare
`__enum_m__<id>(recv, args)` helper. The C backend emits those bodies; the Crystal
backend has none to emit, so the call was refused. Where Crystal's stdlib answers
the *same* call, the emitter now reconstructs `recv.m(args) { blk }` from the
helper — the receiver is `argv[0]`, the block is preserved, and it is emitted with
the **Ruby** method name because the yield oracle (`ty_block_yield`) is keyed on
Ruby names (`drop_while`, not `skip_while`).

The scope is deliberately tiny. The full candidate set measured **21 PASS / 5 DIFF
/ 25 REFUSED→BUILDFAIL** — dishonest in both directions — so only the methods whose
Crystal stdlib form is verified identical are mapped:

- `inject`→`reduce`, `each_with_object`, `count`, `none?`;
- `drop_while`→`skip_while`, gated to Array/Range receivers: Crystal's
  `Hash#skip_while` answers an Array of **Tuples** where Ruby's `Hash#drop_while`
  answers `[k, v]` **Arrays**, so a Hash (or a widened/uninferred receiver that
  could be one at run time) keeps the refusal.

Excluded, and why: `max_by`/`min_by` reject the `(n)` argument form; `find`,
`minmax`, `max_by`/`min_by` and `partition` answer Tuples or take a different
arity; blockless forms differ; and a `reduce`/`inject` symbol op (`arr.inject(:+)`)
has no Crystal form. `none?`/`drop_while` are probe-verified but not yet exercised
by the corpus.

Measured **+4 PASS, +0 DIFF, +0 BUILDFAIL, −4 REFUSED**: `bundle_class_01`,
`count_block_poly_pred`, `hash_values_inject_sym`, `issue_3102`.

**P22s — DIFF-cluster inventory; adjacent interpolated literals (landed).** A
correctness pass: re-ran all 55 `DIFF`s from the P22r corpus, captured Crystal vs
CRuby output for each, and clustered them by root cause (plan §11). Exactly one
cluster was a genuine emitter bug. Adjacent string literals each carrying an
interpolation (`"a#{x}" "b#{y}"`) parse to a nested `InterpolatedStringNode`, whose
parts the emitter's loop did not handle — so it silently dropped every
interpolation and emitted `""` (and `"[]"`, `" done"`). `cr_emit_interp_parts` now
flattens a nested `InterpolatedStringNode` part recursively.

The other 54 are analysis- or runtime-level gaps, documented rather than refused
because a broad refusal would regress many PASS programs: Crystal's
`each`/`each_with_index`/`each_slice` return `nil` (Ruby `self`); blockless forms
return a lazy `Iterator`, not an `Enumerator`; `Float#to_s` exponent form
(`1.0e-5` vs `1.0e-05`); `Kernel#puts` does not flatten nested arrays; predicate
`===` vs `==` and `uniq` `eql?` vs `==`; `Int64` vs `Integer` in `inspect`;
`Class#<` returns `false` not `nil`; the array nil-extension cluster
(`a[3]=v`, `a,b,c=[10,20]`, `fill(v,0..9)`, 3-param auto-splat, `h[missing]`);
error class/message text; `Dir` glob order; and `Fiber#resume` value semantics.

Measured **+1 PASS, −1 DIFF, +0 BUILDFAIL, +0 REFUSED**:
`test_interp_adjacent_concat` DIFF→PASS (the gate's only transition).

**P22t — hash double-splat; defensive interpolation-part refusal (landed).** The
DIFF cluster (plan §11) named `hash_double_splat` as a real bug: the `NK_HashNode`
emitter assumed every element was an `AssocNode`, so a `**h` element read a
nonexistent `key` and emitted `nil => h`, silently diverging. Crystal has no
hash-literal double-splat (`{ **h }` is a parse error), so the emitter now builds a
`.merge` chain — runs of plain assocs become one `{ k => v }` literal and each splat
merges in, preserving Ruby's left-to-right override order; a lone `{ **h }` emits
`.dup` (Ruby copies rather than aliases). Separately, `cr_emit_interp_parts` now
records a named refusal for an interpolation part kind it does not model, instead of
silently dropping the fragment.

Measured **+1 PASS, −1 DIFF, +0 BUILDFAIL, +0 REFUSED**: `test_hash_double_splat`
DIFF→PASS (the gate's only transition; the defensive refusal caused none).

**Known hole in `SpBox` — CLOSED by P22m.** (was: do not dispatch a boxed object yet)
`SpBox` used to define only `val`, `initialize`, `inspect` and `to_s`. It had **no
`sp_send` override**, so `sp_box(some_object).sp_send(name, args)` inherited
`SpObject#sp_send`, which raises. Any gate that routed a widened receiver through
`sp_box(…).sp_send(…)` was therefore only valid while the receiver held a *scalar*;
the moment the union had a user-object arm it compiled and then raised at RUN time.
That is not hypothetical: P22l lost 3 PASS programs to exactly this, and the
`SpObject#sp_send` doc comment actively claimed per-class overrides that no class
provided. See P22m below and
`~/.opencode/plan/crystal-backend-p22-plan-v2.md` for the full post-mortem.

**P22m — `SpBox#sp_send` and per-class `sp_box_send` (additive, corpus-neutral).**
The box now has a real dispatch seam:

```crystal
class SpBox(T) < SpObject
  def sp_send(name : String, args : Array(SpValue)) : SpValue
    sp_box_send(self, name, args)   # emitter emits one overload per user class
  end
end
def sp_box_send(v : SpBox(T), ...) : SpValue forall T   # generic fallback: refuses,
  raise ArgumentError.new(sp_box_undefined(v, name))   # naming the concrete class
end
```

Overload resolution picks the concrete `SpBox(Foo)` at each instantiation, which is
the property that matters: it lets **two arms of one union spell the method
differently and still both resolve**. That is the `(Tags | Array(String))#include?`
shape no single renamed call could ever reach, and the reason P22k refuses it.

`cr_emit_sp_box_send_overload` emits one arm per top-level user class carrying only
**zero-argument public instance methods** — and it walks the **superclass chain**, so
a `SpBox(Derived)` answers a method `Derived` only inherits from `Base`. (Collecting
only the class's own scopes left a subclass with no arm at all: the same
missing-relation trap P22n removed for the class header, on the dispatch path.)
Methods with parameters, block-takers, operators, `initialize`, private methods and
synthesized names get no arm and fall through to the refusal — boxing a method's
arguments per-arm fails *silently* if done wrong, so the arm set is kept to the part
that cannot.

**Corpus-neutral: +0 PASS, 0 transitions.** No emitted program routes a receiver
through `sp_send` (that gate needs `CR_POLY_DISPATCH` or a boxed local, both off),
and a stdlib container in the union has no arm either. It is landed to close the
footgun above and to make the seam real for whoever needs it, and it is verified by
direct execution rather than left as untested dead code — see the plan file for the
five measured behaviours (zero-arg dispatch, inherited dispatch, unknown name,
arity refusal, param-method refusal).

Re-running P22l's gate *on top of* this seam clears 2 of its 3 regressions
(`poly_to_s_user_class`, `poly_recv_default_value_type` go DIFF → PASS); the third,
`poly_dispatch_long_class_name`, now dispatches correctly to both user arms and fails
only on `SpBox(Array(Int64))`, which has no arm because stdlib containers are out of
scope. That makes the P22l design *safe* — and still worth **0**, since its 8
affected programs convert none. Safety was never P22l's problem; demand was.

**P22b — boxed locals (staged off; measured net-negative).** The next step was
to box a widened local as `SpValue` and unbox its reads. Measured, it is
net-negative: Crystal already represents a poly local as a **union**, and a
union resolves the local's methods natively, so boxing it discards working
behavior. The locals-only scope is **25 PASS→non-PASS regressions and 0 gains**;
the full boxed-slot scope (locals + ivars + parameters + receivers) is **76
regressions and 2 gains**. Implementing the read side — unbox at a read the
analyzer narrowed to a concrete type, boxed-local receiver dispatch, operand
boxing, and `sp_box` overloads for metaclasses and containers — fixed 18 of the
23 sampled regressions but introduced 33 new ones (**47 regressions vs 1 gain**);
the one gain is poly *parameters*/equality, not locals. The code is therefore
staged behind `CR_POLY_LOCALS`/`CR_POLY_DISPATCH`, both `0`; the additive runtime
fixes (`sp_send` with no args, `self.` receivers in the dispatch arms, `SpValue`
operand overloads, metaclass/container `sp_box`) remain. The evidence points to
boxing at widened **boundaries** (a parameter, a container element) rather than
at the local slot.

**P22c — boxed container elements (landed).** The first boundary the evidence
points at is the container element. With the `SpBox` wrapper in place, the
empty-`[]` pin now widens a heterogeneous array whose non-scalar elements have an
`sp_box` overload: a user object (its per-class overload from P22a) or a stdlib
container (`Array`/`Hash`/`Range`). Each push boxes through `cr_emit_boxed`, and
the array's `inspect`/`to_s` forward through `SpValue` → `SpBox` → the wrapped
value, so `p a` renders like CRuby's (`[1, [5]]`, or `[1, Foo(2)]` for a class
with a defined `inspect`). `cr_boxable_elem` is the gate: an element with no
overload — BigInt is the common case, since it has no Crystal spelling in the
boxed runtime — keeps the honest refusal, now naming the element's kind
(`a heterogeneous array whose element has no boxed representation (bigint)`).

The widening is regression-safe **by construction**: it only fires on an empty
`[]` literal whose pushes were previously refused, so no PASS program can
change. Three corpus programs newly emit. `poly_array_range_aref` becomes **PASS**
— its `[5]` container element boxes and the range-slice reads work on
`Array(SpValue)` unchanged. The other two expose pre-existing emitter gaps the
refusal had hidden: `block_autosplat` reaches a short-array auto-splat
(`a, b, c = ["p", 1]`), which raises `IndexError` in Crystal where Ruby nil-fills
the missing params, and `enum_with_index_value` reaches a `map.with_index` chain,
which the emitter spells as a blockless `map` followed by `with_index` (a build
error). Both are real gaps in the read/iterator surface, now visible instead of
masked behind the array refusal; neither is caused by the boxing (the
`map.with_index` one is closed by P22d below, and the short-array auto-splat is
left documented — see [P22d](#p22-object-boxing-via-spbox)). Net **+1 PASS,
+1 DIFF, +1 BUILDFAIL, −3 REFUSED**, zero PASS regressions.

**P22d — `with_index` chains (landed).** P22c surfaced two emitter gaps; this
closes the first. Crystal has no blockless `Array#map` (a blockless `map` is a
compile error: "expected to be invoked with a block") and no index-taking
`select`/`reject`/`sort_by`, so Ruby's `arr.<stage>.with_index(off) { |x, i| }`
chain did not translate. The emitter now recognizes the immediate array/range
chain and rewrites it to Crystal's indexed forms:

| Ruby | Crystal |
| --- | --- |
| `arr.map.with_index(off) { }` | `arr.map_with_index(off) { }` |
| `arr.each.with_index(off) { }` | `arr.tap { \|r\| r.each_with_index(off) { } }` |
| `arr.select.with_index(off) { }` | `arr.each_with_index(off).select { }.map(&.[0]).to_a` |
| `arr.reject.with_index(off) { }` | `arr.each_with_index(off).reject { }.map(&.[0]).to_a` |
| `arr.<stage>.with_index(off)` | `arr.each_with_index(off)` |

Two details make the output match CRuby. Crystal's `*_with_index` take an
**Int32** offset while an integer literal is emitted `1_i64`, so the offset is
cast (`.to_i32`) or the stdlib's `offset + i` becomes an Int64 and the yield
fails to typecheck. And Crystal's `each_with_index` Iterator yields
`{element, index}` **Tuples** where Ruby yields `[element, index]` **Arrays** —
the same Tuple/Array tension Phase 12 records. The pair stays a Tuple (a Tuple
auto-splats in a later `|x, i|` block and an Array does not, so a blockless
`arr.each.with_index.map { |x, i| }` keeps working), and only the `.to_a`
terminal converts it to Arrays. The shared `sort_by.with_index` desugar rewrites
to `each_with_index.sort_by { }.map { }`; a blockless `each_with_index` is now
materialized (`arr.each_with_index().to_a`) so Crystal's `sort_by` — which is not
on `Iterator` — resolves. The stored first-class Enumerator forms
(`e = arr.each.with_index`; `e.next`/`e.peek`/`e.size`) remain unhandled: Ruby's
Enumerator is rewindable and Crystal's Iterator is not.

**Measured effect (P22d).** **+5 PASS, +1 DIFF, −5 BUILDFAIL, −1 REFUSED**, zero
PASS regressions. `hash_enum_with_index_chain`, `issue_3228`,
`range_select_with_index`, `sort_by_with_index` and `with_index` (a previously
*refused* program) become PASS; `enum_with_index_value` moves BUILDFAIL → DIFF
(its stored-Enumerator re-iteration is the remaining gap). The difference ledger
pins the five shapes.

The other gap P22c surfaced — a short-array auto-splat binding fewer params than
the block asks for (`a, b, c = ["p", 1]`; Ruby nil-fills `c`, Crystal's
`a, b, c = ary` raises `IndexError`) — is left documented, not fixed. A safe
translation is blocked by the same Tuple/Array tension: Crystal requires a block
variable to keep one static type, but Ruby's auto-splat makes a param `T` when
the element is an Array and `Nil` when a scalar is bound to the first param, so
`a + b` cannot stay monomorphic. A conditional destructure
(`if e.is_a?(Array) then a, b = e[0], e[1] else a, b = e, nil end`) still infers
`b` as `T | Nil` and fails. Only the cases where the poly array's element kind is
**statically known** to be Arrays — a container-element widening like P22c's
`[1, [5], [2, 3]]`, which `yields[0] == TY_ARRAY` distinguishes before the
element collapses to `TY_POLY` — are decidable, and none is in the corpus.

**P22e — boxed instance variables (landed).** A widened (poly) instance variable
is the other slot the P22b evidence points at. Unlike a *local* — where Crystal's
own union already resolves the methods, which is why boxing a local was
net-negative — an ivar must be **declared**, and a poly one has no Crystal
spelling, so it was a hard refusal (511 corpus programs, the single largest
bucket). P22e declares it `SpValue`, boxes every write through `sp_box`, and
unboxes a read the analyzer narrowed to a concrete scalar.

Two things had to be made safe, and their safety is what keeps the refusals
honest rather than trading them for build errors:

- **A synthesized `initialize` seeds the ivar nil and the declaration becomes
  nilable (`T?`).** Crystal refuses to compile a class whose declared ivar is not
  assigned in its `initialize` ("rendering it nilable"); a class that only
  assigns it elsewhere (or has no `initialize`) therefore needs the seed. The
  seed must be `nil`, not `uninitialized T`: a read-before-write that Ruby
  answers `nil` would otherwise read a garbage reference and *segfault*
  (`test/uninit_ivar_returns_nil` pins it). The synthesized `initialize` is
  emitted only when the class has none, so a user `initialize` is never
  shadowed.
- **The ivar is widened only when every read is a shape `SpValue` answers.**
  `cr_ivar_reads_safe` refuses the ivar when a read is the receiver of a
  concrete Crystal method the runtime has no arm for (`@units.abs` on an
  Integer), when the class exposes an `attr_accessor` writer that would assign
  the raw value past the boxing path, or when some *other* poly value in the
  program (a poly parameter) already has such a call — because widening the ivar
  can be the first thing that lets that program emit, and refusing here keeps the
  diagnostic at the real cause instead of turning an honest ivar refusal into a
  `map`/`gsub` build error.

**Measured effect (P22e).** **+7 PASS, +2 DIFF, −7 BUILDFAIL, −11 REFUSED**, zero
PASS regressions, and **no REFUSED→BUILDFAIL trade**: every changed program moved
REFUSED or BUILDFAIL to PASS (or DIFF), because the read-safety gate refuses
exactly the programs that would otherwise build-fail. The four bucket programs
that became PASS are `array_new_poly_size`, `ivar_ternary_mixed`,
`param_inference_if_predicate`, `uninit_ivar_returns_nil`, `void_value_begin_writer_tail`
and `void_value_if_writer_arm`; `map_to_heap_objects` moves BUILDFAIL → PASS. The
two DIFFs (`map_untyped_block_result`, `send_setter_returns_method_value`) are a
lazy-`map` rendering and a setter-return difference, both separate gaps now
visible instead of hidden. The remaining ~500 programs in the bucket each have an
unsafe read or writer and keep their refusal.

**P22f — class/module-body declarations (landed).** The second-largest refusal
bucket (288 programs) was "a side-effecting statement in a class/module body":
any non-`def` statement in a class body was refused, because the two-pass
emitter hoists types before top-level code and a genuine side effect would run out
of order. But three groups of statements have **no emitted effect**, so skipping
them is safe and exact:

- **`module_function`, `private`/`public`/`protected`** (bare, or listing
  symbols, plus the modifier forms the analyzer has already marked): a method is
  callable in Crystal without the marker, and the analyzer has already recorded
  `is_module_function` and resolved the calls. Crystal's own `module_function`/
  `private` have different scoping, so emitting the marker would mistranslate.
- **`include M` of a user module**: emitted, because Crystal's mixin is what
  makes the module's methods available (the emitter does **not** transplant them).
  A builtin mixin (`Comparable`, `Enumerable`, `Forwardable`, ...) is *refused*:
  Crystal's `Comparable` needs a type argument and drops `between?`, and
  `Enumerable` changes the method set, so neither emitting nor dropping the
  marker is behaviour-preserving. `Kernel`/`Math` are always present and their
  calls already resolved, so those are skipped.
- **`extend self` / `extend M`**: skipped, same reason as visibility markers.

Three guards keep the bucket's refusals honest rather than trading them for build
errors: an include of a **reopened** module is refused (Crystal resolves the
mix-in at the include, so a module whose methods are added later would silently
lose them); a **constant-path** include (`include A::B`) is refused (the leaf
name is ambiguous); and a `module_function` method that touches an ivar on a
module an includer mixes in is refused (CRuby runs it on the instance, Crystal
on the module).

The same pass fixed two pre-existing gaps the unblocked programs exposed. A user
class named like a **Crystal stdlib generic** (`Box`, `Tuple`, `Array`, `Set`,
`Range`, `Proc`, ...) cannot be reopened there — Crystal auto-parameterizes the
reference or rejects it ("Tuple is not a class") — so it is now a line-anchored
refusal. And `respond_to?`/`include?`/`instance_methods`-family calls on a
**class-object** receiver joined the reflection table, turning 8 BUILDFAILs into
honest refusals.

**Measured effect (P22f).** **+8 PASS, +0 DIFF, −16 BUILDFAIL, −8 REFUSED**, zero
PASS regressions, and no REFUSED→BUILDFAIL trade: 8 REFUSED → PASS and 8
BUILDFAIL → REFUSED. `include_nested_module`, `include_chain_module_nested_const`,
`include_method_body_locals`, `mixin`, `mixin_to_s_inspect`, `module_extend_self`,
`module_function_include` and `module_function_str_method_param` become PASS.

**P22g — `yield` methods (landed).** The next-largest bucket (182 programs) was
`YieldNode`: a direct `yield` in a method body. Crystal's `yield` is the same
construct, but the method must **declare** it accepts a block (`def f(&)`), which
the emitter omitted. A method whose body contains a `yield` (and has no explicit
`&b` parameter, which stays refused because Crystal infers the proc type from the
signature) now emits `(&)`, and `yield a, b` emits `yield(a, b)`. A block passed
to such a method keeps its declared parameter count: the auto-splat that turns
`|a, b|` into one synthetic param is suppressed when the callee is a user method
that yields two values (Crystal's `yield a, b` already passes two block params).

Two shapes are refused: a method whose `yield` calls **disagree on arity** (Ruby
nil-fills the missing block parameters; Crystal infers one block signature and
rejects the second arity), and a yielding method in a program that already has a
call on a **union** receiver of a builtin-container method (`(Range | String)#each`)
whose Crystal arm is missing -- widening the yield would let such a program emit
and then build-fail on the unrelated dispatch. Blocks with optional/rest/numbered
parameters are refused too (Crystal block params take no defaults).

**Measured effect (P22g).** **+16 PASS, +3 DIFF, −4 BUILDFAIL, −15 REFUSED**, zero
PASS regressions. Sixteen previously-refused programs pass; three become DIFF.
Seven programs move REFUSED → BUILDFAIL: each exposes a *separate* pre-existing
gap the yield rewrite unblocked -- `Process.times` (unsupported), a `Struct#new`
nil-fill, a `Hash` K/V inference, a `(Range | String)` union dispatch, and three
type-flow cases in the inlined yield body. None is caused by the `yield`
translation, and the union guard above removes every case it can detect; the
remainder are documented rather than hidden.

**P22h — comparison at a widened parameter boundary (landed).** P22b established
that a poly *local* must keep its native Crystal union (boxing the slot regressed
47 programs), and pointed at widening the *boundary* instead. This is the first
such boundary. A method parameter the call sites widened to a mixed union --
`def less_or_equal(a, b); a <= b; end` called with `Int`, `String`, `Float`,
`Bool` and `nil` -- emits as one Crystal union, and an operator on it resolves
through Crystal's per-member overloads, which fails when a member lacks the
operator (`Error: expected argument #1 to 'Int64#<=' to be Float64, ..., not
String`). A **comparison** between two such parameters is now boxed to `SpValue`
on both sides, so the operator runs through the runtime's comparator instead:
`sp_box(a) <= sp_box(b)`.

Three limits make the box local rather than structural:

- **Only a comparison.** A comparison answers `Bool`, so the box never escapes
  the expression. An *arithmetic* operator answers `SpValue`, which does escape:
  `Array.new(sp_box(r) * sp_box(c), 0.0)` handed `SpValue` to a parameter typed
  `Int`, and boxing `prefix + mid` inside a method made it **return** an
  `SpValue`, so every later `s.size()` on that result became `undefined method
  'size' for SpValue`. `%` has no `SpValue` operator at all. Boxing the
  arithmetic operators cost three corpus programs and gained nothing, so they are
  left to their native union.
- **Only a parameter.** The gate is `LocalVar.is_param`, not "`type` is
  `TY_POLY`". A poly local, a poly array element or a union of user classes keeps
  its native union, which already resolves the members that share the operator.
  Boxing every `TY_POLY` operand cost 21 programs.
- **Only when it is not already an `SpValue`.** A boxed ivar read (`CR_POLY_IVARS`)
  or a boxed local is left alone, so the box is never applied twice.

One correction to the P22b note above, which cost real time: **`TY_POLY` does not
mean "emitted as `SpValue`"**. It is a top type the analyzer assigns when a slot's
writes disagree; how it *emits* depends on the slot. A widened **parameter**
emits as a raw Crystal union, which is exactly why it needs this boundary box.
The earlier helper that assumed otherwise silently declined to fire.

**Measured effect (P22h).** **+1 PASS, +0 DIFF, −1 BUILDFAIL, +0 REFUSED**, zero
PASS regressions (gate PASS, 365 baselined), the single transition being
`test_poly_equality_mixed_sites`: a mixed-union comparison that previously failed
to compile now agrees with CRuby on all ten lines, including the three rescued
`ArgumentError`s (`1 <= "x"`, `false <= true`, `nil <= nil`), which is Ruby's own
"comparison of X with Y failed". `test_nullable_poly_hash_arg_unbox` was an
incidental gain of the over-broad first cut and is correctly not part of this
phase. The difference ledger pins AGREE for the case (`poly_param_compare`).

**P22i — compound assignment to an instance variable (landed).**
`NK_InstanceVariableOperatorWriteNode` (`@x op= v`) was refused outright unless the
whole-slot `CR_POLY_BOXED` was on, which meant a plain `@count += n` on an `Int64`
ivar — which Crystal answers natively — was refused for no reason at all. That single
node was the sole refusal in `test_map_shadowed_param_poly_arg`. It now emits, like
the local case beside it, under two limits:

- **Only the arithmetic operators** (`+ - * / %`), where Crystal carries the compound
  form and the operand needs no nil guard. `**=`, `<<=`, `|=`, `&&=`, `||=` are
  refused **by name** — `<<` against a nilable `Array` slot, `|` against a nilable
  `Integer` slot and `||` against an unset ivar each need a guard the expansion does
  not carry. The old refusal said only `InstanceVariableOperatorWriteNode`; the new
  one names the construct.
- **Only a concrete scalar slot.** A widened ivar is declared `SpValue`, so the
  compound form would need an `SpValue#op=` that does not exist, and the arithmetic
  result would have to be unboxed before landing in the slot — the escape P22h had
  to avoid. A container slot (`@a *= 2` on `Array(Int64)?`) needs a nil guard, so
  `@count += n` is translated and `@a *= 2` is not.

**Measured effect (P22i).** **+1 PASS, +0 DIFF, +2 BUILDFAIL, −3 REFUSED**, zero
PASS regressions (gate PASS, 366 baselined). This is a *weak* trade — one PASS for two
programs moving from a precise refusal to a build error — and both are pre-existing
gaps this node had been masking, neither caused by it:

- `test_poly_user_include_value` — `@asked += 1` emits correctly; the class
  separately hits `undefined method 'includes? for Tags` (an `include?` →
  `includes?` remap gap in a *different* method).
- `test_safe_nav_typed_receiver` — `@count += n` on `Int64` emits correctly;
  `Player` declares `@combat : Combat` but assigns it only conditionally, so
  Crystal's "not initialized in all of the `initialize` methods" rule fires. That
  is P22e's `has_init` logic, not this node.

Neither is cheaply guardable, and refusing a compound assign *for an unrelated
reason* would be worse than the trade: a refusal that names the wrong construct is
what the honesty ledger exists to prevent. P22f achieved "no REFUSED → BUILDFAIL
trade" because its guards were predictive; here they are not, so the trades are
documented instead. A future P22e fix that declares a conditionally-assigned ivar
nilable would clear the second, and an `include?` remap fix the first. The ledger
pins AGREE for the scalar case (`ivar_op_write_scalar`).

**P22j — a conditionally-assigned ivar is nilable, and `&.` is not dropped (landed).**
P22i documented two REFUSED → BUILDFAIL trades it had been masking. This clears the
first and, in clearing it, exposed and fixed a second gap underneath.

- **The ivar declaration.** P22e decided nilability from whether a class *has* an
  `initialize`, not from whether it assigns every ivar in one unconditionally. So
  `@combat = Combat.new if armed` left `@combat` declared `Combat`, and Crystal
  rejected the class with `instance variable '@combat' of Player must be Combat, not
  Nil`. `cr_ivar_assigned_in_init` now scans the **top-level** statements of
  `initialize`'s body for a plain write to that ivar — a write nested in an
  if/unless/while/case/rescue does not cover every path — and the declaration is `T?`
  when the slot is not assigned there. That is Ruby-faithful: an ivar assigned only
  conditionally reads nil on the other path, which is exactly what `T?` says.
- **`&.` was silently dropped.** The node carries `call_operator == "&."` and
  `cr_emit_call` never read it, so every safe navigation became a plain `.`. That is
  right only when the receiver cannot be nil; on a nilable one it is `undefined
  method 'm' for Nil`. The two had to land together, because declaring `Combat?`
  turned a previously-working `.` into a build failure.

  The rewrite is `x&.m(args)` → `x.try { |t| t.m(args) }`. Three Crystal behaviours
  shaped it, each confirmed by probe after a wrong first attempt: **`&.` is unusable
  in this position** (`p(x&.m())` is `unterminated call`, because `&.` binds looser
  than the call's own parens); a nil test in a **ternary does not narrow**
  (`(x.nil? ? nil : x.m)` still reported `undefined method 'm' for Nil`); and
  **`try` is allowed on a non-nilable receiver** — which is what makes it the right
  answer, since Spinel's `TyKind` carries no nilability and `try` is exact for both.
  The block goes *inside* the braces (`x&.m { }` is a call of `m` with a block, so it
  reads `try { |t| t.m() do ... end }`); appending it after the closing brace produced
  a parse error.

**Measured effect (P22j).** **+3 PASS, +1 DIFF, −4 BUILDFAIL, +0 REFUSED**, zero PASS
regressions and **no REFUSED → BUILDFAIL trade** (gate PASS, 369 baselined):
`safe_nav_typed_receiver` (the trade P22i documented), plus
`case_when_string_range_nil_bound` and `yield_inline_ctor_self`, both unblocked
incidentally by the `try` rewrite and the nilable declaration. All three agree with
CRuby exactly. `safe_nav_arg_iterators` becomes a DIFF: it compiles now, and the
remaining disagreement (`[1, 2, 4, 5]` against CRuby) is a pre-existing gap, documented
rather than hidden. The ledger pins AGREE for both halves (`safe_nav_nilable_ivar`,
`conditional_ivar_is_nilable`).

**P22k — refuse a user-defined method on a statically unknown receiver (landed).**
This closes the second REFUSED → BUILDFAIL trade P22i documented.
`cr_translate_name` mapped `include?` → `includes?` for every non-Hash receiver,
which shadowed a user class's own `include?` into `undefined method 'includes? for
Tags`.

It is worth being precise about why that is **not** a rename bug, because the fix is
not a rename. The receiver there is a `Tags | Array(String)` union and the two arms
spell the method *differently*: `include?` is undefined for `Array(String)`, and
`includes?` is undefined for `Tags`. No single name resolves it — only boxed
dispatch could, which is the deferred project (see the phase notes). So the honest
step is a **named refusal**, and three cases are distinguished:

- a **Hash** receiver → `has_key?`, unchanged;
- a **concrete user object** that defines its own `include?` → keeps the Ruby name
  (`cr_user_method_ok`), since there is nothing to remap to;
- any other **concrete** receiver → still remaps to `includes?`. This case matters:
  `@names.include?(x)` on an `Array(String)` is correct even in a program that
  defines `include?` elsewhere, so the program-wide check must not fire on it;
- a receiver with **no concrete Crystal spelling** (a poly value) → refused by name.

A new `cr_any_method_named` answers the program-wide question, because
`cr_user_method_ok` matches on the *receiver's* class and is therefore silent for a
poly value.

**Measured effect (P22k).** **+0 PASS, +0 DIFF, −1 BUILDFAIL, +1 REFUSED**, zero PASS
regressions and no other transition (gate PASS, 369 baselined):
`test_poly_user_include_value` moves BUILDFAIL → REFUSED. The refusal is line-anchored
and names the obstruction. P22i's two trades are now both cleared. The ledger pins the
refusal.

## Ruby → Crystal divergences handled

These are real semantic differences between the two languages that the emitter
translates for the supported surface:

- **Integer division.** Ruby's `/` on two integers truncates; Crystal's `/`
  always yields a `Float`. The emitter emits `//` when both operands are integer
  types, `/` otherwise.
- **Method names.** A flat translation table rewrites the unambiguous ones:
  `length` → `size` (Array/Hash/String), `include?` → `includes?`.
- **Indexing.** `h[k]` is a `CallNode` named `[]`; emitted as Crystal bracket
  syntax `h[k]`, not `h.[](k)`.
- **Empty arrays.** Crystal cannot infer an empty literal's element type. When
  the inference leaves it poly (the common `a = []` then `a << x` inside a
  block), the emitter looks ahead to the first push and pins `[] of T`.
- **Ranges.** Emitted parenthesised (`(1..5)`) because `..` binds looser than a
  method call; the exclusive-end flag is bit 2 of the Prism range flags.
- **`Struct.new`.** A top-level `X = Struct.new(:a, :b)` becomes a Crystal
  reference-type `class`: a positional `def initialize(@a : T, @b : T)` assigns
  the members in order, and a reader/writer pair is generated for each. Ruby's
  `Struct` is a reference object, so a `class` (not the value-type `struct`
  keyword) matches its aliasing/mutation semantics. `Data.define` is *refused*,
  not translated: it is a value type with member-wise `==`, which a plain class
  answers by identity — emitting it would silently change `a == b`.

## What the corpus covers (and passes)

`examples/crystal_poc/` — 18 programs, each compared against CRuby:

literals, arithmetic (incl. `**`, int vs float division), `while`/`until`,
`if`/`elsif`/`else`/`unless`/ternary, recursion, top-level and class methods,
classes with ivars + `attr_accessor` + `initialize`, Array/Hash/String
operations, string interpolation, ranges (inclusive/exclusive), boolean logic,
and blocks (`each`/`map`/`select`/`sum`/`times`, `next`, nested blocks).

## Results

Measured 2026-09-28 on Crystal 1.21.0 / LLVM 20.1.8 (32-core Linux, gcc 13.3),
CRuby as the oracle:

```
programs: 18   pass: 18   diff: 0   build-fail: 0
sum poly_sites=1   sum unsupported=0
```

Every program's stdout is byte-identical to CRuby. `poly_sites` counts expression
nodes the inference left as `TY_POLY`/`TY_UNKNOWN`; `unsupported` counts
constructs outside the subset. Both are near zero across the corpus, which is the
core feasibility signal: **the analysis output was sufficient for a second
backend without modification.**

**Full-corpus baseline.** Run against the C backend's own corpus (top-level
`test/*.rb`), the Crystal target emits cleanly on a subset and refuses the rest
with a line-anchored diagnostic. The C backend compiles all of them, so the gap
is entirely the Crystal subset.

Working through Crystal 1.21's removal of `untyped` revealed that the earlier
"defer to dynamic dispatch on untyped receivers" strategy was not viable (there
is no `untyped` type). As a result, we converted poly-receiver calls and poly
containers to honest refusals rather than emitting parse errors. While this
lowered the emit-clean count relative to the Phase 2 interim (851 → 682), it
removed a large class of silent build failures and moved the boundary to honest,
measurable refusals.

Phase 2 lifted coverage by translating optional parameters, multi-assign, index
reads/writes, constants/constant-paths, modules (and nested ones),
`case`/`when`, `self`, and a widened method table; it also turned side-effecting
class/module bodies into honest refusals rather than silently reordered output.

Of the programs that still emit, the main build failures are now concentrated in
nilability mismatches (type is `(T | Nil)` but code expects `T`), a few method
differences, and the remaining poly/unknown container cases that we continue to
refuse. The top remaining refusal reasons are poly containers, unknown method
calls, keyword/rest parameters, block parameters, and `yield`. Driving the
emit-clean count up while keeping refusals honest is the work that follows.

**Measured results.** The table below is a same-corpus, same-harness before/after
so the deltas are meaningful: the baseline is commit `bd1f9b4a` built in a
worktree, and both sides run the top-level `test/*.rb` corpus through the fixed
`scripts/crystal_corpus.sh`. Building each emitting program and comparing its
output against CRuby gives:

| outcome | baseline | P2 | P3 | P4 | P5 | P6 | P7 | P8 | P9 | P10 | P11 | P12 | P13 | P14 | P15 | P16 | P17 | P18 | P19 | P20 | P21 | P22c | P22d | P22e | P22f | P22g | P22h | P22i | P22j | P22k | P22n | P22o+p | P22q | P22r | P22s | **P22t (current)** | meaning |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| corpus | 4417 | 4417 | 4418 | 4398 † | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | **4398** | programs run |
| emit-clean | 757 | 786 | 787 | 778 | 778 | 778 | 778 | 777 | 777 | 777 | 777 | 777 | 734 | 673 | 663 | 664 | 664 | 663 | 656 | 644 | 642 | 645 | 646 | 648 | 656 | 671 | 678 | 681 | 681 | 680 | 680 | 688 | 687 | 691 | 691 | **691** | emits Crystal at all (the rest refuse) ‡ |
| PASS | 196 | 256 | 257 | 264 | 268 | 278 | 289 | 294 | 297 | 300 | 303 | 305 | 307 | 307 | 308 | 315 | 321 | 325 | 325 | 327 | 327 | 328 | 333 | 340 | 348 | 364 | 365 | 366 | 369 | 369 | 378 | 396 | 397 | 401 | 402 | **403** | builds and output is byte-identical to CRuby |
| DIFF | 70 | 102 | 102 | 89 | 88 | 78 | 67 | 61 | 58 | 55 | 52 | 50 | 49 | 49 | 50 | 50 | 50 | 50 | 50 | 49 | 49 | 50 | 51 | 53 | 53 | 56 | 56 | 56 | 57 | 57 | 55 | 55 | 55 | 55 | 54 | **53** | builds, but disagrees with CRuby |
| BUILDFAIL | 491 | 428 | 428 | 425 | 422 | 422 | 422 | 422 | 422 | 422 | 422 | 422 | 378 | 317 | 305 | 299 | 293 | 288 | 281 | 268 | 266 | 267 | 262 | 260 | 252 | 258 | 257 | 259 | 255 | 254 | 247 | 237 | 235 | 235 | 235 | **235** | the emitted Crystal does not compile |
| REFUSED | 3659 | 3631 | 3631 | 3620 | 3620 | 3620 | 3620 | 3621 | 3621 | 3621 | 3621 | 3621 | 3664 | 3725 | 3735 | 3734 | 3734 | 3735 | 3742 | 3754 | 3756 | 3753 | 3752 | 3745 | 3745 | 3720 | 3720 | 3717 | 3717 | 3718 | 3718 | 3710 | 3711 | 3707 | 3707 | **3707** | refused at the Ruby line, with a reason |

‡ From P22h on, `emit-clean` is exactly PASS+DIFF+BUILDFAIL; earlier columns were
counted independently and can drift by a few programs (P22g shows 671 against 678).
P22l was tried and reverted (−3 PASS / 0 gained), and P22m is metrics-neutral, so
neither gets a column.

(Phase commits, for the column headers: P2 `a8455324`, P3 `2b722b6d`,
P4 `42923c6d`, P5 `d115b402`, P6 `a5eb7684`, P7 `f8ba43ac`, P8 `bc5dd918`,
P9 `a3fc1ffd`, P10 `d06db81e`, P11 `e35d1fb3`, P12 `9f4e8772`,
P13 `7725fc9d`, P14 `3adf0168`, P15 `c2f2eb1d`, P16 `ed1ea4ae`,
P17 `9c616578`, P18 `9b7ec7d3`, P19 `0c1b225c`; the P19 → P20 deltas in the
table above belong to the honesty pass `f1234994`, not to the boxed runtime,
which is metrics-neutral — P20 adds the runtime and its staged emitter seam and
is measured below. The P20 → P21 deltas are the container half: **+0 PASS,
+0 DIFF, −2 BUILDFAIL, +2 REFUSED**, zero PASS regressions.)

† Phase 4 drops `test/promote_*.rb` (20 programs) from the list — the same filter
the Makefile applies to the C backend's raise/wrap runs, because those tests only
have defined output under `--int-overflow=promote` and the Crystal target has no
promote mode. To keep the deltas honest they are measured on the same list:
Phase 3 restricted to the 4398 shared programs is PASS 257 / DIFF 99 /
BUILDFAIL 426 / REFUSED 3616. Against that, Phase 4 is **+7 PASS, −10 DIFF,
−1 BUILDFAIL, +4 REFUSED, with zero PASS regressions** — every changed program
moved from DIFF to PASS (a fixed translation), DIFF to a named refusal, or
BUILDFAIL to a named refusal. Phase 5 adds **+4 PASS, −1 DIFF, −3 BUILDFAIL,
+0 REFUSED**, Phase 6 **+10 PASS, −10 DIFF, +0 BUILDFAIL, +0 REFUSED**, and
Phase 7 **+11 PASS, −11 DIFF, +0 BUILDFAIL, +0 REFUSED**, Phase 8
**+5 PASS, −6 DIFF, +0 BUILDFAIL, +1 REFUSED**, Phase 9
**+3 PASS, −3 DIFF, +0 BUILDFAIL, +0 REFUSED**, Phase 10
**+3 PASS, −3 DIFF, +0 BUILDFAIL, +0 REFUSED**, Phase 11
**+3 PASS, −3 DIFF, +0 BUILDFAIL, +0 REFUSED**, Phase 12
**+2 PASS, −2 DIFF, +0 BUILDFAIL, +0 REFUSED**, and Phase 13
**+2 PASS, −1 DIFF, −44 BUILDFAIL, +43 REFUSED**, Phase 14
**+0 PASS, +0 DIFF, −61 BUILDFAIL, +61 REFUSED**, Phase 15
**+1 PASS, +1 DIFF, −12 BUILDFAIL, +10 REFUSED**, Phase 16
**+7 PASS, +0 DIFF, −6 BUILDFAIL, −1 REFUSED**, Phase 17
**+6 PASS, +0 DIFF, −6 BUILDFAIL, +0 REFUSED**, Phase 18
**+4 PASS, +0 DIFF, −5 BUILDFAIL, +1 REFUSED**, Phase 19
**+0 PASS, +0 DIFF, −7 BUILDFAIL, +7 REFUSED** — all with zero PASS regressions
and no change to any program other than the ones named below. The P20 runtime is
separately metrics-neutral (see [P20](#p20-the-boxed-value-runtime)); the
`+2 PASS, −1 DIFF, −13 BUILDFAIL, +12 REFUSED` the table shows against P19 is the
`f1234994` honesty pass. P21 (see [P21](#p21-widened-scalar-arrays)) is
**+0 PASS, +0 DIFF, −2 BUILDFAIL, +2 REFUSED** — the two BUILDFAILs whose
heterogeneous array held a container element are now honest refusals at the
offending push. P22c (see [P22](#p22-object-boxing-via-spbox)) is
**+1 PASS, +1 DIFF, +1 BUILDFAIL, −3 REFUSED** — the widened array now accepts a
boxable object/container element, which turns `poly_array_range_aref` into a PASS
and, on two other programs, unblocks the rest of a file whose remaining code
touches a pre-existing auto-splat / `with_index` gap (now visible). P22d is
**+5 PASS, +1 DIFF, −5 BUILDFAIL, −1 REFUSED** — the `with_index` chain
translation (see [P22](#p22-object-boxing-via-spbox)) closes the larger of those
two gaps. P22e is **+7 PASS, +2 DIFF, −2 BUILDFAIL, −7 REFUSED** — boxed
instance variables, gated so every trade is an improvement (see [P22](#p22-object-boxing-via-spbox)). P22f is **+8 PASS, +0 DIFF, −8 BUILDFAIL, +0 REFUSED** — class/module-body declarations, again with no REFUSED→BUILDFAIL trade (8 REFUSED → PASS and 8 BUILDFAIL → REFUSED). P22g is **+16 PASS, +3 DIFF, +6 BUILDFAIL, −25 REFUSED** — `yield` methods (see [P22](#p22-object-boxing-via-spbox)); it exposes 7 separate pre-existing gaps as REFUSED → BUILDFAIL, documented in its section. P22h is **+1 PASS, +0 DIFF, −1 BUILDFAIL, +0 REFUSED** — a comparison at a widened parameter boundary, the first step of the boundary-boxing direction P22b pointed at (see [P22](#p22-object-boxing-via-spbox)). P22i is **+1 PASS, +0 DIFF, +2 BUILDFAIL, −3 REFUSED** — compound assignment to a scalar instance variable, a weak trade whose two build errors are pre-existing gaps documented in its section. P22j is **+3 PASS, +1 DIFF, −4 BUILDFAIL, +0 REFUSED** — a conditionally-assigned ivar declared nilable, plus the `&.` → `try` rewrite that fix exposed (see [P22](#p22-object-boxing-via-spbox)); it clears one of P22i's two trades and adds no new ones. P22k is **+0 PASS, +0 DIFF, −1 BUILDFAIL, +1 REFUSED** — a call to a user-defined method on a statically unknown receiver is refused by name rather than emitted as a build error, closing P22i's second trade (see [P22](#p22-object-boxing-via-spbox)). P22l was tried — receiver dispatch on a widened parameter — measured at **−3 PASS / 0 gained**, and reverted (see its post-mortem in the P22 section). P22m is **metrics-neutral** (+0 everywhere) — the `SpBox#sp_send` seam, landed additive and verified by direct execution. P22n is **+9 PASS, −2 DIFF, −7 BUILDFAIL, +0 REFUSED** — the superclass carried into the class header. P22o+p is **+18 PASS, +0 DIFF, −10 BUILDFAIL, −8 REFUSED** — the inheritance call path completed and `super` translated (see [P22](#p22-object-boxing-via-spbox)); four latent build errors the newly-real relation exposed became named refusals, one DIFF became a PASS, and the single remaining REFUSED→BUILDFAIL (`class_value_new_optional_param`) is the documented class-value-constructor limitation. P22q is **+1 PASS, +0 DIFF, −2 BUILDFAIL, +1 REFUSED** — parse-clean emission (symbol quoting, do/end-block calls in operand position) plus the implicit-parent `super` whitelist, landed with one BUILDFAIL→PASS and one BUILDFAIL→REFUSED (see [P22](#p22-object-boxing-via-spbox)). P22r is **+4 PASS, +0 DIFF, +0 BUILDFAIL, −4 REFUSED** — the first desugared-Enumerable passthrough, scoped to the methods whose Crystal stdlib form is byte-identical; the broader candidate set was measured at 21 PASS / 5 DIFF / 25 BUILDFAIL and rejected as dishonest (see [P22](#p22-object-boxing-via-spbox)). P22s is **+1 PASS, −1 DIFF, +0 BUILDFAIL, +0 REFUSED** — a DIFF-cluster pass that inventoried all 55 `DIFF`s by root cause and fixed the one clear emitter bug (adjacent interpolated literals were emitted empty); the other 54 are analysis- or runtime-level gaps recorded in the plan (see [P22](#p22-object-boxing-via-spbox)). P22t is **+1 PASS, −1 DIFF, +0 BUILDFAIL, +0 REFUSED** — a `{ **h, k: v }` hash literal is now emitted as a `.merge` chain (Crystal has no hash-literal double-splat; the splat was previously emitted as `nil => h`), plus a defensive refusal for an unmodeled interpolation part (see [P22](#p22-object-boxing-via-spbox)).

Phases 1–2 together are **+60 PASS over the baseline with zero PASS regressions**
(every program that passed on the baseline still passes), and Phase 3 adds a further
+1 (`test/struct_member_write.rb`) on a corpus one program larger — 4417 → 4418 — also
with zero PASS regressions and **no status change on any of the 4417 shared programs**.
The emit-clean count rose by 29 through Phase 2: the honesty-gate work
below turns programs that previously emitted broken Crystal into line-anchored
refusals, and the method sweep, regex-literal support, the `attr_accessor` fix
(below), and the `Struct.new` translation (below) add most of them back. Phase 3
left emit-clean unchanged — `[]=` needs no new typing — and the newly agreeing cases
are the focused `[]=` tests. The new
DIFFs are the remainder — programs
that now compile but disagree with CRuby on an edge case (empty `min`/`max`,
`Array#fill` with a Range, blockless `each_index`, `String#tr` ranges,
numeric-string `upto`, `squeeze` with a charset argument, `transform_values` block
form, `chunk_while` on a Range, and a few regex edge cases; the `min`/`max`,
`tr`-range and `squeeze`-charset ones were cleared by the later phases below). A
DIFF is a strictly better state than a BUILDFAIL: the translation compiles, and
the remaining gap is a named, measurable behavior difference rather than a broken
file.

Phase 4 then went after the worst DIFFs — the ones where the emitted Crystal
*raises at run time* while CRuby completes — and found that all of them were bugs
in our own translations rather than inference gaps, so each has a small,
line-anchored fix:

- **`String#chars`.** Ruby yields an array of one-character **Strings**; Crystal
  yields `Char`. The analyzer already models the result as a string array, so a
  later use of the element as a String (a hash lookup, `==` against a literal)
  raised. The blockless form now emits `.chars.map(&.to_s)`; the *block* form
  cannot be rewritten without changing what the block parameter is, so it is
  refused. This fixed `issue_2903` and `issue_2929`.
- **Named format references.** `"%<n>d" % {n: 9}` is passed through to Crystal's
  `String#%` (i.e. `sprintf`), whose named references index the hash with a
  **String** key (`string/formatter.cr`). Ruby reads the symbol-keyed hash we emit,
  so it raised `KeyError`. A literal format with `%<name>` / `%{name}` now emits
  `arg.transform_keys(&.to_s)`. This fixed `format_named`.
- **`String#to_i(base)`.** Crystal's `to_i` is `strict: true` by default (raising
  `ArgumentError` where Ruby answers `0`) and rejects base `0` and `_` separators.
  An explicit-base call now emits `strict: false, underscore: true` and, for base
  `0`, `prefix: true, leading_zero_is_octal: true` (Crystal rejects base 0, so the
  default valid base is kept and prefix detection does the autodetect per string),
  with a `{ 0 }` block for Ruby's "no valid digits means 0". This fixed
  `string_to_i_base` and `string_to_i_base_zero`. The same `strict` default affects
  bare `s.to_i`; that shape is far more common (114 programs) and is left for a
  separate pass.
- **`Float#to_i`.** Crystal's `Float#to_i` returns an `Int32` and raises past
  2³¹; Ruby's returns a full Integer. Float receivers now emit `.to_i64`, which
  matches Ruby across the Int64 range; a float literal outside that range is
  refused (Ruby returns a Bignum, Crystal raises `OverflowError`).
- **Named capture reads.** `s[/re/, "name"]` answers `nil` in Ruby when the group
  did not participate; Crystal's `String#[]` declares a non-nilable `String` and
  asserts. It now emits the `[]?` form. This fixed `issue_3082`.
- **`min`/`max` on empty.** Crystal raises `Enumerable::EmptyError`; Ruby answers
  `nil`. In a nil-tolerant position (`min?` / `max?`) it now answers nil. This
  fixed `empty_int_array_min_max`.

Two constructs that could only differ at run time are now refused rather than
shipped as a crash. A `sub`/`gsub` replacement using Ruby's `\&` (whole match) or
`\k<name>` (empty when the group did not participate) is refused: Crystal's
replacement parser leaves `\&` literal and *raises* for a non-participating
`\k<name>`, so the two languages are not interchangeable. And an integer that
would overflow `Int64` at run time cannot be caught — see the integer-width
limitation below — so the `promote_*` tests, which exist only to exercise
`--int-overflow=promote`, are excluded from the Crystal corpus as they already are
from the C backend's raise-mode runs.

Also fixed along the way: the exception-class refusal now **names the class** it
is refusing (`the exception class 'FrozenError' has no Crystal equivalent: …`),
where before the message only listed what Crystal *does* define.

Phase 5 closed two follow-ups that Phase 4 surfaced:

- **Operator precedence in receiver position.** The emitter never parenthesized
  a subexpression that is itself an infix/prefix operator, so `(2 * 3).to_s`
  emitted `2_i64 * 3_i64.to_s()` (which Crystal parses as `2 * (3.to_s)`) and
  `(h[1] + h[2]).to_s` compiled to something else entirely. Ruby and Crystal
  share operator precedence, but Crystal binds `.` (and `[`) tighter than every
  operator, so an operator node in receiver *or operand* position (`a * (b + c)`)
  needs explicit parentheses. `cr_emit_recv` now wraps such a node at every
  receiver site (`[]`, `[]=`, `at`, the generic call, `min`/`max`, `chars`,
  `to_i`) and on both operands of a binary operator. This turned three
  BUILDFAILs and one DIFF into PASS (`empty_hash_int_int_promote`, `gsub_sub_block`,
  `at_exit_hook_survives_collection`, `issue_2913`) with no output change
  anywhere else.
- **Bare `String#to_i`.** Phase 4 fixed only the explicit-base form; a bare
  `s.to_i` still raised `ArgumentError` on non-numeric input where Ruby answers
  `0`. It now emits `s.to_i(strict: false, underscore: true) { 0 }`, matching
  Ruby on trailing garbage, `_` separators, empty strings, and whitespace, while
  leaving the default base in place.

Phase 6 then cleared the largest remaining coherent DIFF clusters:

- **User-defined `to_s`/`inspect` were ignored.** Crystal renders an object
  through `to_s(io : IO)` / `inspect(io : IO)`, and every Ruby `to_s`/`inspect`
  is no-arg, so an emitted no-arg override was what `obj.to_s` returned but never
  what `puts obj`, `"#{obj}"` or `p obj` reached — those used Crystal's default
  `#<Obj:0x…>`. For a no-arg `to_s`/`inspect` the emitter now also emits a
  delegating `def to_s(io : IO) : Nil; io << to_s; end` (same for `inspect`), so
  the program's method is the one the formatters use. Fixed `value_type_interp_to_s`,
  `puts_object_to_s_poly`, `value_obj_boxed_to_s`, `puts_object_nested_new_arg`
  and `i994`.
- **Nested `Array#join`.** Ruby's `join` recursively flattens nested Arrays;
  Crystal's renders a nested Array with its inspect form. Array receivers now
  emit `.flatten.join(…)`, and `flatten` recurses through exactly the same set
  (Arrays, not other enumerables). Fixed `array_join_nested`.
- **Charset arguments.** Ruby's `tr` expands `a-z` ranges; Crystal's does not, so
  a literal charset is expanded at emit time. And a lone `"^"` is a literal caret
  in Ruby while Crystal's `delete`/`count`/`squeeze` negate the empty set and
  match everything, so it is emitted escaped. Fixed `string_charset_escape`,
  `string_charset_range_notation`, `str_delete_count_negation` and `str_squeeze_arg`.

Phase 7 closed the **Hash inspection** cluster — the single largest remaining
group. Crystal prints a symbol-keyed hash as `{:a => 1}`; CRuby 4.0 prints
`{a: 1}` for a plain-label key and `{"a-b": 1}` for one that is not, and `p`,
`puts` and `#{}` all go through `inspect`/`to_s`. Since the C backend already
reproduces CRuby's spelling byte-for-byte, every generated program now carries a
small `Hash#inspect`/`#to_s` shim that matches it (a key is a label when it is a
letter or `_` — or any non-ASCII codepoint — followed by letters/digits/`_`, with
an optional trailing `?`/`!`; operators, `@x`, `$y` and `1abc` stay quoted). This
is the one place the emitted program reopens a core class; it changes only
formatting, and the full corpus shows no PASS regression. It resolved
`array_to_h_pairs`, `begin_value_body_local`, `each_with_index_to_h`,
`hash_block_methods_variants`, `hash_inspect_poly`, `hash_transform_keys_cross`,
`issue_3138`, `p_as_expression`, `print_puts_expression_and_kinds`,
`to_h_with_block` and `typed_hash_inspect_and_each_value`, and let the
`struct_to_h_print` difference-ledger entry be promoted from DIVERGE to AGREE.

Phase 8 closed most of the **Regexp** cluster:

- **`Regexp#to_s` / `#inspect`.** CRuby renders a pattern as `(?<on>-<off>:src)`
  for `to_s` (and `"#{/re/}"`), with the flag letters in the order `m,i,x`
  enabled-first, and as `/src/flags` for `inspect`. Crystal prints
  `(?-imsx:...)` — its own flag set (`s` for dotall) and order — and orders
  inspect's flags `i,m,x`. The emitted `Regex#to_s` / `#inspect` shim reproduces
  CRuby's exact spelling (and its `/`-escaping). Fixed `issue_3061`,
  `regexp_render_source_variable`, `regexp_string_interpolation`.
- **`Regexp#options`.** Ruby returns the Integer bitmask (1=IGNORECASE,
  2=EXTENDED, 4=MULTILINE); Crystal returns its `Regex::Options` enum, which the
  stdlib uses internally and so cannot be overridden. `.options` on a regex
  receiver is rewritten to the shim's `ruby_options`. Fixed `regexp_source_options`.
- **`\h` / `\H`.** Ruby's are the hex-digit class; PCRE2 reads them as horizontal
  whitespace (and its negation), so the pattern is rewritten at emit time,
  splicing the members inside a character class and wrapping them outside. Fixed
  `regex_hex_shorthand`.
- **Sub/gsub pre/post-match backreferences.** `` \` `` and `\'` (and `\+`, the
  last group) are expanded by Ruby and left literal by Crystal, so they join
  `\&` and `\k<name>` in the honest refusal. `sub_prematch_backref` moves
  DIFF → REFUSED.

`test/regexp_split_compat.rb` stays a documented DIFF: CRuby's `split(regex)`
(zero-width matches split into characters, capture groups included, trailing
empties dropped unless the limit is negative) cannot be reproduced by Crystal's
own `split`, and a blanket refusal would regress the passing
`test/string_split_capture.rb`, so the one program is left visible rather than
traded for a correct one.

Phase 9 cleared three Ruby-shape mismatches:

- **`Array#values_at` / `zip`.** Crystal answers a Tuple (and, for `zip`, an
  Array of Tuples) where Ruby answers an Array, so the result was rendered and
  compared with `{}` instead of `[]`. Both are now followed by `.to_a` /
  `.map(&.to_a)`, which keeps a later `|a, b|` destructuring working. Fixed
  `values_at` and `issue_2995`.
- **`String#split(" ")`.** A literal single space selects Ruby's whitespace
  (awk) mode — leading whitespace stripped, runs collapsed — which is exactly
  Crystal's no-argument `split`; the literal-space form was splitting on spaces.
  It is rewritten to the no-arg form (carrying a limit when present). Fixed
  `str_split_space_whitespace`.

The rest of the tuple/iterator tail (`each_with_index` and `chunk` yielding
Tuples, `each`/`each_slice` returning nil instead of the receiver, un-materialized
lazy iterators) needs the emitter's iterator/materialization machinery reworked,
and is left as tracked DIFFs (the whitespace/limit/trailing-empty `split` cases
were later closed by Phase 11).

Phase 10 was a contained batch of two stdlib-semantics fixes:

- **`ENV[k]`.** Ruby's `ENV` is a String-keyed Hash: a missing key answers `nil`.
  Crystal's `ENV[]` raises, but `ENV[]?` answers nil and `ENV[]=` already accepts
  nil to delete — so the read is emitted with `?`. Fixed `env_nil_string_to_s` and
  `env_string_inference`, and closes the `ENV[]` entry in the gap list below.
- **`Math.lgamma`.** Ruby answers `[log|Gamma(x)|, sign]`; Crystal answers the log
  alone. The emitted call is paired with a synthesized `sp_lgamma_sign` (the sign
  is `-1` on the negative intervals where `floor(x)` is odd and at `-0.0`, `+1`
  otherwise), which reproduces CRuby's `lgamma_r` convention at the poles. Fixed
  `issue_3116`.

Phase 11 closed the **`String#split`** cluster (everything but the regex-
separator case). Ruby and Crystal's `split` disagree on three axes at once — a
single-space or `nil` separator is whitespace (awk) mode with leading whitespace
stripped and runs collapsed; the default limit drops trailing empty fields; and
a positive limit caps the field count with the unsplit remainder last while a
negative limit keeps trailing empties. The Crystal emitter now routes a String
(or `nil`, or no) separator through `sp_ruby_split`, a direct port of the C
backend's `sp_str_split_{ws,ws_limit,drop_trailing,limit}` pair (so the two
backends share one definition of the semantics), and a block form yields each
field via `.each`. Fixed `str_split_trailing_empty`,
`str_split_space_dynamic_limit` and `string_split_block`. A Regexp or Char
separator keeps the plain path, so `regexp_split_compat` remains a DIFF.

Phase 12 took on the **iterator / Tuple** cluster. Two contained fixes landed: a
blockless `each_index` is a lazy `Iterator` in Crystal, so `each_index.map { }`
returned a `MapIterator` where Ruby returns an Array — it is now materialized
with `.to_a`; and `Enumerable#first(n)` on an iterator answers a lazy iterator, so
`first(n)` now gets a trailing `.to_a` (identity for an Array). Together these
fixed `issue_3160` and `issue_3171`.

The rest of the cluster is genuinely blocked, and the reason is worth recording
because it is the same shape in every case: **Crystal's Tuple is Ruby's pair
Array, and the two are only interchangeable on one side.** `each_with_index`,
`with_index`, `zip` and `chunk` yield `{a, b}` Tuples where Ruby yields `[a, b]`
Arrays. A whole-capture or `p` of the pair wants the Array spelling (`{}` vs
`[]`), but a destructuring block (`|a, b|`) works on a Tuple by auto-splat and
fails on an Array (Crystal does not auto-splat arrays). Since Ruby's `Pair` has
no static type, no single emission is right for both uses of the same call —
`enumerator_with_index_gc` even contains both in one program. Converting to an
Array also erases the positional types (`Tuple#to_a` is a union element type),
so `chunk { } .map { |k, xs| ... }` stops compiling. The remaining programs
(`each_with_index_reduce_seed`, `enumerator_with_index_gc`, `issue_3225`,
`poly_builtin_arm_container_read`) need either a per-`Tuple` inspect override,
which Crystal does not allow (`Tuple` cannot be reopened), or a context-aware
shape choice in the emitter. The `each`/`each_slice`-returns-the-receiver cases
(`range_each_slice_returns_receiver`, `enumerator_*`) are blocked for a second
reason: the desugarer materializes the receiver to `.to_a` before the call, so
wrapping the call in `.tap` would return the materialized array, not Ruby's
original receiver — and Ruby's `Enumerator` inspect format has no Crystal
counterpart anyway.

Phase 13 was an **honesty pass on the `BUILDFAIL` bucket**, which had grown to
422 — larger than PASS. The dominant cause (206 of 422) was `undefined method`:
the honesty gate is a *name* allowlist plus a 2-entry receiver-typed table, so a
method Crystal lacks on the *receiver's type* slipped through whenever the
receiver was a local rather than a bare constant — `t.value` on a Thread,
`t.utc`/`strftime` on a Time, `re.match?(..)` on a Regex, a Fiber transfer. Those
compiled to "undefined method" instead of refusing at the Ruby line. The
receiver-typed table now carries the Thread / Time / Fiber / Regex methods
Crystal genuinely lacks (the class-receiver shapes — `Thread.value`,
`Time.mktime` — were already in its `mod` table). Two Time methods Crystal *does*
have equivalently are remapped rather than refused (`utc` → `to_utc`,
no-argument `getlocal` → `to_local`); the mutating `gmtime`/`localtime` and the
argument-taking `getlocal(offset)` have no equivalent and are refused. Net:
**42 BUILDFAILs became line-anchored refusals, 2 became PASS, 1 DIFF became a
refusal, and no PASS regressed** — emit-clean fell (777 → 734) by design, since
those 42 programs were never actually building.

The same phase made the metrics reproducible and regression-proof:

- `scripts/crystal_corpus_list.sh` emits the corpus list deterministically
  (top-level `test/*.rb`, minus `test/promote_*.rb` and `test/crystal_struct.rb`),
  so the numbers above can be rebuilt from a clean tree.
- `test/crystal_corpus_baseline.txt` is a committed snapshot, and
  `scripts/crystal_corpus_gate.sh <results.txt>` fails if any baselined PASS
  stops passing (other transitions are reported, not failed). This is the
  zero-regression rule every phase here has been held to, now executable.

Phase 14 continued the same honesty sweep on the remaining `undefined method`
tail. The receiver-typed table grew three kinds of rule:

- **Class-object reflection** — `Dog.ancestors`, `Dog.superclass`,
  `M.const_get`, `Dog.method_defined?`, … checked *before* the user-class shadow
  test, because reflection over the program's own class has no Crystal form
  either.
- **Container methods Crystal lacks** — `Array#slice!`/`#append`/`#prepend`,
  `Hash#default=`/`#replace`/`#fetch_values`/`#flatten`/`#value?`/`#key?`,
  `String#sum`, keyed on the container *family* so every element type is covered.
- **Methods Crystal's Object simply has not** — `respond_to?`, `eql?`,
  `equal?`, `then`, `yield_self`, `freeze`, `frozen?`, `send`, `public_send` —
  refused by name, since a concrete receiver is already caught by the allowlist
  but a poly/untyped one reaches dynamic dispatch and fails to compile. `Integer#[]`
  (a bit read) and `String#[]=` (immutable) are guarded directly, because `[]`
  and `[]=` are special-cased ahead of the table.

The safety property is simple: a method Crystal lacks can never appear in a PASS
program, so these refusals cannot regress one. Net **61 `BUILDFAIL`s became
refusals, with zero PASS or DIFF change** — emit-clean fell 734 → 673, again by
design.

Phase 15 fixed two real emitter bugs in string interpolation and refused the
missing Ruby exception classes:

- **Literal `#{` in a string.** A Ruby string holding a literal `#{` (from a
  single-quoted literal or an escaped interpolation) was emitted verbatim into a
  Crystal double-quoted string, where `#{` opens an interpolation — so the
  literal was `Unterminated string interpolation`. `cr_emit_escaped` now escapes
  the `#` of a literal `#{`.
- **Multi-statement interpolation.** `"#{a; b; c}"` was emitted as several
  statements inside `#{}`, which Crystal's grammar does not accept. The sequence
  is now wrapped in `begin ... end` (a Crystal expression whose value is the last
  statement), preserving Ruby's run-every-statement / value-is-the-last rule.
  Fixed `interpolation_multistmt` (BUILDFAIL → PASS) and moved
  `interp_single_buffer` to a DIFF (its remaining difference is Float notation).
- **Ruby exception classes with no Crystal name** — `StandardError`,
  `TypeError`, `NameError`, `ZeroDivisionError`, `LocalJumpError`,
  `SignalException`, `UncaughtThrowError` and ~15 more — are now refused in a
  *value* position (constructing one, `is_a?`, a class value). A `rescue` clause
  still maps `StandardError → Exception` and `ZeroDivisionError →
  DivisionByZeroError`; only the bare reference has no Crystal form.

Phase 16 fixed **symbol folds** — the most PASS-productive phase since the
DIFF work. Ruby's `reduce(:+)` / `inject(seed, :&)` names the fold by a Symbol.
The shared desugarer rewrites an identifier-named symbol (`:gcd`, `:merge_with`)
to a block, but deliberately leaves an **operator** symbol for the C backend's
typed fold emitters — which the Crystal emitter does not have, so it emitted
`reduce(:+)`, invalid in Crystal. The Crystal emitter now spells those as a
block (`reduce { |a, b| a + b }`), and maps the `inject` name to `reduce`
(Crystal has no `inject`). A symbol fold over a **Hash** is refused: Ruby folds
`[k, v]` pair Arrays, Crystal's `Hash#reduce` yields `{k, v}` Tuples whose `+`
grows the Tuple type. Net **6 BUILDFAIL → PASS and 1 REFUSED → PASS** (a program
that was refused only because it used `inject`), with zero regressions.

Phase 17 took the small **wrong-arity** remaps where Ruby's form differs from
Crystal's by an argument Crystal does not take:

- `Enumerable#count` with no argument and no block is the size; Crystal's
  `count` requires an item or a block. Emitted as `.size`.
- `Hash#fetch(key)` with no default raises `KeyError` on a miss — exactly
  Crystal's `h[key]`; Crystal's own `fetch` requires a default.
- `Array#values_at` with no index is `[]`; emitted as `.first(0)`, an empty
  Array of the right element type (Crystal's `values_at` needs an index).
- `File.delete` / `File.unlink` with no path delete nothing and answer `0`.
- `exit(true)` / `exit(false)` map to status `0` / `1`.

**6 `BUILDFAIL → PASS`**, zero regressions.

Phase 18 implemented the **`ndigits` rounding** forms:

- **`Integer#round/ceil/floor/truncate(n)`.** Crystal's take no argument. `n >= 0`
  is self; `n < 0` rounds to `10**(-n)` with *exact* integer arithmetic (a float
  form loses precision above 2⁵³, and round is half-away-from-zero) via emitted
  `sp_int_*` helpers. A literal `n` past `10**18` needs a Bignum, so it is
  refused (`int_round_large_precision`).
- **`Float#round/ceil/floor/truncate(n)`.** Crystal has `Float#round(n)` (which
  accepts negative `n`) but no digit-taking ceil/floor/truncate, so `sp_flt_*`
  scale-round-scale. CRuby returns an **Integer** for a literal `ndigits <= 0`
  and for the no-argument forms, while Crystal's `Float#round/ceil/floor` answer
  a `Float64` and it has no `truncate` — so `.to_i` is appended in exactly those
  cases.

**4 `BUILDFAIL → PASS`** (plus a bonus: the long-standing `issue_2926` DIFF on
`round` of a poly value now agrees), −5 BUILDFAIL, +1 refusal, zero regressions.

Phase 19 refused three constructs Crystal structurally cannot host:

- **A user-defined `!`.** Crystal's `!` is a pseudo-method the compiler refuses
  to let a program redefine, so a Ruby `def !` has no equivalent.
- **A blockless `loop`.** In Ruby it is an infinite Enumerator (yielding nil);
  Crystal's `loop` requires a block and has no equivalent enumerator.
- **`is_a?` / `kind_of?` / `instance_of?` with a runtime class value.** Crystal's
  take a *type*, so a variable holding a class cannot be passed (a constant is
  still emitted as-is).

Also fixed a latent emitter bug: Crystal rejects `_` as a *method-parameter*
name (only block params may be `_`), so a lone underscore is now renamed
consistently at every binding site. **7 `BUILDFAIL → REFUSED`**, zero regressions.

Phase 20 was the final honesty pass — a mix of refusals and one last remap:

- **Refused, no Crystal counterpart:** `Marshal`, the `MatchData` constant
  (Crystal spells it `Regex::MatchData`), `Fiber.yield` (Crystal's takes no
  value), `Float::MAX`/`MIN`/`EPSILON`, `Process::CLOCK_MONOTONIC`/`CLOCK_REALTIME`,
  the `File::RDONLY`-family open flags, and `Time.utc`/`Time.gm` with the 7th
  (microseconds) argument.
- **Translated:** Ruby's `Numeric#step(limit, step)` takes positional bounds;
  Crystal's takes named `to:`/`by:`, so the two are rewritten across. This
  turned `float_step_block` and `range_step_endless` into PASS (the latter's
  `10.step(1, -2)` / `1.step(5)` lines were the build failure); `float_step_to_a`
  becomes a DIFF because Crystal's `step` walks *toward* `to` regardless of sign
  while Ruby walks only in the sign's direction.

Net **+2 PASS, −1 DIFF, −13 BUILDFAIL, +12 REFUSED**, zero PASS regressions. Two
Fiber programs moved DIFF → REFUSED (they were emitting non-deterministic output
for an out-of-subset feature).

Regex literals (`/pattern/`, including `/i /m /x` flags and `#{}` interpolation)
translate to Crystal's native regex literal, which is the same syntax; the pattern
text drops in verbatim with only the `/` delimiter re-escaped (the unescaped source
resolves `\/` and `%r{}` slashes to a bare `/`). This unblocked 57 programs. Most of
the new BUILDFAILs it exposed are not regex bugs: the dominant one is a nilable
`MatchData` — `m = str.match(/re/); m[1]` fails because Crystal types the result as
`(Regex::MatchData | Nil)` and will not dispatch a method on it without a guard. That
is the separate nilability gap, now visible instead of hidden behind a regex refusal;
the rest are patterns Crystal's engine genuinely cannot compile (named backreferences
 `\k<n>`) or a `MatchData` constant that needs a `Regex::MatchData` remap.

A top-level `X = Struct.new(:a, :b)` is emitted as a Crystal reference-type
`class`: the analyzer already models it as a user class (`is_struct`, members as
ordered ivars with readers/writers), so the emitter adds one synthesized
`def initialize(@a : T, @b : T)` whose ivar-params assign the members positionally,
and suppresses the verbatim `X = Struct.new(…)` write. A `class` (not Crystal's
value-type `struct` keyword) is deliberate: Ruby `Struct` instances are reference
objects, so aliasing and in-place mutation (`arr[i].x = …`) must survive. On top of
the constructor and readers/writers the emitter generates the feature surface:
`to_a`/`values` (member values in order), `.members` (symbol list), `to_h`
(symbol-keyed, `=>` form), member-wise `==` (typed to the class so a foreign
operand still falls through to identity), and `[]` / `[]=` by symbol / string /
integer position (negative counts from the end). Methods written in a block-form
`Struct.new(…) do … end` are emitted too. This moves the pure-Struct programs from a
broken verbatim emit to PASS. `[]=` is emitted as **one overload set per distinct
member type** — grouped by the *mapped* Crystal spelling, so `Int32` and `Int64`
members share one — and each overload's `case` arms list only that type's members.
That keeps it cast-free: a heterogeneous struct needs no `union` and no `.as`, and a
store of the wrong type fails overload resolution rather than writing a value under a
mislabelled member. Each arm ends in the assignment, so the setter answers the stored
value, matching Ruby's `s[k] = v`. `to_h` prints with CRuby's symbol-key spelling
because the emitted `Hash#inspect` shim renders `{a: 1}` (Phase 7; it was a
documented DIFF before). One Crystal/Ruby printing difference remains a documented
DIFF: `puts <struct>` uses Crystal's default object formatting rather than a
custom `to_s`. A third difference is the *class* of the error for an
unknown member **name**: Ruby raises `NameError`, but Crystal 1.21 defines no such
class (its whole hierarchy is `Exception`/`ArgumentError`/`IndexError`/`KeyError`/…),
so `[]` and `[]=` answer `IndexError`. Both are errors; only the class differs, and it
is pinned in `scripts/crystal_difference_test.sh` as
`expect_diverge "struct_member_name_error"`. An out-of-range **index** is `IndexError`
on both sides, so that case agrees.

Still not translated (they build-fail on the missing method): `dig` (recursing into
a member needs every member type to answer `dig`, which a heterogeneous struct cannot
offer), `values_at`, and a
`Struct#new` called with fewer arguments than members (Ruby fills the rest with
nil — nilable ivars would break every member that is read in arithmetic, so that
trade was measured and rejected). `Data.define` is deliberately *not* translated: it is a value type whose `==`
compares members, and a plain class answers `==` by identity, so emitting it would
silently change results; it stays an honest refusal until member-wise equality lands.

Reproduce with `scripts/crystal_corpus_list.sh` (the deterministic file list) and
`scripts/crystal_corpus.sh` (the runner):
`scripts/crystal_corpus.sh "$(scripts/crystal_corpus_list.sh)" /tmp/run 12`.
Check the result against the committed baseline with
`scripts/crystal_corpus_gate.sh /tmp/run/results.txt`, which fails on any PASS
regression. The runner gives every build its own `CRYSTAL_CACHE_DIR` (a shared
cache across parallel builds corrupts itself and manufactures ~90 bogus "compiler
bug" reports) and compares outputs serially after the builds finish. The harness keys work files by repo-relative path, not
basename: the corpus has duplicate basenames in subdirectories
(`test/main.rb`, `test/require/main.rb`, `test/collect/seed/main.rb` all basename
to `main`), and a basename key silently overwrote one with another, so four
programs were never built or compared and every reported total was short by four.
It also reads the file list on a dedicated file descriptor and gives each program
`/dev/null` on stdin, so a program that reads stdin cannot consume the list and
truncate the run.

### The honesty gate: refuse, do not ship broken Crystal

Crystal's type system and stdlib differ from Ruby's enough that a direct
translation of a name can produce Crystal that either fails to compile with a
message about the wrong thing, or compiles and silently disagrees. The emitter
therefore refuses a construct it cannot translate, at the Ruby source line, and
says why. Two receiver-aware tables drive this, consulted after the program's own
methods and classes have had priority:

- `cr_missing_const` refuses a constant Crystal's stdlib does not define
  (`Data`, `Enumerator`, `Queue`, `Socket`, `TCPServer`, `RUBY_ENGINE`,
  `Float::NAN`, ...). `Float::INFINITY` is the one constant with an exact Crystal
  spelling (`(1.0 / 0.0)`), so it is remapped rather than refused. A program's own
  `class Data` still wins, because the check runs after the user-class lookup.
- `cr_missing_method` refuses a method that does not exist on *this* receiver,
  which a name-only allowlist cannot see: `String#<<` is undefined in Crystal
  while `Array#<<` is fine, `GC.start`/`GC.stat`/`Thread#value`/`File.stat`/
  `File.lstat`/`Range#lazy`/`Mutex#locked?`/`Hash#freeze` have no counterpart, and
  the receiver may be a class (`Time.at`), a constructor call
  (`Thread.new{}.value`), or a typed value.
- `cr_call_remap` rewrites the few calls whose Crystal form has a different shape
  rather than a different name: `Time.at(n)` → `Time.unix(n).to_local` (Ruby's is
  local time, Crystal's is UTC, so the zone is carried explicitly),
  `Time.now` → `Time.local`, `Dir.rmdir` → `Dir.delete`. `Time.at` is only
  rewritten for a single integer argument; a fractional or sub-second argument has
  no Crystal form and is refused.

A few names are emittable for some shapes and refused for others, because the
Crystal form would be wrong rather than merely missing:

- `String#chars` with a **block** is refused (Crystal yields `Char` where Ruby
  yields a one-character String); the blockless form is rewritten to
  `.chars.map(&.to_s)`.
- A `sub`/`gsub` replacement literal using `\&` or `\k<name>` is refused; Crystal's
  replacement language leaves `\&` literal and raises for a non-participating
  `\k<name>`.
- `Float#to_i` on a literal outside the Int64 range is refused (Ruby answers a
  Bignum; Crystal raises `OverflowError`).

The method allowlist was extended only with names verified by a build-and-run
comparison against Ruby, not merely by compiling. The first batch: `match`, `=~`,
`Exception#message`, `Class#name`, `Kernel#at_exit`, and the data methods
`fill`/`each_index`/`max`/`min`/`map!`/`slice_when`/`upto`/`itself`/`dig`/`tr`.
A second sweep added `compact`/`flatten`/`chunk_while`, the Hash methods
`each_key`/`each_value`/`transform_values`/`transform_keys`/`invert`/`clear`, and
`squeeze`/`succ`. Each was exercised on a realistic receiver and its output diffed
byte-for-byte against CRuby.

Names whose Crystal version is *not* Ruby's are deliberately left refused —
`Object#class` (`5.class` is `Integer` in Ruby, `Int32` in Crystal), `String#scan`
(Ruby yields Strings, Crystal yields `MatchData`), `String#chr`/`each_char`/`lines`
(Char vs String, line terminators), the methods Crystal simply lacks (`match?`,
`freeze`, `frozen?`, `inject`, `slice`, `slice!`, `lazy`, `send`, `equal?`,
`union`, `difference`, `take`, `except`, `default`, `delete_if`, `pow`, `ord`,
`casecmp`, `swapcase`, `chop`, `to_sym`, `intern`, `delete_suffix`,
`reverse_each`, `then`, `yield_self`), and the methods that return a Tuple where
Ruby returns an Array (`product`, `divmod`, `rpartition`, `partition`, `minmax`).
Three more were verified on their natural receiver but left out because the
allowlist is name-based, not receiver-aware: `chomp` (fails once allowed on a
nilable receiver such as `gets.chomp`), `modulo` (Crystal's `%` needs matching
Int/Float types), and `to_h` (fails on a Struct receiver). For each of those a
clear refusal is better than the build error it would cause.
`scripts/crystal_reject_test.sh` pins the refusal messages.
`scripts/crystal_difference_test.sh` goes further: for each named Ruby-vs-Crystal
difference it pins the honest disposition — AGREE (translated, output matches
CRuby), REFUSE (cleanly refused, naming the construct), or DIVERGE (emitted but
known to differ on an edge). It is a regression net for the subtle translations
(nil-on-miss `[]`, string quoting, block autosplat, `exist?`→`exists?`, trailing
`while`) and an honesty ledger for the gaps that cannot be closed (no runtime
metaprogramming, no `untyped`).

Two independent codegen bugs surfaced while building the refusal tables and were
fixed:

- **Crystal-keyword locals.** `out`, `in`, `as` and `with` are ordinary Ruby
  identifiers but reserved in Crystal, so `out = ""` emitted Crystal that failed
  with "expecting variable or instance variable after out". `cr_local` renames
  such a local consistently at every binding site (write, read, block parameter,
  multi-assign target, method parameter).
- **Flat multi-param blocks over a one-value iterator.** Ruby auto-splats
  `[[1,2],[3,4]].each { |a, b| }`; Crystal yields one value and rejects
  `do |a, b|` with "too many block parameters". The emitter now binds one
  synthetic param and destructures it in the body, gated on
  `ty_block_yield(...) == 1` so the genuinely pair-yielding iterators
   (`Hash#each`, `each_with_index`) are untouched. The in-body assignment is
   element-shape agnostic, so it also handles a Tuple from `zip`.
- **`attr_accessor` calls refused.** The analyzer resolves `attr_reader`/
  `attr_writer`/`attr_accessor` into the class's `readers[]`/`writers[]` (emitted
  as `def x` / `def x=`), but those names never create a `def` scope, so a call to
  the generated accessor (`c.x`, `c.x = v`) was refused as "not in the Crystal
  subset". `cr_user_method_ok` now also accepts a name found in the receiver
  class's `readers[]` (getter) or `writers[]` (setter, matched on the base name
  with the trailing `=` stripped). This unblocked 29 programs; the ones that still
  build-fail do so on unrelated pre-existing gaps (`Struct.new`, Crystal's strict
  ivar initialization, unary-operator defs), which the old accessor refusal had
  been masking.



### Nilability: consumption-site inference

Ruby's `h[k]` answers `nil` for a missing key and `a[i]` for an out-of-range
index; Crystal's `h[k]` and `a[i]` raise. The fix is not to make *every* read
nilable — that breaks `h[k] + 1`, which is the overwhelmingly common case — but to
make a read nilable **where its result is consumed as possibly-nil**, using
`h[k]?` / `a[i]?`. Spinel's `TyKind` carries no nilability, so the emitter
tracks this structurally:

- **Nil-tolerant positions** set a one-node flag around the read: a `.nil?`,
  `inspect`, `to_s` or `to_a` receiver; the left arm of `||`; the left operand of
  `== nil` / `!= nil`; a bare argument to `p`/`puts`/`print`/`pp`; and a read whose
  value is discarded as a statement.
- **The flag never propagates into a subtree.** In `p(h[k] + 1)` the argument to
  `p` is the `+`, so the operand of `+` must keep the raising form; in `t[k][j]`
  making the inner read nilable would change the outer receiver's type.
- **Range slices keep the raising form.** `a[1..3]` is a slice, not an element
  read, so it is never given a `?`.
- **`Array#at` maps to `[]?`**, because Ruby's `at` answers `nil` out of range
  where Crystal's `at` raises.
- **`min` / `max` map to `min?` / `max?`** in a nil-tolerant position, because
  Ruby answers `nil` on an empty collection where Crystal raises
  `Enumerable::EmptyError`. The same one-node hand-off the index read uses carries
  the flag into the call (the flag is otherwise cleared before any call, so it can
  never reach a receiver).
- **A named capture read always uses `[]?`.** `s[/re/, "name"]` answers `nil`
  whenever the group did not participate, so — unlike the index read — the nilable
  form is unconditionally correct and does not depend on the surrounding context.
- **A local bound from a maybe-missing read is declared `T?` only when every read
  of it tolerates nil.** A pre-pass tallies, per local, how many reads sit in a
  nil-tolerant position versus how many reads it has in all. `v = h[k]` followed
  by `v.nil?` and `v || d` is all-safe, so `v` is `T?`; `x = c.last` followed by
  `x + 1.0` is not, so `x` stays `T` and the arithmetic stays monomorphic. One
  plain read anywhere in the method vetoes the `T?`.
- **A method's or block's trailing expression is its return value, not a
  discarded read**, so it never picks up the discarded-value rule. Converting a
  trailing `h[k]` to `[]?` would widen the return type to a union that every
  caller then has to absorb.

This is deliberately partial: it cannot express the refinement that a guarded
program implies (`v = h[k]; return unless v.nil?; v + 1`). Those cases still fail
to build rather than producing wrong output, which is the same honest-failure
discipline the rest of the backend follows.

**Compile time.** Each `crystal build --release` takes ~10 s, dominated by
compiling the Crystal stdlib (not the generated program). This is the main
practical cost of the target versus the C backend's millisecond-scale `cc` link,
and it would dominate any real-world use.

### `rescue` / `ensure`: structure translates, the exception hierarchy does not

Crystal's `begin/rescue/else/ensure` is structurally the same as Ruby's, so the
control flow ports directly — including the implicit form, where a method with
its own `rescue`/`ensure` clauses is a `BeginNode` wrapping the body. Prism's
`RescueNode` chain (`subsequent`) maps onto consecutive Crystal `rescue`
clauses, `else_clause` and `ensure_clause` onto `else` and `ensure`.

The clause *syntax* is not Ruby's, and getting it wrong produces files that parse
but do not compile:

| Ruby | Crystal |
| --- | --- |
| `rescue A, B => e` | `rescue e : A \| B` |
| `rescue => e` | `rescue e` |
| `rescue` | `rescue` |

The variable comes *before* a colon, and a catch-all clause that binds a name
takes no colon at all (`rescue e :` does not parse). `raise Class, message` also
needed work: Crystal's `raise` takes a single value, so it becomes
`raise(ArgumentError.new("message"))`; a bare `raise` re-raises `$!`, which
Crystal spells `raise e`, so a clause that binds no name gets a synthesized one.

**The exception hierarchy is the real limit.** Crystal 1.21 defines only
`Exception`, `ArgumentError`, `IndexError`, `KeyError`, `RuntimeError`,
`DivisionByZeroError`, `OverflowError`, `TypeCastError`, `NotImplementedError`,
`NilAssertionError` and `InvalidByteSequenceError`. Ruby has around forty, and
the ones the corpus rescues most — `NoMethodError`, `TypeError`, `FrozenError` —
have no counterpart at all. The mapping covers the exact names plus
`ZeroDivisionError` → `DivisionByZeroError` and `StandardError` → `Exception`;
anything else is refused at its own Ruby line, naming the class.

Mapping a missing class onto a broad one instead would not be a compromise, it
would be a bug: `rescue TypeError` becoming `rescue Exception` catches everything,
so a clause that should fall through would take the handler branch. That is a
silent wrong answer, so it is refused — the same discipline the rest of the
backend follows for anything outside the subset.

One ordering is refused for the same reason. A catch-all clause followed by a
specific one is meaningful in Ruby, because a bare `rescue` matches only
`StandardError` and its subclasses, leaving a later `rescue Exception` reachable
for a `ScriptError` or a bare `Exception`. Crystal's bare `rescue` already
matches everything, which makes the later clause dead, and the compiler rejects
the order outright. Reordering would silently change which clause runs, so
`test/bare_rescue_standard_error_only.rb` is refused rather than mistranslated.

The remaining divergence is documented rather than fixed: a bare `rescue` widens
from `StandardError` to `Exception`. This is observable only when a non-
`StandardError` is raised, and it accounts for most of the `Exception` versus
`StandardError` diffs among the newly emitting programs.

## Known gaps and limitations

These are where a full port would have to do real work. Most of them are now
**hard refusals** (the [rejection discipline](#rejection-discipline) names the
construct at its Ruby line instead of shipping wrong output); the exceptions are
the *semantic divergences* below, where a method exists in both languages and the
program compiles but can differ on edge inputs.

- **Semantic divergence of shared methods.** A method that exists in both stdlibs
  but answers differently on edge inputs. These compile and run, so the rejection
  discipline never sees them; closing each needs receiver-type-aware translation
  plus a per-method check. Several are now closed: `String#to_i` (both forms) via
  Crystal's `strict`/`underscore`/`prefix` flags, `ENV[k]` via `ENV[]?`, and
  `Math.lgamma` via the emitted `sp_lgamma_sign`. Still open: `Array#sum` on
  floats (Ruby uses Kahan-Babuška-Neumaier compensation), `Float#to_s` fixed vs
  scientific notation, and the like.
- **`puts <collection>`.** Ruby's `puts` splats an array/hash (one element per
  line); Crystal's prints the container via `to_s`. Not translated — the corpus
  uses `.join` instead. A general fix needs receiver-type-aware emission.
- **Integer width.** `TY_INT` maps to `Int64`; Ruby integers are arbitrary
  precision. Values beyond 2⁶³ overflow (Spinel's C backend auto-promotes to
  Bigint; the Crystal path does not). A **literal** past 64 bits is already
  refused at its source line, and the same is done for a float literal whose
  `to_i` is out of range, but an arithmetic result that overflows only at run time
  cannot be caught without range analysis — the analyzer's own widen-to-bigint
  detection only fires under `--int-overflow=promote`, which the Crystal target
  does not implement. The `promote_*` tests therefore stay out of the Crystal
  corpus by the same rule that keeps them out of the C backend's raise-mode runs.
  `test/range_unbounded_bounds.rb` (infinite/nil Range bounds, which CRuby sizes as
  Infinity) remains a documented DIFF for the same reason.
- **Poly containers.** When the inference leaves a container poly — notably a
  nested array literal whose *outer* array is not narrowed (`[[1,2],[3,4]]` →
  `poly_array`, so the inner block param is `poly`) — the element type is
  unresolvable and Crystal has no `[] of untyped`. The look-ahead handles
   concrete pushes; genuinely-poly elements still fail to build. This is the
   sharpest *inference* gap surfaced by the PoC.
  - **`Struct` feature surface.** The core definition (positional constructor,
    readers, writers) plus `to_a`/`values`, `.members`, `to_h`, member-wise `==`,
    and `[]`/`[]=` by symbol/string/index all translate; `[]=` emits one cast-free
    overload set per distinct member type, so a heterogeneous struct writes without
    a union. Still not translated: `dig`, `values_at`, and a `Struct#new`
    called with fewer arguments than members (Ruby nil-fills the rest — nilable
    ivars would break members read in arithmetic, a measured net loss). An unknown
    member *name* answers `IndexError` where Ruby answers `NameError`, since Crystal
    1.21 has no `NameError`; that difference is pinned in the difference-test ledger.
    Programs using the missing methods build-fail on them. `Data.define` is refused
    outright (value-type `==`), not partially translated.
 - **Non-local block control flow.** `break`/`return` out of a *yielding* method's
  block (and `catch`/`throw`, `retry`) are not handled. Crystal `Proc`s cannot
  break out of the enclosing method, so these would need emulation (exceptions /
  CPS). This is the sharpest *semantic* gap for a full port and was deliberately
  left out of the corpus.
- **Out of subset entirely:** class inheritance (`< Parent`) and `super`, FFI,
  Thread/Fiber (the concurrency model is not replicated — `Thread#value`,
  `Fiber#transfer`, `Thread#join(timeout)` and the like are refused, Phase 13),
  `for` loops, `retry` inside a rescue clause, pattern matching (`case`/`in` —
  plain `case`/`when` does translate), most of the Ruby exception classes (see
  the `rescue` section above), and dynamic metaprogramming (which Spinel itself
  does not support). Regexp literals, rendering and the common matching methods
  *do* translate (Phases 2 and 8); only the pattern dialect differences (`\h`,
  which is rewritten, and named backreferences, which are refused) remain.

## Effort to go beyond the PoC

The emitter is ~720 LOC against an 88k-LOC C emitter, but the C emitter encodes
decades of edge cases. A production Crystal backend would need: receiver-type-
aware method translation (not a flat table), poly-container representation,
non-local control-flow emulation, integer-width promotion, and the exact-output
surface (`inspect`, `sprintf`, Regexp dialect, Marshal) that the C runtime
reproduces byte-for-byte. The PoC shows the *architecture* is sound — the
analysis contract is backend-agnostic — but the long tail of Ruby semantics is
where the cost lives, and it is largely independent of the choice of target
language (an LLVM-IR backend would face the same semantic gaps without a second
type system in between).
