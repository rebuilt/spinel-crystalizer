#!/usr/bin/env bash
# Corpus gate for the Crystal backend.
#
# Three things this gets right that a naive `xargs -P` loop does not:
#
#  1. Every build gets its OWN CRYSTAL_CACHE_DIR. Sharing one cache across
#     parallel `crystal build` invocations corrupts it and produces ~90 bogus
#     "compiler bug" reports that look exactly like real failures.
#  2. Reference (CRuby) and generated-program output are compared SERIALLY,
#     after all builds finish. Comparing while builds are still running mixes
#     partial results.
#  3. Build failures and output differences are reported separately, because
#     they mean different things: a BUILDFAIL is a rejected translation, a DIFF
#     is a translation that compiles and disagrees with Ruby.
#  4. Work files are keyed by the repo-RELATIVE path, not the basename. The
#     corpus has duplicate basenames in subdirectories (test/main.rb,
#     test/require/main.rb, test/collect/seed/main.rb all basename to `main`),
#     so a basename key silently overwrote one with another: four programs were
#     never built or compared, and every reported total was short by four.
#     `crystal_key` maps the path to a flat unique id.
#
# Usage: crystal_corpus.sh <list-of-ruby-files> <outdir> [jobs]
#
# To build the list of programs the target emits cleanly:
#   find test -name '*.rb' | sort | xargs -P 16 -I{} sh -c \
#     'bin/spinel --target crystal {} -S >/dev/null 2>&1 && echo {}' | sort -u > ok.txt
set -u
LIST="$1"; OUT="$2"; JOBS="${3:-12}"
cd "$(git rev-parse --show-toplevel)" || exit 1
REPO=$PWD
SPINEL="$REPO/bin/spinel"
WORK="$OUT/work"; CACHE="$OUT/cache"; LOGS="$OUT/logs"
rm -rf "$OUT"; mkdir -p "$WORK" "$CACHE" "$LOGS"

# A flat, unique, filesystem-safe id for a corpus program. Two different sources
# must never map to the same id, or one result silently replaces the other.
crystal_key() {
  local f="$1" r
  r="${f#"$REPO"/}"
  r="${r#./}"
  printf '%s' "$r" | tr '/' '_' | sed 's/\.rb$//'
}
export -f crystal_key; export REPO

emit_one() {
  f="$1"; n=$(crystal_key "$f")
  cr="$WORK/$n.cr"
  if ! "$SPINEL" --target crystal "$f" -S >"$cr" 2>"$LOGS/$n.emit"; then
    echo "REFUSED" > "$LOGS/$n.status"; return
  fi
  CRYSTAL_CACHE_DIR="$CACHE/$n" timeout 300 crystal build --release -o "$WORK/$n.bin" "$cr" \
    >"$LOGS/$n.build" 2>&1 || { echo "BUILDFAIL" > "$LOGS/$n.status"; return; }
  echo "BUILT" > "$LOGS/$n.status"
}
export -f emit_one; export SPINEL WORK CACHE LOGS
grep -v '^[[:space:]]*$' "$LIST" | sort -u | xargs -P "$JOBS" -I{} bash -c 'emit_one "$@"' _ {}

# Serial phase: reference and comparison. No parallelism here on purpose.
#
# The list is read on fd 3, not stdin, and the programs get stdin from
# /dev/null. A test program that reads stdin would otherwise consume the LIST
# itself: the loop would see EOF early and silently report only the programs up
# to that point, with a correct-looking totals block for the truncated set.
: > "$OUT/results.txt"
while read -r f <&3; do
  [ -n "$f" ] || continue
  n=$(crystal_key "$f")
  st=$(cat "$LOGS/$n.status" 2>/dev/null)
  case "$st" in
    REFUSED)  printf 'REFUSED   %s\n'  "$n" >> "$OUT/results.txt"; continue ;;
    BUILDFAIL)printf 'BUILDFAIL %s\n' "$n" >> "$OUT/results.txt"; continue ;;
  esac
  ref=$(cd "$REPO" && timeout 30 ruby "$f" 2>/dev/null </dev/null)
  got=$(cd "$REPO" && timeout 30 "$WORK/$n.bin" 2>/dev/null </dev/null)
  if [ "$ref" = "$got" ]; then v=PASS; else v=DIFF; fi
  printf '%-8s %s\n' "$v" "$n" >> "$OUT/results.txt"
done 3< "$LIST"

echo "--- totals ---"
awk '{c[$1]++} END {for (k in c) printf "%-10s %d\n", k, c[k]}' "$OUT/results.txt" | sort
echo "programs: $(wc -l < "$LIST")"
