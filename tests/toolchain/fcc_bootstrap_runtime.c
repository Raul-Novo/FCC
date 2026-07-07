// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>

#undef stdin
#undef stdout
#undef stderr

FILE* __cdecl __acrt_iob_func(unsigned int index);

FILE* stdin;
FILE* stdout;
FILE* stderr;

void assert(int condition);

static void fcc_bootstrap_init_stdio(void) {
  stdin = __acrt_iob_func(0);
  stdout = __acrt_iob_func(1);
  stderr = __acrt_iob_func(2);
}

void assert(int condition) {
  if (!condition) {
    fcc_bootstrap_init_stdio();
    fputs("assertion failed\n", stderr);
    abort();
  }
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) __attribute__((used)) static void (*const FCC_BOOTSTRAP_INIT_STDIO)(
    void) = fcc_bootstrap_init_stdio;
