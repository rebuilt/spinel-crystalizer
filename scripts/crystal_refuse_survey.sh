#!/usr/bin/env bash
# Refusal-reason survey for the Crystal backend.
#
# Runs `spinel --target crystal <prog> -S` over the corpus (spinel only, no
# Crystal builds, so it is fast) and aggregates the FIRST refusal line of every
# REFUSED program. The buckets size the next phase candidates; the plan's §0.5
# table is this output. Two traps it works around: a shared stderr temp file
# races under xargs -P (each program gets its own log), and some programs'
# stderr carries binary bytes, which plain grep skips (grep -a).
#
# Usage: crystal_refuse_survey.sh [list-file]     (default: crystal_corpus_list.sh)
#        JOBS=8 crystal_refuse_survey.sh         (parallelism, default 16)
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

printf '%s\n' "$programs" | xargs -P "${JOBS:-16}" -I{} bash -c \
  'n=$(echo "$1" | tr "/" "_" | sed "s/\.rb$//")
   bin/spinel --target crystal "$1" -S >/dev/null 2>"$0/$n.log" ||
     grep -a -m1 "^spinel:" "$0/$n.log"' "$TMP" {} \
  | sed -E "s/^spinel: [^:]+:[0-9]+: //; s/ \([0-9]+ total outside the subset\)\$//; s/'[^']*'/'X'/g" \
  | sort | uniq -c | sort -rn
