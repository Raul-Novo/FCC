// SPDX-License-Identifier: GPL-3.0-or-later
int main(void) {
  int values[4];
  int* start = values;
  int* second = start + 1;
  int* also_second = 1 + start;
  int* third = also_second + 1;
  int* back = third - 1;
  int distance = third - start;

  values[0] = 1;
  second[0] = 5;
  third[0] = 7;
  if ((second == also_second) && (back == second) && (third > start)) {
    return values[1] + values[2] + distance - 14;
  }

  return 1;
}
