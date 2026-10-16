#!/usr/bin/env bash
# The Crystal backend's difference ledger. For each named Ruby-vs-Crystal
# difference (see the "Crystal for Rubyists" reference), pin its honest
# disposition:
#
#   AGREE   -- we translate it and the output is byte-identical to CRuby.
#   REFUSE  -- we refuse cleanly, naming the construct (the honesty gate).
#   DIVERGE -- we emit it but it differs from CRuby on a known edge; documented,
#              not hidden. If one ever starts agreeing we are told to promote it.
#
# This is the executable form of "what we handle / refuse / diverge." It is a
# regression net for the subtle translations (nil-on-miss [], attr_*, autosplat,
# string quoting) and an honesty ledger for the gaps that cannot be closed
# (no runtime metaprogramming, no untyped). It complements crystal_reject_test.sh,
# which pins refusal messages in isolation.
#
# Usage: crystal_difference_test.sh [path-to-spinel]
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
SPINEL="${1:-bin/spinel}"
CRYSTAL="${CRYSTAL:-crystal}"
tmp=$(mktemp -d /tmp/spinel-crystal-diff.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
ok=1
n_agree=0; n_refuse=0; n_diverge=0

have_crystal=1
command -v "$CRYSTAL" >/dev/null 2>&1 || { have_crystal=0; echo "note: '$CRYSTAL' not found; AGREE/DIVERGE build checks skipped"; }

# emit_cr <label> <ruby source> : write $tmp/<label>.{rb,cr}; return 0 iff emitted.
emit_cr() {
  printf '%s\n' "$2" > "$tmp/$1.rb"
  if ! "$SPINEL" --target crystal "$tmp/$1.rb" -S > "$tmp/$1.cr" 2> "$tmp/$1.emit"; then
    return 1
  fi
  return 0
}

# expect_agree <label> <ruby source> : transpile, build, run both, byte-compare.
expect_agree() {
  local label="$1" src="$2"
  if ! emit_cr "$label" "$src"; then
    echo "FAIL [agree $label]: refused, but should translate: $(head -1 "$tmp/$label.emit")"
    ok=0; return
  fi
  if [ "$have_crystal" = 0 ]; then n_agree=$((n_agree+1)); return; fi
  ruby "$tmp/$label.rb" > "$tmp/$label.rbout" 2>&1
  if ! CRYSTAL_CACHE_DIR="$tmp/cache-$label" timeout 300 "$CRYSTAL" build --release -o "$tmp/$label.bin" "$tmp/$label.cr" 2> "$tmp/$label.build"; then
    echo "FAIL [agree $label]: emitted Crystal does not compile:"; head -3 "$tmp/$label.build"; ok=0; return
  fi
  "$tmp/$label.bin" > "$tmp/$label.crout" 2>&1
  if ! diff -q "$tmp/$label.rbout" "$tmp/$label.crout" >/dev/null; then
    echo "FAIL [agree $label]: output differs from CRuby:"; diff "$tmp/$label.rbout" "$tmp/$label.crout" | head -6; ok=0; return
  fi
  n_agree=$((n_agree+1))
}

# expect_agree_file <label> <ruby-file> : transpile a whole corpus test file,
# build and run it, and compare the Crystal output against the file's committed
# .expected (which is also the CRuby output, so CRuby itself is diffed in too).
# Used for the broad Struct surface, which is too large for an inline source.
expect_agree_file() {
  local label="$1" rb="$2"
  if ! "$SPINEL" --target crystal "$rb" -S > "$tmp/$label.cr" 2> "$tmp/$label.emit"; then
    echo "FAIL [agree $label]: refused, but should translate: $(head -1 "$tmp/$label.emit")"
    ok=0; return
  fi
  if [ "$have_crystal" = 0 ]; then n_agree=$((n_agree+1)); return; fi
  ruby "$rb" > "$tmp/$label.rbout" 2>&1
  if ! diff -q "$rb.expected" "$tmp/$label.rbout" >/dev/null; then
    echo "FAIL [agree $label]: CRuby output no longer matches $rb.expected"; ok=0; return
  fi
  if ! CRYSTAL_CACHE_DIR="$tmp/cache-$label" timeout 300 "$CRYSTAL" build --release -o "$tmp/$label.bin" "$tmp/$label.cr" 2> "$tmp/$label.build"; then
    echo "FAIL [agree $label]: emitted Crystal does not compile:"; head -3 "$tmp/$label.build"; ok=0; return
  fi
  "$tmp/$label.bin" > "$tmp/$label.crout" 2>&1
  if ! diff -q "$tmp/$label.rbout" "$tmp/$label.crout" >/dev/null; then
    echo "FAIL [agree $label]: output differs from CRuby:"; diff "$tmp/$label.rbout" "$tmp/$label.crout" | head -6; ok=0; return
  fi
  n_agree=$((n_agree+1))
}

# expect_refuse <substring-of-diagnostic> <ruby source> : must exit non-zero and name the reason.
expect_refuse() {
  local want="$1" src="$2" out
  printf '%s\n' "$src" > "$tmp/refuse.rb"
  if out=$("$SPINEL" --target crystal "$tmp/refuse.rb" -S 2>&1); then
    echo "FAIL [refuse]: compiled, but must be refused: $src"; ok=0; return
  fi
  case "$out" in
    *"$want"*) n_refuse=$((n_refuse+1)) ;;
    *) echo "FAIL [refuse]: refused without saying why (wanted: $want)"; echo "$out" | head -3; ok=0 ;;
  esac
}

