// SPDX-License-Identifier: GPL-3.0-or-later

typedef struct Entry {
  const char* name;
  int value;
} Entry;

static const char PREFIX[] = "--asm-extension=";
static const Entry ENTRIES[] = {
    {"auto", 1},
    {"none", 2},
};

int main(void) {
  return (int)(sizeof(PREFIX) + (sizeof(ENTRIES) / sizeof(ENTRIES[0])));
}
