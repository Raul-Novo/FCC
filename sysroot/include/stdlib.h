/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void abort(...);
void exit(...);
void free(...);
void* calloc(...);
void* malloc(...);
void* realloc(...);

