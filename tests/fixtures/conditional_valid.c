int pick(int value) {
  return value ? 2 : 3;
}

const char* choose(int flag, const char* value) {
  return flag ? value : "";
}

