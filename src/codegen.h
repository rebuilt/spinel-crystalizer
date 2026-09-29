#ifndef SPINEL_CODEGEN_H
#define SPINEL_CODEGEN_H

#include "node_table.h"

/* Generate the full C translation unit for the program in `nt`.
   Returns a malloc'd NUL-terminated buffer (caller frees). Aborts the
   process with a diagnostic on an unsupported construct. */
char *codegen_program(const NodeTable *nt);

/* Crystal proof-of-concept backend: same analysis, emits Crystal source
   instead of C. Covers a small concrete subset; anything outside it is
   counted as a metric (see src/codegen_crystal.c). Returns a malloc'd
   NUL-terminated buffer (caller frees). */
char *codegen_program_crystal(const NodeTable *nt);

#endif
