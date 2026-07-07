/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int fclose(...);
int fflush(...);
int fgetc(...);
int fputc(...);
int fputs(...);
int fprintf(...);
int snprintf(...);
int sprintf(...);
int printf(...);
FILE* fopen(...);
int fopen_s(...);
FILE* tmpfile(...);
int tmpfile_s(...);
int remove(...);
int fseek(...);
long ftell(...);
size_t fread(...);
size_t fwrite(...);
int ferror(...);
