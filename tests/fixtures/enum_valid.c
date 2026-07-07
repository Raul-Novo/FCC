enum {
  BASE = 3,
  SHIFTED = BASE << 2,
  AUTO_VALUE
};

typedef enum Color {
  COLOR_RED,
  COLOR_GREEN = 7,
  COLOR_BLUE
} Color;

int enum_total = AUTO_VALUE + COLOR_BLUE;
int enum_size = sizeof(enum Color);

int main(void) {
  enum { LOCAL_VALUE = COLOR_GREEN + 1 };
  Color color;
  color = COLOR_RED;
  return enum_total + enum_size + LOCAL_VALUE + color - 33;
}
