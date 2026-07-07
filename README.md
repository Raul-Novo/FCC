# FCC

**FCC** is the **Flintine C Compiler**.

Here, Flintine doesn't mean anything, it is just the name we gave to the compiler.

This is an experimental C compiler for **Windows x86_64**.
It is still a work in progress, but it already supports a useful and test-backed subset of C17.

The goal of FCC is not to be a production-ready compiler yet.
For now, it is a learning, research, and engineering project focused on building a real compiler step by step: source loading, diagnostics, lexing, parsing, semantic analysis, code generation, object generation, linking, and eventually self-hosting.

## AI Assistance Notice

Most parts of FCC were built with significant AI assistance.

That does **not** mean the whole project is blindly AI-generated.
The overall direction, testing, review, debugging, integration decisions, and many fixes were handled manually. However, a large portion of the implementation, refactoring, test design, and documentation was created or heavily assisted by AI.

This repository should be understood as an **AI-assisted compiler project**, not as a fully hand-written compiler.

## Current Target

FCC currently targets:

* Windows 11
* x86_64
* C17 subset
* NASM-style x86_64 assembly
* COFF object output
* Internal PE/COFF executable linking for the narrow shape FCC emits today

The current toolchain path uses:

* `clang-cl`
* `link.exe` from the MSVC environment
* `nasm`
* PowerShell
* `make`

## Build

Before building anything that uses `link.exe`, open a PowerShell environment where MSVC tools are available.

Then you can build FCC with:

```powershell
make debug
```

Other useful commands:

```powershell
make release
make asan
make test
make selfhost
make clean
```

## Basic Usage

FCC uses explicit driver modes.
For example, you choose whether you want to lex, parse, check, emit assembly, emit an object file, or build an executable.

Examples:

```powershell
build/bin/fcc.exe --version
```

Check a C file:

```powershell
build/bin/fcc.exe --check tests/fixtures/sample.c
```

Preprocess a file:

```powershell
build/bin/fcc.exe --preprocess tests/fixtures/pp_include_main.c
```

Preprocess with an include directory:

```powershell
build/bin/fcc.exe --preprocess -I tests/include tests/fixtures/pp_include_search_main.c
```

Emit assembly:

```powershell
build/bin/fcc.exe --emit-asm --output build/sample.s tests/fixtures/sample.c
```

Emit a COFF object file:

```powershell
build/bin/fcc.exe --emit-obj --output build/sample.obj tests/fixtures/sample.c
```

Compile and link an executable:

```powershell
build/bin/fcc.exe --compile-and-link --output build/sample.exe tests/fixtures/sample.c
```

## Driver Modes

FCC currently supports these main modes:

| Mode                 | Description                                             |
| -------------------- | ------------------------------------------------------- |
| `--lex-only`         | Stops after lexing                                      |
| `--parse-only`       | Stops after parsing                                     |
| `--check`            | Stops after semantic analysis                           |
| `--preprocess`       | Runs the standalone preprocessor                        |
| `--emit-asm`         | Emits NASM-style x86_64 assembly                        |
| `--emit-obj`         | Emits a COFF object file                                |
| `--compile-and-link` | Builds a Windows executable using FCC's internal linker |
| `--version`          | Prints compiler version and signature information       |

Useful debugging options include:

```powershell
--dump-tokens
--dump-ast
```

For example:

```powershell
build/bin/fcc.exe --lex-only --dump-tokens tests/fixtures/sample.c
build/bin/fcc.exe --parse-only --dump-ast tests/fixtures/sample.c
```

## What Works Today

FCC already has working support for several compiler stages.

### Frontend

Implemented pieces include:

* source file loading
* UTF-8 path handling on Windows
* line and column tracking
* diagnostic messages
* token definitions
* lexer
* parser
* AST
* semantic analysis
* symbol scopes
* tag scopes
* type IDs
* integer constant expression evaluation
* lvalue tracking
* basic initializer checking
* struct, union, and enum support for the current subset

### C Language Support

FCC currently supports a growing subset of C17, including:

* functions
* declarations
* typedefs
* recursive declarators
* arrays
* function pointer declarators in the current supported style
* structs
* unions
* enums
* labels and `goto`
* `if`, `while`, `for`, `do while`
* `switch`
* casts
* `sizeof`
* `_Alignof`
* `_Static_assert`
* conditional expressions
* compound assignments
* increment and decrement expressions
* initializer lists
* pointer arithmetic
* member access
* subscripting
* string literals
* integer literals
* character literals
* comments

