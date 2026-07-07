static int value = 10;

int main(void) {
  value += 1;
  value--;
  ++value;
  value = ~value / 2 % 1 - 3 * 4;
  value = array[0] + object.member;
  value = pointer->member;
  value = func(value, 1);
  if (value >= 11 && value != 0) {
    value = value << 1;
  } else {
    value = value | 4 ^ 2 & 1;
  }
  // done
  return !value ? value : 0;
}
