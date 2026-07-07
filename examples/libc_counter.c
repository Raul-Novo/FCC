#include <stdio.h>

int main(void) {
  int i;

  for (i = 0; i < 5; i = i + 1) {
    printf("counter = %d\n", i);
  }

  return 0;
}
