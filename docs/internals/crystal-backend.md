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
scripts/poc_crystal.sh                # run the corpus vs CRuby and print metrics
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

### No `untyped` in Crystal 1.21

Crystal 1.21 **removed the `untyped` type**. `untyped` is no longer a keyword
(`def f(x : untyped)`, `[] of untyped`, and `Array(untyped)` are all parse
errors), and the `Any` alias that replaced it is gone too, as is `Object` as a
generic argument ("can't use Object as generic type argument yet"). The stdlib
source contains no use of `untyped` as a type at all.

This has two consequences for the emitter:

1. There is **no dynamic dispatch** to defer to. A call on a poly receiver
   cannot be verified by us *or* resolved by Crystal, so poly receivers are
   refused rather than passed through.
2. A poly container has **no spelling**. `Array(untyped)` cannot be written, so
   poly arrays and hashes are refused instead of emitting a type that only ever
   produced a parse error.

Both were worth doing even though they *lower* the emit-clean count: every one of
the 91 programs that emitted `untyped` was a guaranteed build failure, so
converting them to line-anchored refusals costs no working program and removes a
whole class of silently-broken output. For the same reason an empty array or
hash literal is emitted as `Array(T).new` / `Hash(K, V).new` rather than the
`[] of T` / `{} of K => V` forms, which are annotation-only and parse as errors
the moment the literal is used as a method receiver.

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
| `TY_POLY*` / `TY_UNKNOWN` | `untyped` (counted as a metric) |

Locals and method parameters are annotated with their inferred type when it is
concrete, which is what keeps `Int64` arithmetic and container element types in
agreement. Value-type promotion, GC rooting, and the tagged-union machinery have
no Crystal analogue — Crystal's own GC and static unions absorb them.

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

**Full-corpus baseline.** Run against the whole 4413-program `test/` suite (the
 C backend's own corpus, so every program is one Spinel can analyze), the Crystal
target emits cleanly on **809 (18.4%)** and refuses the rest with a line-anchored
diagnostic. The C backend compiles all 4413, so the gap is entirely the Crystal
subset.

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

**Measured results.** Of the 809 programs that emit, building each and comparing
its output against CRuby gives:

| outcome | count | meaning |
| --- | --- | --- |
| PASS | 222 | builds and output is byte-identical to CRuby |
| DIFF | 75 | builds, but disagrees with CRuby |
| BUILDFAIL | 508 | the emitted Crystal does not compile |

Against the pre-`rescue` baseline of 682 emitting / 189 PASS / 61 DIFF / 432
BUILDFAIL, this is **+127 emit-clean programs and +33 PASS with no regressions**:
every program that passed before still passes.

Reproduce with `scripts/crystal_corpus.sh`; it gives every build its own
`CRYSTAL_CACHE_DIR` (a shared cache across parallel builds corrupts itself and
manufactures ~90 bogus "compiler bug" reports) and compares outputs serially
after the builds finish.

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

- **Semantic divergence of shared methods.** Methods present in both stdlibs but
  with different edge-case behaviour: `String#to_i` (Ruby accepts `_` separators,
  Crystal raises), `ENV[]` (Ruby returns nil for a missing key, Crystal raises),
  and the like. These compile and run, so they are not caught by the rejection
  discipline; closing them needs receiver-type-aware translation plus per-method
  semantic checks.
- **`puts <collection>`.** Ruby's `puts` splats an array/hash (one element per
  line); Crystal's prints the container via `to_s`. Not translated — the corpus
  uses `.join` instead. A general fix needs receiver-type-aware emission.
- **Integer width.** `TY_INT` maps to `Int64`; Ruby integers are arbitrary
  precision. Values beyond 2⁶³ overflow (Spinel's C backend auto-promotes to
  Bigint; the Crystal path does not).
- **Poly containers.** When the inference leaves a container poly — notably a
  nested array literal whose *outer* array is not narrowed (`[[1,2],[3,4]]` →
  `poly_array`, so the inner block param is `poly`) — the element type is
  unresolvable and Crystal has no `[] of untyped`. The look-ahead handles
  concrete pushes; genuinely-poly elements still fail to build. This is the
  sharpest *inference* gap surfaced by the PoC.
- **Non-local block control flow.** `break`/`return` out of a *yielding* method's
  block (and `catch`/`throw`, `retry`) are not handled. Crystal `Proc`s cannot
  break out of the enclosing method, so these would need emulation (exceptions /
  CPS). This is the sharpest *semantic* gap for a full port and was deliberately
  left out of the corpus.
- **Out of subset entirely:** class inheritance (`< Parent`) and `super`, FFI,
  Regexp, Thread/Fiber, `for` loops, `retry` inside a rescue clause, pattern
  matching (`case`/`in` — plain `case`/`when` does translate), most of the Ruby
  exception classes (see the `rescue` section above), and dynamic
  metaprogramming (which Spinel itself does not support).

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
