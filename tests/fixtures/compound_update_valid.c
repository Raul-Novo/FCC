// SPDX-License-Identifier: GPL-3.0-or-later
int main(void) {
  int value = 1;
  int values[3];
  int* cursor = values;

  value += 2;
  value *= 3;
  value -= 1;
  ++value;
  value--;
  values[0] = 3;
  values[1] = 4;
  cursor++;
  cursor[0] += value;
  --cursor;
  cursor += 2;
  cursor--;
  return values[0] + values[1] + cursor[0] - 27;
}
