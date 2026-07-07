typedef unsigned char BYTE;

int global_newline = (int)'\n';

int main(void) {
  BYTE value;
  value = (BYTE)260;
  return value + (int)'\n' + (int)'\\' + global_newline - 116;
}