# expect_diverge <label> <note> <ruby source> : must EMIT (not be refused); we
# document that it differs from CRuby on a known edge. Builds+runs both and
# confirms they differ, so if it ever starts agreeing we are prompted to promote
# it to expect_agree.
expect_diverge() {
  local label="$1" note="$2" src="$3"
  if ! emit_cr "$label" "$src"; then
    echo "FAIL [diverge $label]: refused, but should emit ($note)"; ok=0; return
  fi
  n_diverge=$((n_diverge+1))
  if [ "$have_crystal" = 0 ]; then return; fi
  ruby "$tmp/$label.rb" > "$tmp/$label.rbout" 2>&1
  if CRYSTAL_CACHE_DIR="$tmp/cache-$label" timeout 300 "$CRYSTAL" build --release -o "$tmp/$label.bin" "$tmp/$label.cr" 2> "$tmp/$label.build"; then
    "$tmp/$label.bin" > "$tmp/$label.crout" 2>&1
    if diff -q "$tmp/$label.rbout" "$tmp/$label.crout" >/dev/null; then
      echo "NOTE [diverge $label]: now AGREES with CRuby -- promote to expect_agree? ($note)"
    fi
  else
    echo "NOTE [diverge $label]: emitted but does not compile (a BUILDFAIL, not a clean divergence): $note"
  fi
}

# ---- AGREE: the backend translates these; output matches CRuby --------------

# The boxed poly-value runtime (SpValue, sp_box, the scalar operators) is
# emitted into every program so a widened slot has a real Crystal type. Nothing
# in this test drives it yet, but the emitted file must still compile and run,
# and the runtime must be present -- otherwise a later phase that boxes into it
# would be building against a name that is not there.
expect_agree "runtime_skeleton" 'p 1 + 1'
if have_crystal=1; then
  if ! grep -q 'class SpValue' "$tmp/runtime_skeleton.cr"; then
    echo "FAIL [runtime_skeleton]: SpValue runtime not emitted"; ok=0
  fi
  if ! grep -q 'def sp_box' "$tmp/runtime_skeleton.cr"; then
    echo "FAIL [runtime_skeleton]: sp_box helper not emitted"; ok=0
  fi
fi

# [] into a Hash/Array answers nil on a miss in Ruby; Crystal raises. We emit
# []? where the value is consumed as possibly-nil (cr_maybe_missing).
expect_agree "index_nil_on_miss" 'h = {a: 1}; p h[:b]'

# Single-quoted Ruby strings are double-quoted Crystal strings (single quotes
# are char literals in Crystal); backslashes are re-escaped so \n stays literal.
expect_agree "string_quoting" "p 'abc'"

