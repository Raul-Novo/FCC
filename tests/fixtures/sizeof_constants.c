typedef int Word;

typedef struct Layout {
  unsigned char a;
  int b;
  unsigned char c;
} Layout;

typedef union Value {
  int as_int;
  unsigned char bytes[4];
} Value;

int global_size = sizeof(Layout) + sizeof(union Value);

int main(void) {
  int local[2][3];
  return global_size + sizeof local + sizeof(Word*) - 48;
}
