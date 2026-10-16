#!/usr/bin/env bash
# Obstacle-set survey for the Crystal backend (E10).
#
# A program is REFUSED at its FIRST unsupported construct, so the first-obstacle
# histogram produced by crystal_refuse_survey.sh is only an UPPER BOUND on what
# fixing a bucket can buy: the program usually has a second obstacle behind the
# first. The emitter already scans the whole program; with SPINEL_CRYSTAL_WHYS=1
# it prints EVERY unsupported reason, so this survey can report, per reason:
#
#   first : programs whose FIRST obstacle is this reason (the refuse survey)
#   only  : programs whose ONLY obstacle is this reason -- what a fix clears
#           outright, i.e. the real upper bound on the PASS gain.
#
# A large `first` with a small `only` means the bucket is a shallow mislabel or
# an early gate that hides a long tail; a large `only` is a genuine rung.
#
# Usage: crystal_obstacle_survey.sh [list-file]   (default: crystal_corpus_list.sh)
#        JOBS=8 crystal_obstacle_survey.sh
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
LIST="${1:-}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ -n "$LIST" ]; then
  programs=$(grep -v '^[[:space:]]*$' "$LIST" | sort -u)
else
  programs=$(scripts/crystal_corpus_list.sh)
fi
N=$(printf '%s\n' "$programs" | grep -c .)

# One log per program (a shared stderr temp file races under xargs -P). The
# program gets stdin from /dev/null so a stdin-reading test cannot eat the list.
export SPINEL_CRYSTAL_WHYS=1
printf '%s\n' "$programs" | xargs -P "${JOBS:-16}" -I{} bash -c \
  'n=$(echo "$1" | tr "/" "_" | sed "s/\.rb$//")
   bin/spinel --target crystal "$1" -S >/dev/null 2>"$0/$n.log" </dev/null || true' \
  "$TMP" {}

# Aggregate in ONE awk pass over the per-program logs. The first version of
# this looped over the logs in bash, forking grep+sed four times per program;
# at 7,347 programs that is ~30,000 forks and the survey outlasted the parallel
# phase that produced the logs. awk does the same job in one process.
#
# A log holds one program. Its first "spinel: ..." line is that program's FIRST
# obstacle; the SET of distinct "spinel-crystal-why: ..." lines is its whole
# obstacle set. Normalization is unchanged, so the buckets do not move: strip the
# lead-in, drop the trailing "(N total outside the subset)", collapse a quoted
# name to 'X'.
cat > "$TMP/agg.awk" <<'AWK'
FNR == 1 {
  if (first != "") { print first > firstfile; if (ns == 1) for (k in seen) print k > solefile }
  first = ""; ns = 0; delete seen
}
/^spinel: / && first == "" {
  first = $0
  sub(/^spinel: [^:]+:[0-9]+: /, "", first)
  sub(/ \([0-9]+ total outside the subset\)$/, "", first)
  gsub(/'[^']*'/, "'X'", first)
}
/^spinel-crystal-why: / {
  s = $0
  sub(/^spinel-crystal-why: /, "", s)
  gsub(/'[^']*'/, "'X'", s)
  if (!(s in seen)) { seen[s] = 1; ns++ }
}
END {
  if (first != "") { print first > firstfile; if (ns == 1) for (k in seen) print k > solefile }
}
AWK
: > "$TMP/first.txt"; : > "$TMP/sole.txt"
awk -f "$TMP/agg.awk" firstfile="$TMP/first.txt" solefile="$TMP/sole.txt" "$TMP"/*.log

refused=$(wc -l < "$TMP/first.txt" | tr -d ' ')
sole=$(wc -l < "$TMP/sole.txt" | tr -d ' ')

echo "programs: $N   refused: $refused   sole-obstacle: $sole"
echo
echo "=== first-obstacle histogram (top 25) ==="
awk '{c[$0]++} END {for (k in c) printf "%6d  %s\n", c[k], k}' "$TMP/first.txt" | sort -rn | head -25
echo
echo "=== sole-obstacle histogram -- what one fix clears (top 25) ==="
awk '{c[$0]++} END {for (k in c) printf "%6d  %s\n", c[k], k}' "$TMP/sole.txt" | sort -rn | head -25
