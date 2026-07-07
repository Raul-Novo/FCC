typedef int (*Unary)(int);

typedef struct HandlerPair {
  Unary first;
  Unary second;
} HandlerPair;

int inc(int value) {
  return value + 1;
}

int twice(int value) {
  return value * 2;
}

static Unary current = inc;
static Unary explicit_current = &twice;
static HandlerPair handlers = {inc, &twice};

int main(void) {
  return current(4) + explicit_current(3) - 11;
}
