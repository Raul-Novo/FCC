typedef struct Entry {
  const char* text;
  unsigned long length;
  int token;
} Entry;

int lookup(void) {
  static const Entry entries[] = {{"int", 3, 6}, {"void", 4, 7}};
  return entries[1].token;
}
