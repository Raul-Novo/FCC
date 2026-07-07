typedef enum Mode {
  MODE_ZERO,
  MODE_ONE,
  MODE_TWO
} Mode;

int select_value(Mode value) {
  int result;
  result = 0;
  switch (value) {
    case MODE_ZERO:
      result = 11;
      break;
    case MODE_ONE:
      result = 22;
      break;
    case MODE_TWO:
      result = 30;
    default:
      result = result + 3;
      break;
  }

  return result;
}

int main(void) {
  return select_value(MODE_ZERO) + select_value(MODE_ONE) + select_value(MODE_TWO) +
         select_value(42) - 69;
}
