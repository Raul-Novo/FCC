#define NEEDS_TWO(first, second, ...) first

int value(void) {
  return NEEDS_TWO(1);
}
