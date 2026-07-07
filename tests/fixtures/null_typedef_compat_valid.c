typedef enum Kind {
  KIND_A = 1,
  KIND_B = 2,
} Kind;

typedef struct Node Node;

struct Node {
  Kind kind;
  Node* next;
};

static int read_const_node(const Node* node) {
  return node->kind;
}

int main(void) {
  Node node;
  Node* current = 0;
  const Node* const_current;

  node.kind = KIND_A;
  node.next = 0;
  current = &node;
  const_current = current;
  if ((current != 0) && (node.kind == KIND_A)) {
    return read_const_node(current) != const_current->kind;
  }

  return 1;
}
