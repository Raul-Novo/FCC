typedef int (*Unary)(int);

int main(void) {
  Unary fn;
  return fn();
}
