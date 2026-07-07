#define ADD(left, right) ((left) + (right))
#define EMPTY() 0
#define SELECT(first, second, third) ((first) ? (second) : (third))
#define ONE_EMPTY(value) (value + 1)
#define STRINGIZE(value) #value
#define PASTE(left, right) left ## right
#define VALUE_TOKEN 42
#define VALUE_ALIAS VALUE_TOKEN
#define VALUE_CHAIN VALUE_ALIAS
#define ADD_WRAPPER(left, right) ADD(left, right)
#define ADD_WRAPPER_ALIAS(left, right) ADD_WRAPPER(left, right)
#define ADD_OBJECT_NAME ADD
#define SELF_TYPE SELF_TYPE
#define VARIADIC_ADD(left, ...) ADD(left, __VA_ARGS__)
#define VARIADIC_ONLY(...) __VA_ARGS__
#define VARIADIC_EMPTY_TAIL(value, ...) value ## __VA_ARGS__
#define VARIADIC_STRINGIZE(...) #__VA_ARGS__
#define VARIADIC_PASTE(left, ...) left ## __VA_ARGS__

typedef int SELF_TYPE;

int add_value(void) {
  return ADD(20, 22);
}

int spaced_invocation(void) {
  return ADD (1, 2);
}

int nested_arguments(void) {
  return ADD((1 + 2), (3 + 4));
}

int empty_value(void) {
  return EMPTY();
}

int empty_argument(void) {
  return ONE_EMPTY();
}

int conditional_value(void) {
  return SELECT(1, 5, 4);
}

int rescanned_object_value(void) {
  return VALUE_CHAIN;
}

int rescanned_function_value(void) {
  return ADD_WRAPPER_ALIAS(5, VALUE_CHAIN);
}

int rescanned_object_function_name(void) {
  return ADD_OBJECT_NAME(6, 7);
}

int variadic_add_value(void) {
  return VARIADIC_ADD(8, VALUE_CHAIN);
}

int variadic_only_value(void) {
  return VARIADIC_ONLY(3 + 4);
}

int variadic_empty_tail_value(void) {
  return VARIADIC_EMPTY_TAIL(11);
}

int variadic_paste_value(void) {
  return VARIADIC_PASTE(4, 2);
}

int pasted_number(void) {
  return PASTE(4, 2);
}

int pasted_identifier(void) {
  int joined_value = 9;
  return PASTE(joined, _value);
}

const char* stringized_expression(void) {
  return STRINGIZE(left + right);
}

const char* stringized_literal(void) {
  return STRINGIZE("quoted");
}

const char* variadic_stringized(void) {
  return VARIADIC_STRINGIZE(left, right);
}

const char* string_literal(void) {
  return "ADD(left, right)";
}

int comment_value(void) {
  return 7; /* ADD(1, 2) */
}

SELF_TYPE self_macro_value(void) {
  return 0;
}
