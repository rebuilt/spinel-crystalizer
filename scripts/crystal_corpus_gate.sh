#!/usr/bin/env bash
# Zero-regression gate for the Crystal corpus.
#
# Compares a corpus results file (as written by scripts/crystal_corpus.sh) against
# the committed baseline and FAILS if any program that PASSed in the baseline no
# longer does. Other transitions are printed for review but are not failures: the
# corpus is expected to gain PASSes and to move BUILDFAILs to honest refusals.
#
# The hard rule is the one every phase in docs/internals/crystal-backend.md is
# held to: a change may add PASSes, may turn DIFF/BUILDFAIL into a named refusal,
# but must never make a previously-PASSing program stop passing.
#
# Usage: crystal_corpus_gate.sh <results.txt> [baseline.txt]
#   results.txt   a run's results.txt (STATUS<tab>key per line)
#   baseline.txt  default test/crystal_corpus_baseline.txt
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
RESULTS="${1:?usage: crystal_corpus_gate.sh <results.txt> [baseline.txt]}"
BASE="${2:-test/crystal_corpus_baseline.txt}"
[ -f "$RESULTS" ] || { echo "crystal-corpus-gate: no such results file: $RESULTS" >&2; exit 2; }
[ -f "$BASE" ]    || { echo "crystal-corpus-gate: no such baseline: $BASE" >&2; exit 2; }

# The baseline is keyed by NAME (field 2) with the status in field 1. A row whose
# status and name ran together (a past formatting slip, e.g. `BUILDFAILtest_x`)
# would land under an empty key and be silently skipped -- including a PASS row,
# which would hide a regression. Validate the shape before comparing.
bad=$(awk 'NF && $1 !~ /^(PASS|DIFF|REFUSED|BUILDFAIL)$/ { n++ } END { print n+0 }' "$BASE")
if [ "$bad" != "0" ]; then
  echo "crystal-corpus-gate: $bad malformed row(s) in $BASE:" >&2
  awk 'NF && $1 !~ /^(PASS|DIFF|REFUSED|BUILDFAIL)$/ { print "  "NR": "$0 }' "$BASE" | head >&2
  exit 2
fi

# base[key]=status, res[key]=status; report regressions and a transition summary.
awk '
  NR==FNR { base[$2]=$1; if ($1=="PASS") np++; next }
  { res[$2]=$1 }
  END {
    bad=0; nreg=0
    for (k in base) {
      if (base[k] != "PASS") continue
      if (res[k] != "PASS") {
        printf "REGRESSION  %s: PASS -> %s\n", k, (res[k]=="" ? "MISSING" : res[k])
        bad=1; nreg++
      }
    }
    for (k in res) if (!(k in base)) { printf "NEW         %s: %s\n", k, res[k] }
    printf "crystal-corpus-gate: %s (%d PASS baselined, %d regressions)\n", (bad ? "FAIL" : "PASS"), np, nreg
    exit bad
  }
' "$BASE" "$RESULTS"
