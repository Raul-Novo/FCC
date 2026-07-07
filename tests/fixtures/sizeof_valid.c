int answer(void) {
  int value;

  value = sizeof(int);
  return value + sizeof value + sizeof(1);
}
