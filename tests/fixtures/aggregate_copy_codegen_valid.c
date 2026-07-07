// SPDX-License-Identifier: GPL-3.0-or-later

typedef struct Pair {
  int left;
  int right;
} Pair;

int main(void) {
  Pair first = {0};
  Pair second;

  first.left = 3;
  first.right = 4;
  second = first;
  return second.left + second.right;
}