# attr_accessor/reader/writer become property/getter/setter; we emit explicit
# def/def x= methods, and a call to the generated accessor (c.x / c.x = v) must
# resolve against the class's readers[]/writers[], not just its def scopes.
expect_agree "attr_accessor" 'class C
  attr_accessor :x
  def initialize
    @x = 5
  end
end
c = C.new
p c.x
c.x = 9
p c.x'

# A top-level `X = Struct.new(:a, :b)` becomes a Crystal reference-type `class`
# with a positional `initialize`, readers and writers; member get/set round-trip.
expect_agree "struct_new" 'Point = Struct.new(:x, :y)
p Point.new(1, 2).x + Point.new(3, 4).y
a = Point.new(1, 2)
a.x = 10
p a.x'

# []= writes a member by symbol, string name or integer position (negative counts
# from the end) and answers the stored value. One overload set per distinct member
# type, so a heterogeneous struct writes without a union or a cast.
expect_agree "struct_aref_write" 'S = Struct.new(:a, :b)
s = S.new(1, 2)
s[:a] = 10
s["b"] = 20
s[-1] = 30
p (s[0] = 40)
p s.to_a
p (begin; s[9] = 1; "no"; rescue => e; e.class; end)
R = Struct.new(:name, :count)
r = R.new("a", 1)
r[0] = "b"
r[-1] = 2
p [r.name, r.count]'

# The broad Struct surface (top-level and nested classes, homogeneous and
# heterogeneous members, readers/writers, to_a/to_h/==/[]/[]=, block and
# implicit-self reads) as one file, pinned against the committed .expected.
expect_agree_file "struct_full" "test/crystal_struct.rb"

# No array autosplat and a max block arity of 1 in Crystal; we destructure the
# yielded pair in the block body instead.
expect_agree "block_autosplat" '[[1,2],[3,4]].each { |a,b| p a+b }'

# P21: an empty `[]` widened by disagreeing SCALAR pushes becomes Array(SpValue);
# each push is boxed on the way in (sp_box) and the array inspects like CRuby's.
expect_agree "poly_scalar_array" 'a = []
a << 1
a << "x"
p a'

# P21.1: a multi-argument push is classified from EVERY argument, so a
# heterogeneous `push(1, "x")` widens instead of pinning Array(Int64) and then
# failing to build on the String.
expect_agree "poly_scalar_array_multipush" 'a = []
a.push(1, "x")
p a'

# P22h: a COMPARISON between method parameters the call sites widened to a mixed
# union (here `a <= b`, used with Int, Float, String, Bool and nil) is boxed to
# SpValue on both sides, so Crystal's Int64#<=(String) is never asked to resolve.
# SpValue's comparator answers true for a comparable pair and raises for a
# Ruby-illegal one (`1 <= "x"`), which the caller can rescue, matching CRuby.
expect_agree "poly_param_compare" 'def less(a, b)
  a <= b
end
p less(1, 2.0)
begin
  p less(1, "x")
rescue
  p false
end'

# P22i: a compound assignment to a CONCRETE SCALAR instance variable needs no
# boxing at all -- Crystal carries `@x += v` natively. This node used to be refused
# outright unless the whole-slot poly switch was on, which refused a plain
# `@count += n` on an Int64 ivar for no reason.
expect_agree "ivar_op_write_scalar" 'class Counter
  def initialize
    @count = 1
  end

  def bump(n)
    @count += n
  end
end

c = Counter.new
p c.bump(4)
p c.bump(2)'

# P22j: `x&.m` is `x.try { |t| t.m }`. The emitter ignored the node's
# `call_operator`, so every safe navigation became a plain `.` -- correct only for a
# receiver that cannot be nil. `try` is exact for both a nilable and a non-nilable
# receiver, which matters because Spinel's TyKind carries no nilability. (`&.` itself
# is unusable here: inside a parenthesised argument it is "unterminated call".)
expect_agree "safe_nav_nilable_ivar" 'class Holder
  def initialize(set)
    @item = "v" if set
  end

  def peek
    @item&.upcase
  end
end

