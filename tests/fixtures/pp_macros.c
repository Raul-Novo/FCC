#define VALUE 42
#define FLAG

int first(void) {
  return VALUE;
}

#ifdef FLAG
int second(void) {
  return VALUE;
}
#else
int second(void) {
  return 0;
}
#endif

#undef FLAG

#ifndef FLAG
int third(void) {
  return VALUE;
}
#endif
