# FCC Examples

These examples show two different paths:

- `minimal_return.c` and `infinite_loop.c` use FCC's internal linker through `--compile-and-link`.
- `hello_world_libc.c`, `libc_counter.c`, and `infinite_hello_libc.c` use FCC to emit COFF
  objects, then use `clang-cl` and the MSVC CRT for libc calls such as `printf`.

Run `msvc` before scripts that link with the MSVC CRT.
