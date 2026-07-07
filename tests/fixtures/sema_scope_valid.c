int nested(int value) {
  int result = value;

  {
    int value = 3;
    result = result + value;
  }

  return result;
}
