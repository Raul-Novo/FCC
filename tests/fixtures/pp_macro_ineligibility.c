#define VALUE 42
#define SELF_PLUS SELF_PLUS + VALUE
#define LEFT RIGHT + VALUE
#define RIGHT LEFT

int self_value(void) {
  return SELF_PLUS;
}

int mutual_value(void) {
  return LEFT;
}
