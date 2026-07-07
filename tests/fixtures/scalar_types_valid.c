// SPDX-License-Identifier: GPL-3.0-or-later
typedef unsigned long long size_like;
typedef signed char small_signed;

short global_short = 2;
unsigned short global_unsigned_short = 3;
long global_long = 4;
unsigned long global_unsigned_long = 5;
long long global_long_long = 6;
unsigned long long global_unsigned_long_long = 7;
_Bool global_bool = 1;

int main(void) {
  size_like width = sizeof(size_like);
  small_signed small = (signed char)1;

  return sizeof(_Bool) + sizeof(signed char) + sizeof(short) + sizeof(unsigned short) +
         sizeof(long) + sizeof(unsigned long) + sizeof(long long) +
         sizeof(unsigned long long) + global_bool + small + width - 40;
}
