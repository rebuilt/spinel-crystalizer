#!/usr/bin/env bash
# Proof-of-concept harness for the Spinel Crystal backend.
#
# For each program in examples/crystal_poc/ it:
#   1. runs CRuby to get the reference stdout (the oracle),
#   2. compiles the same source through `spinel --target crystal`,
#   3. runs the resulting native binary and diffs its stdout against CRuby,
#   4. records the backend's own metrics (poly_sites / unsupported) and the
#      wall-clock time of the `crystal build` step.
#
# It is a feasibility probe, not a correctness gate: it reports how much of a
# representative subset translates cleanly and where it does not.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPINEL="${SPINEL:-$ROOT/bin/spinel}"
CORPUS="${CORPUS:-$ROOT/examples/crystal_poc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$SPINEL" ]; then echo "spinel not built: $SPINEL (run: make bin/spinel)"; exit 1; fi
if ! command -v crystal >/dev/null 2>&1; then echo "crystal toolchain not found on PATH"; exit 1; fi

pass=0; fail=0; buildfail=0
total_poly=0; total_unsup=0
printf '%-22s %-8s %-9s %-9s %s\n' "program" "result" "poly" "unsup" "crystal-build"
printf '%-22s %-8s %-9s %-9s %s\n' "-------" "------" "----" "-----" "-------------"

for f in "$CORPUS"/*.rb; do
  name="$(basename "$f" .rb)"
  ref="$(ruby "$f" 2>/dev/null)"

  bin="$WORK/$name"
  metrics="$WORK/$name.metrics"
  start=$(date +%s.%N)
  if "$SPINEL" --target crystal "$f" -o "$bin" 2>"$metrics"; then
    end=$(date +%s.%N)
    build_ms=$(awk "BEGIN{printf \"%.0f\", ($end-$start)*1000}")
    got="$("$bin" 2>/dev/null)"
    poly=$(sed -n 's/.*poly_sites=\([0-9]*\).*/\1/p' "$metrics")
    unsup=$(sed -n 's/.*unsupported=\([0-9]*\).*/\1/p' "$metrics")
    poly=${poly:-0}; unsup=${unsup:-0}
    total_poly=$((total_poly + poly)); total_unsup=$((total_unsup + unsup))
    if [ "$got" == "$ref" ]; then
      printf '%-22s %-8s %-9s %-9s %s ms\n' "$name" "PASS" "$poly" "$unsup" "$build_ms"
      pass=$((pass+1))
    else
      printf '%-22s %-8s %-9s %-9s %s ms\n' "$name" "DIFF" "$poly" "$unsup" "$build_ms"
      fail=$((fail+1))
      echo "  --- $name: output differs from CRuby ---"
      diff <(printf '%s' "$ref") <(printf '%s' "$got") | sed 's/^/    /' | head -20
    fi
  else
    printf '%-22s %-8s %-9s %-9s %s\n' "$name" "BUILD-FAIL" "-" "-" "-"
    buildfail=$((buildfail+1))
    echo "  --- $name: crystal build failed ---"
    sed -n '1,12p' "$metrics" | sed 's/^/    /'
  fi
done

echo
total=$((pass + fail + buildfail))
echo "programs: $total   pass: $pass   diff: $fail   build-fail: $buildfail"
echo "sum poly_sites=$total_poly   sum unsupported=$total_unsup"
