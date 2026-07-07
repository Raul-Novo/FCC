// SPDX-License-Identifier: GPL-3.0-or-later
typedef struct Pair {
  int left;
  int right;
} Pair;

int sum_pair(Pair* pair) {
  return pair->left + pair->right;
}

int main(void) {
  Pair pair;
  int values[4];

  pair.left = 3;
  pair.right = 4;
  values[0] = pair.left;
  values[1] = pair.right;
  values[2] = values[0] + values[1];
  values[3] = sum_pair(&pair);
  return values[2] + values[3] - 14;
}
