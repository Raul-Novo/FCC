// SPDX-License-Identifier: GPL-3.0-or-later

int escape_alarm = (int)'\a';
int escape_backspace = (int)'\b';
int escape_formfeed = (int)'\f';
int escape_vertical = (int)'\v';
int escape_question = (int)'\?';

int sum_escapes(void) {
  return (int)'\a' + (int)'\b' + (int)'\f' + (int)'\v' + (int)'\?';
}
