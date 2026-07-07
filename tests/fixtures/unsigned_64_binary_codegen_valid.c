typedef unsigned long long size_t;

int main(void) {
  size_t small = 2;
  if (small > (18446744073709551615ULL / 2ULL)) {
    return 1;
  }

  return 0;
}
