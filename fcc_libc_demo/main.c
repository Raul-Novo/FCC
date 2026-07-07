#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int require_equal(const char* left, const char* right) {
  if (strcmp(left, right) != 0) {
    printf("mismatch: '%s' != '%s'\n", left, right);
    return 1;
  }

  return 0;
}

int main(void) {
  char message[128];
  char copied[128];
  char file_buffer[128];
  char* heap_text;
  FILE* temp_file;
  size_t message_length;
  size_t written_count;
  size_t read_count;

  memset(message, 0, sizeof(message));
  memset(copied, 0, sizeof(copied));
  memset(file_buffer, 0, sizeof(file_buffer));

  snprintf(message, sizeof(message), "FCC libc demo value=%d", 42);
  message_length = strlen(message);

  printf("message: %s\n", message);
  printf("message length: %llu\n", (unsigned long long)message_length);

  memcpy(copied, message, message_length + 1);
  if (require_equal(copied, message) != 0) {
    return EXIT_FAILURE;
  }

  heap_text = (char*)malloc(64);
  if (heap_text == NULL) {
    printf("malloc failed\n");
    return EXIT_FAILURE;
  }

  memset(heap_text, 0, 64);
  sprintf(heap_text, "heap text: %s", "ok");
  printf("%s\n", heap_text);

  temp_file = tmpfile();
  if (temp_file == NULL) {
    printf("tmpfile failed\n");
    free(heap_text);
    return EXIT_FAILURE;
  }

  written_count = fwrite(message, 1, message_length, temp_file);
  if (written_count != message_length) {
    printf("fwrite failed\n");
    fclose(temp_file);
    free(heap_text);
    return EXIT_FAILURE;
  }

  if (fseek(temp_file, 0, SEEK_SET) != 0) {
    printf("fseek failed\n");
    fclose(temp_file);
    free(heap_text);
    return EXIT_FAILURE;
  }

  read_count = fread(file_buffer, 1, message_length, temp_file);
  fclose(temp_file);

  printf("tmpfile read: %s\n", file_buffer);
  printf("bytes read: %llu\n", (unsigned long long)read_count);

  if (require_equal(file_buffer, message) != 0) {
    free(heap_text);
    return EXIT_FAILURE;
  }

  free(heap_text);
  fflush(0);
  return EXIT_SUCCESS;
}
