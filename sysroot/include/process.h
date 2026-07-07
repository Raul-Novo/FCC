/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#define _P_WAIT 0

intptr_t _spawnv(...);
intptr_t _spawnvp(...);
intptr_t _wspawnv(...);
intptr_t _wspawnvp(...);