### Integer Types

The current Windows x64 model supports scalar integer handling for:

* `_Bool`
* `char`
* `signed char`
* `unsigned char`
* `short`
* `unsigned short`
* `int`
* `unsigned int`
* `long`
* `unsigned long`
* `long long`
* `unsigned long long`

### Code Generation

FCC can currently emit NASM-style x86_64 assembly for supported integer, pointer, and aggregate cases.

Supported codegen areas include:

* local variables
* globals
* integer and pointer expressions
* string literals
* function calls
* many-argument calls
* indirect calls through function pointers
* file-scope function pointer initializers
* switch statements
* aggregate copies
* selected aggregate parameters
* local and global aggregate initializers
* signed and unsigned width-sensitive behaviour

The emitted assembly is intended for:

```powershell
nasm -f win64
```

## Preprocessor

FCC has a standalone preprocessor mode:

```powershell
build/bin/fcc.exe --preprocess tests/fixtures/example.c
```

Current preprocessor support includes:

* quoted local includes
* `-I` include directories
* `#pragma once`
* object-like macros
* function-like macros
* `#define`
* `#undef`
* macro parameter substitution
* stringizing
* token pasting
* variadic macro tails with `__VA_ARGS__`
* line splicing
* bounded recursive rescanning
* `#if`
* `#elif`
* `defined`
* `#ifdef`
* `#ifndef`
* `#else`
* `#endif`
* `#error`
* initial predefined target macros

Normal compile stages also preprocess input before lexing and parsing.

## Internal Linker

FCC includes an early internal linker subsystem called `fcc_link`.

At the moment, it can:

* assemble FCC-generated NASM output with `nasm`
* read the narrow COFF object shape FCC currently emits
* produce PE32+ Windows executables
* embed a minimal `asInvoker` application manifest in `.rsrc`

This means FCC can already build simple Windows console executables without using an external C compiler or external linker for the final link step.

Example:

```powershell
build/bin/fcc.exe --compile-and-link --output build/sample.exe tests/fixtures/sample.c
```

The internal linker is intentionally limited right now.
It is not a general-purpose replacement for `link.exe` yet.

## Known Limits

FCC is still incomplete. Some parser support is ahead of semantic analysis and code generation, so a construct should only be considered properly supported when it has tests across the full pipeline.

Current limitations include:

* no full floating-point support yet
* no complete Windows x64 floating-point ABI handling
* no full variadic call support
* incomplete VLA support
* incomplete C17 declaration and type compatibility rules
* incomplete qualifier support
* no full `_Atomic`, `volatile`, `restrict`, or `_Alignas` support yet
* no bit-fields yet
* no compound literals yet
* no designated initializers yet
* no complete system-header or hosted standard-library support
* no multi-object linking yet
* no static library or import library support yet
* no CRT startup integration yet
* limited optimisation support

Also, `sizeof` and `_Alignof` currently use FCC's internal integer expression type instead of the target `size_t` typedef.

## Example C Input

A very small example:

```c
int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(20, 22);
}
```

Compile and link it:

```powershell
build/bin/fcc.exe --compile-and-link --output build/example.exe example.c
```

Run it:

```powershell
build/example.exe
echo $LASTEXITCODE
```

Expected result:

```text
42
```

## Assembly Emission

FCC emits NASM-style Intel syntax assembly for Windows x86_64.

Example:

```powershell
build/bin/fcc.exe --emit-asm tests/fixtures/codegen_answer.c
```

By default, FCC uses the `.s` extension.

You can request `.asm` instead:

```powershell
build/bin/fcc.exe --emit-asm --asm-extension=asm tests/fixtures/codegen_answer.c
```

Or choose the full output path yourself:

```powershell
build/bin/fcc.exe --emit-asm --output build/codegen_answer.s tests/fixtures/codegen_answer.c
```

## Project Status

FCC is not finished, but it is already much more than a toy lexer or parser.

It currently has:

* a real compiler pipeline
* regression tests
* semantic checks
* x86_64 assembly generation
* COFF object emission
* an early PE/COFF executable linker
* a growing C17 subset
* early self-hosting work
