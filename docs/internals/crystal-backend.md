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

`--target` currently accepts only `crystal`. The default (no flag) is unchanged
and still emits C.

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

**Compile time.** Each `crystal build --release` takes ~10 s, dominated by
compiling the Crystal stdlib (not the generated program). This is the main
practical cost of the target versus the C backend's millisecond-scale `cc` link,
and it would dominate any real-world use.

## Known gaps and limitations

These are where a full port would have to do real work:

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
- **Out of subset entirely:** `case`/`when`, exceptions/`rescue`, class
  inheritance (`< Parent`) and `super`, FFI, Regexp, Thread/Fiber, `for` loops,
  and dynamic metaprogramming (which Spinel itself does not support).

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
