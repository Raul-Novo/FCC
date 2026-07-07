struct Layout {
  unsigned char a;
  int b;
  unsigned char c;
} layout_global;

union Value {
  int as_int;
  unsigned char bytes[4];
} value_global;

struct Node* head;

struct Node {
  int value;
  struct Node* next;
} node_global;

int size_values(void) {
  return sizeof(struct Layout) + sizeof(union Value) + sizeof(struct Node);
}
