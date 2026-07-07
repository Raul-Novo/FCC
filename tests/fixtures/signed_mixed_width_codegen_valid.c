// SPDX-License-Identifier: GPL-3.0-or-later

int main(void) {
  long long value;
  value = 13;
  if (value < (-2147483647 - 1)) {
    return 1;
  }

  if (value > (-2147483647 - 1)) {
    value = -1;
  }

  if (value > 4294967295) {
    return 2;
  }

  return 0;
}
