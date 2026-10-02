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

| outcome | baseline | P2 | P3 | P4 | P5 | P6 | P7 | P8 | P9 | P10 | P11 | P12 | P13 | P14 | P15 | **P16 (current)** | meaning |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| corpus | 4417 | 4417 | 4418 | 4398 † | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | 4398 | **4398** | programs run |
| emit-clean | 757 | 786 | 787 | 778 | 778 | 778 | 778 | 777 | 777 | 777 | 777 | 777 | 734 | 673 | 663 | **664** | emits Crystal at all (the rest refuse) |
| PASS | 196 | 256 | 257 | 264 | 268 | 278 | 289 | 294 | 297 | 300 | 303 | 305 | 307 | 307 | 308 | **315** | builds and output is byte-identical to CRuby |
| DIFF | 70 | 102 | 102 | 89 | 88 | 78 | 67 | 61 | 58 | 55 | 52 | 50 | 49 | 49 | 50 | **50** | builds, but disagrees with CRuby |
| BUILDFAIL | 491 | 428 | 428 | 425 | 422 | 422 | 422 | 422 | 422 | 422 | 422 | 422 | 378 | 317 | 305 | **299** | the emitted Crystal does not compile |
| REFUSED | 3659 | 3631 | 3631 | 3620 | 3620 | 3620 | 3620 | 3621 | 3621 | 3621 | 3621 | 3621 | 3664 | 3725 | 3735 | **3734** | refused at the Ruby line, with a reason |

(Phase commits, for the column headers: P2 `a8455324`, P3 `2b722b6d`,
P4 `42923c6d`, P5 `d115b402`, P6 `a5eb7684`, P7 `f8ba43ac`, P8 `bc5dd918`,
P9 `a3fc1ffd`, P10 `d06db81e`, P11 `e35d1fb3`, P12 `9f4e8772`,
P13 `7725fc9d`, P14 `3adf0168`, P15 `c2f2eb1d`.)

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
**+1 PASS, +1 DIFF, −12 BUILDFAIL, +10 REFUSED**, and Phase 16
**+7 PASS, +0 DIFF, −6 BUILDFAIL, −1 REFUSED** — all with zero PASS regressions
and no change to any program other than the ones named below.

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
