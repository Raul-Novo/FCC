int sum_to(int limit) {
  int total = 0;
  int i = 0;

  while (i < limit) {
    total = total + i;
    i = i + 1;
  }

  return total;
}

void spin(void) {
  int i = 0;

  for (i = 0; i < 4; i = i + 1) {
    if (i == 2) {
      continue;
    } else {
      break;
    }
  }

  return;
}
