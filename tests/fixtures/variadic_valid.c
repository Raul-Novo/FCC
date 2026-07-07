int any(...);
int fixed(int value, ...);

int main(void) {
  any();
  any(1);
  fixed(1);
  fixed(1, 2, 3);
  return 0;
}
