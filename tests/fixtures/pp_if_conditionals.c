#define VALUE 21
#define ENABLED 1
#define DISABLED 0

#if defined(ENABLED) && VALUE * 2 == 42
int if_value(void) {
  return __FCC__;
}
#else
int if_value(void) {
  return 0;
}
#endif

#if DISABLED
int elif_value(void) {
  return 0;
}
#elif !defined(MISSING) && (3 << 2) == 12
int elif_value(void) {
  return VALUE + 1;
}
#else
int elif_value(void) {
  return 1;
}
#endif

#if UNKNOWN_NAME
int inactive_unknown_name(void) {
  return 0;
}
#endif

#if defined VALUE
int defined_without_parentheses(void) {
  return VALUE;
}
#endif
