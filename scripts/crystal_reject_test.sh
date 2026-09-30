#!/usr/bin/env bash
# The Crystal backend's honesty gate: a construct it cannot translate must be
# REFUSED, naming the reason, rather than emitted as Crystal that fails to
# compile with a message about the wrong thing.
#
# A refusal is the contract. Emitting `Data` or `GC.start` and letting Crystal
# report "undefined constant Data" looks like a working translation until the
# build fails, and the diagnostic points at the compiler rather than at the
# missing feature. So each case below must (a) exit non-zero and (b) say WHY.
#
# Usage: crystal_reject_test.sh [path-to-spinel]
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
SPINEL="${1:-bin/spinel}"
tmp=$(mktemp -d /tmp/spinel-crystal-reject.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
ok=1

# expect_refuse <substring-of-diagnostic> <ruby source>
expect_refuse() {
  local want="$1" src="$2" out
  printf '%s\n' "$src" > "$tmp/case.rb"
  if out=$("$SPINEL" --target crystal "$tmp/case.rb" -S 2>&1); then
    echo "FAIL: compiled, but must be refused: $src"
    ok=0
    return
  fi
  case "$out" in
    *"$want"*) ;;
    *) echo "FAIL: refused without saying why (wanted: $want)"; echo "$out" | head -3; ok=0 ;;
  esac
}

# ---- constants with no Crystal counterpart -----------------------------------
expect_refuse "the constant 'Data'"          'p Data'
expect_refuse "the constant 'Socket'"        'p Socket'
expect_refuse "the constant 'Enumerator'"    'p Enumerator'
expect_refuse "the constant 'Queue'"         'q = Queue.new; p q'
expect_refuse "the constant 'RUBY_ENGINE'"   'p RUBY_ENGINE'
expect_refuse "the constant 'Float::NAN'"    'p Float::NAN'

# ---- methods that do not exist on this receiver ------------------------------
expect_refuse "method 'start' is not available on this receiver" 'GC.start'
expect_refuse "method 'stat' is not available on this receiver"  'p GC.stat'
expect_refuse "method 'value' is not available on this receiver" 'Thread.new { 1 }.value'
expect_refuse "method 'locked?' is not available on this receiver" 'm = Mutex.new; p m.locked?'
# String is immutable, so the in-place append has no Crystal form...
expect_refuse "method '<<' is not available on this receiver" 's = "a"; s << "b"'
# ...while the same name on an Array is fine, so the rule must be receiver-aware.
if ! "$SPINEL" --target crystal -e 'a = [1]; a << 2' -S >/dev/null 2>&1; then
  echo "FAIL: Array#<< was refused; the rule must be per-receiver, not per-name"
  ok=0
fi
# Time.at only translates for the one shape it matches.
expect_refuse "method 'at' is not available on this receiver" 'p Time.at(1.5)'

if [ "$ok" = 1 ]; then
  echo "crystal-reject-test: PASS"
else
  echo "crystal-reject-test: FAIL"
  exit 1
fi
