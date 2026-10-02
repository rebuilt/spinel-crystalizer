#!/usr/bin/env bash
# Emit the Crystal backend's full-corpus file list, deterministically, so the
# metrics in docs/internals/crystal-backend.md are reproducible from a clean tree.
#
# The corpus is every top-level test/*.rb EXCEPT:
#   - test/promote_*.rb       only have defined output under --int-overflow=promote,
#                             which the Crystal target does not implement. The same
#                             filter the Makefile applies to the C backend's
#                             raise/wrap runs, so those programs are not a Crystal
#                             translation gap.
#   - test/crystal_struct.rb  a whole-file test pinned against .expected and run by
#                             scripts/crystal_difference_test.sh, not the corpus.
#
# Usage: crystal_corpus_list.sh [repo-root]
set -u
cd "${1:-$(git rev-parse --show-toplevel)}" || exit 1
find test -maxdepth 1 -name '*.rb' | sort | grep -v -E '/promote_[^/]*\.rb$|/crystal_struct\.rb$'
