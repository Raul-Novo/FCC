typedef int (*Unary)(int);
typedef int (*Binary)(int, int);

int declared_callback(Unary, int);

int inc(int value) {
  return value + 1;
}

int twice(int value) {
  return value * 2;
}

int add(int left, int right) {
  return left + right;
}

int apply(Unary fn, int value) {
  return fn(value);
}

int apply_explicit(Unary fn, int value) {
  return (*fn)(value);
}

int combine(Binary fn, int left, int right) {
  return fn(left, right);
}

int main(void) {
  Unary fn;
  Unary address_fn;

  fn = inc;
  address_fn = &inc;
  return apply(fn, 4) + apply(twice, 3) + apply_explicit(address_fn, 4) +
         combine(add, 2, 3) - 21;
}
