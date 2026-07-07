int main(void) {
  int value = 0;

  do {
    value = value + 1;
    if (value == 1) {
      goto done;
    }
  } while (value < 3);

done:
  return value;
}