p Holder.new(true).peek
p Holder.new(false).peek'

# P22j: an ivar assigned only inside a conditional is nilable, exactly as Ruby reads
# it. The declaration was `T` because the class HAS an initialize, and Crystal
# rejected the class with "instance variable '@combat' of Player must be Combat, not
# Nil".
expect_agree "conditional_ivar_is_nilable" 'class Slot
  def initialize(n)
    @v = n if n > 0
  end

  def get
    @v
  end
end

p Slot.new(3).get
p Slot.new(-1).get'

# P22k: a call to a user-defined method on a receiver whose class is not statically
# known is refused by name. Crystal's Array/Set spell `include?` as `includes?`, but a
# user class defines it under the Ruby name -- and on a union the two arms spell the
# method DIFFERENTLY, so neither name resolves `(Tags | Array(String))`.
expect_refuse "a call to a user-defined method on a receiver whose class is not statically known" 'class Tags
  def initialize(names); @names = names; end
  def include?(n); @names.include?(n); end
end
def found(v) = v.include?("ruby")
p found(Tags.new(["ruby"]))
p found(["ruby"])'

# Consistent dot notation / renamed predicates: File.exist? is File.exists?.
expect_agree "exist_to_exists" 'p File.exist?("/dev/null")'

# Trailing while/until are not Crystal syntax; we rewrite the modifier form to a
# leading loop.
expect_agree "trailing_while" 'n = 3; n -= 1 while n > 0; p n'

# P23: `$!` is the exception the innermost rescue clause is handling. The slot
# is a runtime class variable the clause sets on entry and restores on exit, so
# `$!` is that exception inside the clause and nil outside it; `.class` renders
# through the same name shim as any other receiver.
expect_agree "p23_last_error" 'begin
  raise ArgumentError, "boom"
rescue => e
  p $!.class
  p $!.message
end
p $!.nil?'

# P24: a class the program defines is emitted with the closest Crystal exception
# base, so it inherits Crystal's Exception#initialize and can be raised with a
# message; a namespaced user class resolves in rescue/raise (qualified emitted
# name). `raise "message"` becomes raise(RuntimeError.new(...)) because Crystal's
# raise(String) raises a bare Exception, not Ruby's RuntimeError.
expect_agree "p24_user_exception_message" 'class MyErr < StandardError; end
begin
  raise MyErr, "kaput"
rescue MyErr => e
  p e.message
end'

expect_agree "p24_namespaced_exception_rescue" 'module App
  class Error < StandardError; end
end
begin
  raise App::Error, "boom"
rescue App::Error => e
  p e.message
  p e.class
end'

expect_agree "p24_raise_string_is_runtime_error" 'begin
  raise "boom"
rescue RuntimeError => e
  p e.message
  p e.class
end'

# P25.0: valued control flow. Prism hides a return/next/break operand inside an
# ArgumentsNode the walker did not model, so EVERY valued return/next/break was
# refused as the bare node kind "ArgumentsNode" -- a bug, not a design limit,
# since Crystal's return/next/break take the same value. The same context gap is
# closed for `case`, `unless`, `while` and `until` in value position (all are
# expressions in Crystal, and while/until answer their break value like Ruby).
expect_agree "p25_return_value" 'def f(n)
  return n if n < 2
  f(n - 1) + f(n - 2)
end
p [0, 1, 5, 10].map { |i| f(i) }'

expect_agree "p25_next_break_value" 'p [1, 2, 3].map { |x| next x * 10 if x == 2; x }
p [1, 2, 3].each { |x| break x if x == 2 }'

expect_agree "p25_case_value" 'a = 2
p(case a
  when 1 then :one
  when 2, 3 then :few
  else :many
  end)'

expect_agree "p25_unless_else_value" 'x = 2
p(unless x > 1
  "small"
else
  "big"
end)'

expect_agree "p25_while_break_value" 'n = 0
r = while true
  n += 1
  break "stopped at #{n}" if n == 3
end
p r'

# A multi-value return/next/break answers an Array in Ruby but a Tuple in Crystal,
# so it is refused by name rather than emitted as the wrong shape.
expect_refuse "a return/next/break with multiple values" 'def f; return 1, 2; end
p f'

