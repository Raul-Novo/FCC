// SPDX-License-Identifier: GPL-3.0-or-later

static const char* const PARAMETER_REGISTERS[4] = {
    "rcx",
    "rdx",
    "r8",
    "r9",
};

static int consume_values(int values[4]) {
  int* cursor;
  cursor = values;
  return cursor[0];
}

int main(void) {
  return consume_values(0);
}
