#define SPLICE_VALUE 4\
2

int spliced_directive(void) {
  return SPLICE_VALUE;
}

int spliced_code(void) {
  return 20 + \
22;
}

#if 1 \
&& defined(SPLICE_VALUE)
int spliced_if(void) {
  return 7;
}
#endif