# Ruby's `loop` catches StopIteration and answers its result, so `loop { e.next }`
# ends when the Enumerator runs out. Crystal's Iterator#next answers a Stop value
# instead of raising, so the same source never terminates -- refused rather than
# emitted as a hang. (`e.next` outside a loop is fine: nothing observes the raise.)
expect_refuse "Enumerator#next" 'e = [1, 2].each
p loop { e.next }'

# ---- DIVERGE: emitted, but differs from CRuby on a known edge ----------------
# Each of these was REFUSED before P25.0 (a valued return/next/break or a
# value-position case/unless/while was the first obstacle) and now emits. Three
# of them land on a pre-existing gap the backend has never modelled, so they are
# pinned here as known divergences rather than left as silent wrong answers. If
# one ever starts agreeing, the ledger says to promote it to expect_agree.

# Ruby's Hash#each yields the pair as a two-element Array when the block takes a
# single parameter (`h.map { |pair| pair }` -> ["x", 1]); Crystal's Hash#each
# yields a Tuple ({"x", 1}). The pair is only observable when the block body
# looks at it, which the emitter cannot prove, so the shape is left diverging.
expect_diverge "p25_diverge_hash_pair_is_array" \
  "Hash#each yields a Tuple in Crystal, an Array in Ruby, for a single block param" \
  'h = { "x" => 1, "y" => 2 }
p h.map { |pair| pair.inspect }'

# Ruby has TrueClass and FalseClass; Crystal has one Bool. `TrueClass === false`
# is false in Ruby and true in Crystal, so a `case`/`===` on a Bool class literal
# cannot be translated. (The other lines of the same program agree.)
expect_diverge "p25_diverge_bool_class_identity" \
  "Crystal has one Bool where Ruby has TrueClass and FalseClass" \
  'b = true
f = false
p TrueClass === b
p case b when TrueClass then :yes else :no end
p FalseClass === f
p TrueClass === f
p Integer === 5
p Integer === b'

# Ruby's String#[] with an Integer index answers a one-character String; Crystal's
# answers a Char. A boxed index that is sometimes a Range and sometimes an
# Integer reaches the read as an Int, so the Char form is what gets emitted.
expect_diverge "p25_diverge_string_index_char" \
  "String#[] answers a Char in Crystal, a one-character String in Ruby" \
  'def pick(i) = i.odd? ? (0..1) : 0
p "abcdef"[pick(1)]
p "abcdef"[pick(2)]'

# ---- REFUSE: no Crystal counterpart, so refuse cleanly and name it ----------

expect_refuse "a \`for ... in\` loop"              'for x in [1,2]; p x; end'
expect_refuse "a lambda/proc"                    'f = -> { 42 }; p f.call'
expect_refuse "method 'proc' is not in the Crystal subset" 'f = proc { 42 }; p f.call'
expect_refuse "method 'eval' is not in the Crystal subset" 'p eval("1+1")'
expect_refuse "respond_to?' is not available on this receiver" 'p "a".respond_to?(:upcase)'
# Regex backreference globals $`/$'/$& are not supported; use $~.pre_match etc.
expect_refuse "a named regexp back-reference"     '"a" =~ /a/; p $&'
# Data.define is a VALUE type (member-wise ==); a Crystal class compares by
# identity, so we refuse it rather than emit a reference class that would answer
# `==` wrong. (A plain Struct.new is a reference type and IS translated.)
expect_refuse "the constant 'Data'"              'D = Data.define(:a); p D.new(1) == D.new(1)'
# P22: a heterogeneous array whose element is a user object or a stdlib container
# is widened to Array(SpValue) and the element boxed through SpBox; the wrapped
# object's inspect forwards, so the rendering matches CRuby's.
expect_agree "poly_object_array" 'class Foo
  def initialize(n); @n = n; end
  def inspect; "Foo(#{@n})"; end
end
a = []
a << 1
a << Foo.new(2)
p a'

