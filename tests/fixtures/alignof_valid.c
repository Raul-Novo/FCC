typedef int Word;

typedef struct Layout {
  unsigned char tag;
  int value;
  unsigned char tail;
} Layout;

typedef union Value {
  unsigned char byte;
  long long wide;
} Value;

int global_align = _Alignof(Layout) + _Alignof(union Value);

int main(void) {
  return global_align + _Alignof(char) + _Alignof(short) + _Alignof(int) +
         _Alignof(long long) + _Alignof(Word*) + _Alignof(int[3]) - 39;
}
