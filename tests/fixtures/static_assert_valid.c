typedef int Word;

enum {
  WORD_SIZE = 4
};

_Static_assert(sizeof(Word) == WORD_SIZE, "Word should use the Windows int size");

struct Checked {
  _Static_assert(_Alignof(long long) == 8, "long long should keep Windows x64 alignment");
  int value;
};

int main(void) {
  _Static_assert(sizeof(struct Checked) == 4, "Checked should contain one int field");
  return sizeof(struct Checked) - 4;
}