expect_agree "poly_container_array" 'a = []
a << 1
a << [5]
p a'
# P22: an element with no `sp_box` overload -- a Bignum has no Crystal spelling
# here -- keeps the honest refusal rather than emitting a push that cannot
# resolve. The message names the element's kind.
expect_refuse "a heterogeneous array whose element has no boxed representation" 'a = []
a << 1
a << 2**100
p a'
# P22d: `arr.<stage>.with_index` chains. Crystal has no blockless Array#map (a
# build error: "expected to be invoked with a block") and no index-taking
# select/reject/sort_by, so map/each/select/reject are rewritten to Crystal's
# indexed forms. `each.with_index` answers the receiver (Ruby's each return), and
# the blockless pair Enumerator materializes [element, index] Arrays at `.to_a`.
expect_agree "with_index_map" 'p [4, 5].map.with_index(1) { |x, i| x * i }'
expect_agree "with_index_select" 'p((1..5).select.with_index { |v, i| i.even? })'
expect_agree "with_index_each_returns_receiver" 'r = [7, 8].each.with_index { |x, i| x + i }
p r'
expect_agree "with_index_to_a" 'p [10, 20].each.with_index(1).to_a'
expect_agree "sort_by_with_index" 'p ["b", "aa", "a"].sort_by.with_index { |w, i| [w.length, i] }'
# P22e: a widened (poly) instance variable is boxed as SpValue. A read that Ruby
# answers nil before any write does too, and puts/interpolation render the run
# time kind like CRuby.
expect_agree "poly_ivar_stores" 'class C
  def set_int; @x = 42; end
  def set_str; @x = "hi"; end
  def show; puts @x; end
end
c = C.new
c.set_int; c.show
c.set_str; c.show'
expect_agree "poly_ivar_unset_reads_nil" 'class C
  def x; @x; end
  def y_nilp; @y.nil?; end
end
p C.new.x
p C.new.y_nilp'
# P22f: `include M` of a user module is emitted (Crystal mixes its methods in);
# `module_function`/`private` and other declaration-only markers are skipped,
# since the analyzer already captured their effect.
expect_agree "include_user_module" 'module Greet
  def hi; "hi"; end
end
class C
  include Greet
end
p C.new.hi'
expect_agree "module_function" 'module M
  module_function
  def twice(n); n * 2; end
end
p M.twice(3)'
# P22f: a user class named like a Crystal stdlib generic cannot be reopened there.
expect_refuse "a class named 'Box' (a Crystal stdlib type" 'class Box
  def initialize(n); @n = n; end
end
p Box.new(1)'
# P22g: a method that `yield`s is translated -- the signature declares `(&)` and
# `yield` stays `yield`. A method whose `yield` calls disagree on arity is
# refused (Ruby nil-fills the block params, Crystal infers one arity).
expect_agree "yield_block" 'def twice
  yield 1
  yield 2
end
twice { |x| puts x }'
expect_agree "yield_multi_arg" 'def pair
  yield 10, 20
end
pair { |a, b| p a + b }'
expect_refuse "calls take different numbers of arguments" 'def m
  yield 1, 2
  yield 3
end
m { |a, b| p [a, b] }'
# P21.1: Ruby extends a short array on `a[i] = v`; Crystal's []= raises, so an
# index write whose value disagrees with the pinned element type is refused
# (it would otherwise emit Array(Int64) that the String cannot be stored into).
expect_refuse "an index write whose value type disagrees" 'a = []
a << 1
a[0] = "x"
p a'

# P22o: a subclass inherits the emitted superclass, so a call that resolves to a
# superclass def (or an inherited attr reader) translates. P22p: `super` (bare
# and with args) translates when the superclass really defines the method and
# the arity matches; a `super` to a missing parent method or with a mismatched
# argument count is refused (Ruby raises NoMethodError/ArgumentError, but the
# emitted Crystal would be a build error, so the refusal names the line).
expect_agree "super_bare_noargs" 'class Base
  def initialize(x)
    @x = x
  end
  def show
    "b:#{@x}"
  end
end
class Derived < Base
  def initialize(x)
    super
    @y = x * 2
  end
