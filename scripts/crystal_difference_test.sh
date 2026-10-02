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

# Consistent dot notation / renamed predicates: File.exist? is File.exists?.
expect_agree "exist_to_exists" 'p File.exist?("/dev/null")'

# Trailing while/until are not Crystal syntax; we rewrite the modifier form to a
# leading loop.
expect_agree "trailing_while" 'n = 3; n -= 1 while n > 0; p n'

# ---- REFUSE: no Crystal counterpart, so refuse cleanly and name it ----------

expect_refuse "ForNode"                          'for x in [1,2]; p x; end'
expect_refuse "LambdaNode"                       'f = -> { 42 }; p f.call'
expect_refuse "method 'proc' is not in the Crystal subset" 'f = proc { 42 }; p f.call'
expect_refuse "method 'eval' is not in the Crystal subset" 'p eval("1+1")'
expect_refuse "method 'respond_to?' is not in the Crystal subset" 'p "a".respond_to?(:upcase)'
# Regex backreference globals $`/$'/$& are not supported; use $~.pre_match etc.
expect_refuse "BackReferenceReadNode"            '"a" =~ /a/; p $&'
# Data.define is a VALUE type (member-wise ==); a Crystal class compares by
# identity, so we refuse it rather than emit a reference class that would answer
# `==` wrong. (A plain Struct.new is a reference type and IS translated.)
expect_refuse "the constant 'Data'"              'D = Data.define(:a); p D.new(1) == D.new(1)'

# ---- DIVERGE: emitted, but differs from CRuby on a known edge ---------------

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
