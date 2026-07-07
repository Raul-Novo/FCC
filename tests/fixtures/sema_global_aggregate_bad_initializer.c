int seed = 3;

typedef struct Box {
  int value;
} Box;

static Box box = {seed};

int main(void) {
  return box.value;
}