end
puts Derived.new(3).show'
expect_agree "super_with_args" 'class Base
  def initialize(x)
    @x = x
  end
end
class Derived < Base
  def initialize(x, y)
    super(y)
    @z = x + y
  end
  def show
    "d:#{@x}/#{@z}"
  end
end
puts Derived.new(1, 2).show'
expect_agree "inherited_reader" 'class Base
  attr_reader :x
  def initialize(v)
    @x = v
  end
end
class Child < Base
end
puts Child.new(7).x'
expect_refuse "no superclass of 'Derived' defines 'm'" 'class Base
end
class Derived < Base
  def m
    super
  end
end
puts Derived.new.m'
expect_refuse "argument count does not match the superclass method" 'class Base
  def m(a)
    a
  end
end
class Derived < Base
  def m
    super(1, 2)
  end
end
puts Derived.new.m'

# P23: only `$!` is in the global subset. Another global read is refused by name
# (Ruby would answer nil), and `$!` is read-only in Ruby, so even a write to it
# is refused rather than emitted with different semantics.
expect_refuse "the global variable '\$x' (\$! is the only global in the subset)" 'p $x'
expect_refuse "an assignment to \$! (it is read-only in Ruby)" '$! = nil'

# ---- DIVERGE: emitted, but differs from CRuby on a known edge ---------------

# P23: `$!` holds the exception object, but Crystal's Exception#inspect spells
# `#<ArgumentError:x>` where CRuby spells `#<ArgumentError: x>` (no space).
expect_diverge "p23_dollar_bang_inspect" "Crystal's Exception#inspect omits the space CRuby uses" 'begin
  raise ArgumentError, "x"
rescue
  p $!
end'

# P24: a user exception class now inherits Crystal's Exception, whose #message is
# nil when unset; Ruby's defaults to the class name. The inspect space above and
# the non-String to_s family (test_exception_nonstring_message,
# test_exception_own_to_s) are the same residual message-text divergence.
expect_diverge "p24_default_message_is_class_name" "Ruby's Exception#message defaults to the class name; Crystal's is nil" 'class MyErr < StandardError; end
p MyErr.new.message'

# A string INDEX read is a Char in Crystal and a 1-character String in Ruby
# (`"abc"[0]` is "a" in Ruby, 'a' here). General, but it is what makes a
# `File.binread` result diverge on byte access: Ruby reads ASCII-8BIT bytes,
# Crystal's `File.read` reads a UTF-8 String. The encoding itself is not
# observable (`.encoding` / `.valid_encoding?` are refused), so `bytes` agrees.
expect_diverge "string_index_read" "Crystal String#[] yields a Char; Ruby a 1-char String" 'p "abc"[0]'

# .each returns nil in Crystal (not the receiver), so it cannot be chained.
expect_diverge "each_returns_nil" ".each yields nil, not the receiver" 'p [1,2].each { }'

# Crystal prints parameterized type names; a Hash literal is a real Hash here
# (we emit => form), but .class shows its element types.
expect_diverge "param_type_name" "Crystal .class shows parameterized types" 'p({a: 1}.class)'

# Struct#to_h is translated to a real Hash, and the emitted Hash#inspect shim
# renders symbol keys with CRuby's spelling ({a: 1}, not {:a => 1}).
expect_agree "struct_to_h_print" 'P = Struct.new(:a); p P.new(1).to_h'

# An unknown member NAME: Ruby raises NameError, but Crystal 1.21 defines no such
# class (its whole hierarchy is Exception/ArgumentError/IndexError/KeyError/...),
# so [] and []= answer IndexError. Both are errors; only the class differs.
expect_diverge "struct_member_name_error" "no NameError in Crystal, so IndexError" 'P = Struct.new(:a)
p(begin; P.new(1)[:zz]; rescue => e; e.class; end)'

if [ "$ok" = 1 ]; then
  echo "crystal-difference-test: PASS (agree=$n_agree refuse=$n_refuse diverge=$n_diverge)"
else
  echo "crystal-difference-test: FAIL (agree=$n_agree refuse=$n_refuse diverge=$n_diverge)"
  exit 1
fi
