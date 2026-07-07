// SPDX-License-Identifier: GPL-3.0-or-later

typedef struct Pair {
  int left;
  int right;
} Pair;

static Pair copy_pair(const Pair* source) {
  Pair destination;

  destination = *source;
  return destination;
}

int main(void) {
  Pair pair;
  Pair copied;

  pair.left = 1;
  pair.right = 2;
  copied = copy_pair(&pair);
  return copied.left + copied.right;
}
