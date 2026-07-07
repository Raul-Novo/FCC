int takes_const_pointer(const char* const* value);

int main(int argc, char** argv) {
  (const char* const*)argv;
  return argc;
}

