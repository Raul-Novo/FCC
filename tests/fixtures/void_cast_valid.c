// SPDX-License-Identifier: GPL-3.0-or-later

static int consume(int value) {
  return value + 1;
}

int main(void) {
  (void)consume(1);
  (void)0;
  return 0;
}
