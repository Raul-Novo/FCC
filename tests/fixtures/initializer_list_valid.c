struct Info {
  char name[8];
  int value;
};

static const struct Info info = {
    "FCC",
    3,
};

int main(void) {
  return info.value;
}
