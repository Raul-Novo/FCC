// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcc/driver.h"

#include <stdio.h>

int main(int argc, char** argv) {
  return fcc_driver_run(argc, (const char* const*)argv, stdout, stderr);
}
