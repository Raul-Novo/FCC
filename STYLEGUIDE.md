# Universal C Style Guide (Windows 11, Clang, NASM, Simple Build)

## 1. Scope and Philosophy

This document is normative for all C code in the project, including:

* application code
* library code
* platform layers
* runtime support
* tools written in C
* tests and validation harnesses
* low-level startup code
* architecture-specific code
* user-facing ABI headers where applicable
* assembly modules that ship with the project

This guide is intentionally strict. Its purpose is to maximize:

* correctness before convenience
* clarity before cleverness
* explicit behavior before hidden assumptions
* maintainability before micro-optimization
* reviewability before personal coding preference
* deterministic behavior before “it probably works”

The project style is designed for long-lived codebases that must remain readable, debuggable,
and stable under growth.

Core principles:

* C17 is mandatory.
* Clang is the mandatory compiler.
* The target platform is Windows 11 only.
* `#pragma once` is preferred over traditional include guards.
* Assembly is allowed only where needed and must use NASM, Intel syntax, x86_64.
* The Microsoft linker (`link.exe`) and LLVM linker (`lld-link`) are both valid.
* Build systems must remain simple.
* Rules inspired by NASA’s *The Power of Ten* are binding unless a documented exception exists.

---

## 2. Target Platform

### 2.1 Supported Operating System

* The only supported operating system is **Windows 11**.
* Do not add Linux-specific, macOS-specific, POSIX-specific, or Unix-specific code unless it is
  isolated in clearly excluded experimental branches that are not part of the main project.

### 2.2 Supported Architecture

* Primary architecture: **x86_64**
* Assembly code must target **Intel x86_64** syntax for **NASM**
* 32-bit support is not assumed
* ARM support is not assumed unless the project explicitly adds it later

### 2.3 Environment Assumptions

* Code may assume a Windows 11 environment
* Do not write fake “portable” abstractions that make the code worse unless portability is an
  explicit project goal
* Platform differences within Windows must be documented when relevant

---

## 3. Toolchain and Build Rules

### 3.1 Compiler

* **Clang is mandatory**
* No GCC
* No MSVC C compiler as the primary compiler
* No “supports many compilers” policy unless explicitly required by the project owner

Required baseline:

* `clang -std=c17`

### 3.2 Linker

Allowed linkers:

* `lld-link`
* `link.exe`

Both are acceptable project baselines.

Rule:

* The chosen linker must be documented in the build script
* Do not silently switch linkers between environments without good reason

### 3.3 Assembler

* Assembly must use **NASM**
* Syntax must be **Intel**
* Architecture must be **x86_64**
* MASM is not the project standard
* GAS syntax is not allowed

### 3.4 Build System Policy

Keep the build system simple.

Preferred order:

1. `.bat`
2. `.ps1`
3. `make` only when the project becomes large enough to justify it

Rules:

* **Do not use CMake**
* Do not use overly abstract meta-build systems
* The build must be easy to read and debug
* A new developer should understand how the project builds in a few minutes

### 3.5 When to Use `.bat` or `.ps1`

Use a simple script when the project is:

* small or medium-sized
* made of a few targets
* easy to build in one or a few steps
* not suffering from dependency-management complexity

### 3.6 When to Use `make`

`make` is allowed when the project is large enough that:

* there are many translation units
* there are many generated objects
* incremental rebuilds matter
* dependency tracking matters
* multiple targets or configurations exist

Even then:

* keep the Makefile simple
* do not turn the build into a framework
* avoid clever metaprogramming in Makefiles

---

## 4. Language Standard and Extensions

### 4.1 Standard

* All C code must compile as **C17**
* Code must be written for C, not for compiler-extension dialects disguised as C

### 4.2 Extensions

* Compiler extensions are allowed only when justified
* If a compiler builtin or extension is used repeatedly, wrap it in a local helper API
* Do not scatter compiler-specific behavior throughout the codebase

### 4.3 Freestanding vs Hosted

The project must explicitly document whether it is:

* hosted
* freestanding
* mixed

Rule:

* Build flags must match the project reality
* Do not pretend code is portable between hosted and freestanding environments when it is not

---

## 5. Build Configurations

At minimum, the project should define:

* **debug**
* **release**

Optional:

* **sanitized**
* **analyze**
* **test**

### 5.1 Debug Build Goals

Debug builds optimize for:

* diagnostics
* observability
* symbolic debugging
* stack clarity
* invariant checking

Typical debug flags:

```text
-std=c17
-g3
-O0
-Wall
-Wextra
-Wpedantic
-Werror
-Wshadow
-Wconversion
-Wsign-conversion
-Wdouble-promotion
-Wformat=2
-Wundef
-Wstrict-prototypes
-Wmissing-prototypes
-Wmissing-declarations
-Wimplicit-fallthrough
-Wcast-qual
-Wwrite-strings
-Wvla
-fno-omit-frame-pointer
```

Additional flags may be used depending on the project type.

### 5.2 Release Build Goals

Release builds optimize for:

* correctness
* stable performance
* debuggable optimized code
* predictable behavior

Typical release flags:

```text
-std=c17
-g
-O2
-DNDEBUG
-Wall
-Wextra
-Wpedantic
-Werror
-fno-omit-frame-pointer
```

Rules:

* `-O2` is the default release optimization level
* `-O3` is not the project-wide default
* higher optimization must be justified by measurement

### 5.3 Warning Policy

* Warnings are errors
* The tree must remain warning-clean
* Suppressions must be local and justified
* Do not silence warnings globally to make the build green

Forbidden:

* broad warning disabling
* “we know it’s fine” as a policy
* casting only to silence diagnostics

---

## 6. Formatting and Layout

### 6.1 Canonical Formatter

* `clang-format` is mandatory for C and header files
* The repository must include a `.clang-format`
* Manual formatting exceptions must be rare and justified

### 6.2 Indentation

* Use **2 spaces** per indentation level
* Tabs are prohibited except where a tool requires them
* Makefiles may use tabs where syntax requires it

### 6.3 Line Length

* Soft limit: **100 columns**
* Going over is allowed when forcing a wrap would reduce readability

### 6.4 Braces

Braces are mandatory for all control bodies.

Correct:

```c
if (ready) {
  run();
} else {
  wait();
}
```

Forbidden:

```c
if (ready)
  run();
```

### 6.5 Spacing

Rules:

* one space before `(` in control statements
* no space before `(` in function calls
* one space around binary operators
* unary operators stay attached
* no random alignment spaces
* no trailing whitespace

### 6.6 Empty Lines

Use empty lines to separate:

* logical stages
* declarations from execution when useful
* major branches or phases

Do not add empty lines randomly every two statements.

### 6.7 One Declaration Per Line

Required.

Correct:

```c
int count;
char* name;
```

Forbidden:

```c
int count, total;
char *a, *b;
```

### 6.8 Pointer Style

Pointer alignment is left-bound to the type.

Correct:

```c
int* ptr;
const char* text;
struct Node* next;
```

This style is mandatory for consistency.

---

## 7. File Organization

### 7.1 File Naming

Use:

* `snake_case.c`
* `snake_case.h`
* `module_name.asm`

Avoid:

* spaces
* mixed naming styles
* vague file names like `misc.c`, `stuff.c`, `helpers2.c`

### 7.2 File Size

Guideline:

* keep files focused
* split files when a single file mixes unrelated responsibilities

Review threshold:

* if a file becomes hard to navigate, split it
* if a file grows very large, group by responsibility, not arbitrarily

### 7.3 Header and Source Separation

Headers should contain:

* declarations
* public types
* constants
* macros that must be public
* tiny `static inline` helpers when justified

Source files should contain:

* implementations
* internal helpers
* private constants
* private tables
* internal state

---

## 8. Headers and Inclusion Policy

### 8.1 `#pragma once`

Use:

```c
#pragma once
```

This is the project standard.

Traditional include guards are not the preferred style.

Rationale:

* simpler
* less boilerplate
* less naming noise
* easier to maintain
* supported by Clang and normal Windows toolchains

### 8.2 Include Rules

Rules:

* include what you use
* do not rely on transitive includes
* keep headers self-consistent
* a header should compile when included in a minimal translation unit

### 8.3 Include Order

Recommended order:

1. the file’s own header
2. standard library headers
3. platform headers
4. project public headers
5. project private headers

This catches missing dependencies early.

### 8.4 Forward Declarations

Use forward declarations when they reduce unnecessary coupling.

Do not use them when the full definition is actually needed.

---

## 9. Naming Rules

### 9.1 General Naming

Use:

* functions: `snake_case`
* variables: `snake_case`
* files: `snake_case`
* macros: `SCREAMING_SNAKE_CASE`
* enum constants: `SCREAMING_SNAKE_CASE`
* structs/unions/enums: `PascalCase`

### 9.2 Public Prefixes

Public symbols must use a project or subsystem prefix.

Examples:

* `mem_alloc`
* `str_parse_u64`
* `net_socket_open`
* `gfx_surface_create`

Forbidden in global namespace:

* `init`
* `open`
* `run`
* `lock`
* `start`
* `create`

Too generic. Too collision-prone.

### 9.3 Boolean Names

Boolean variables should read like predicates.

Good:

* `is_ready`
* `has_error`
* `can_write`
* `was_found`

---

## 10. Types and Declarations

### 10.1 Standard Types

Use standard headers where appropriate:

* `<stdint.h>`
* `<stddef.h>`
* `<stdbool.h>`
* `<stdalign.h>`
* `<stdatomic.h>`

### 10.2 Integer Types

Rules:

* use fixed-width integer types when bit width matters
* use `size_t` for sizes and counts
* use `ptrdiff_t` for pointer differences
* use `uintptr_t` and `intptr_t` for pointer-width integer conversions

### 10.3 `typedef` Policy

Allowed for:

* opaque handles
* callback signatures
* repetitive function pointer types
* intentionally abstract public types

Do not typedef primitive integers just to rename them for style.

Bad:

```c
typedef unsigned int uint;
typedef unsigned char byte;
```

### 10.4 `enum` Policy

Use `enum` for symbolic states and finite sets.

Do not rely on enum storage size.

If storage layout matters, store an integer and use enum values symbolically.

### 10.5 Struct Layout

Rules:

* group fields by access pattern first
* consider hot/cold separation in performance-sensitive code
* document layout constraints when ABI or binary layout matters
* use static assertions where structure size or alignment matters

### 10.6 Packed Structures

Allowed only for:

* on-disk formats
* network formats
* hardware descriptors
* firmware tables
* externally defined binary layouts

Packed structs are not general-purpose working structs.

### 10.7 Bitfields

Default policy: **do not use bitfields**

Prefer masks and shifts.

Bitfields are allowed only if there is a very strong reason and the layout is tightly controlled.

### 10.8 Unions

Use unions only when:

* a tagged representation exists
* binary overlay is intentional
* a strict format requires it

Non-trivial unions must have a clear invariant describing which member is active.

---

## 11. Casts, `const`, `volatile`, `inline`, `static`

### 11.1 Cast Policy

Casts must be explicit and justified.

Allowed:

* well-defined narrowing or widening conversions
* pointer/integer conversion via `uintptr_t`
* API boundary conversions
* approved container-style conversions

Forbidden:

* casts to hide type bugs
* removing `const` without a real reason
* casting unrelated pointers to reinterpret layout casually
* cast chains that make ownership or meaning unclear

### 11.2 `const`

Use `const` aggressively for:

* input-only parameters
* read-only tables
* operation tables
* immutable data
* string literals and read-only buffers

### 11.3 `volatile`

Use `volatile` only for:

* MMIO
* special compiler-visible hardware interaction points
* memory locations that are externally observed outside the normal abstract machine

Do not use `volatile` for:

* thread synchronization
* locking
* waiting protocols
* reference counting

### 11.4 `inline`

Prefer `static inline` for tiny typed helpers.

Do not put large logic in headers just because “maybe it gets faster”.

### 11.5 `static`

Anything not meant to be visible outside a translation unit must be `static`.

But remember:

* file-local mutable state is still global state in practice
* hiding a bad design behind `static` does not make it good

---

## 12. Functions and Control Flow

### 12.1 Function Size

Guideline:

* aim for small, single-purpose functions

Recommended maximum:

* about **60 logical lines**

Review threshold:

* around **100 logical lines**

Bigger functions require a clear reason.

### 12.2 Nesting Depth

Recommended maximum nesting depth:

* **3**

Review threshold:

* **4**

Use guard clauses and helpers instead of building arrow-shaped code.

### 12.3 Parameter Count

Recommended maximum:

* **5**

Review threshold:

* **7**

Beyond that, use a parameter struct.

### 12.4 Return Values

Rules:

* every return value must be checked or intentionally ignored with a comment
* boolean returns are for true/false questions only
* detailed failures should use error codes or result objects

### 12.5 `goto`

Allowed for:

* cleanup paths
* ordered unwinding
* one-way exits from complex error handling

Forbidden:

* loop construction with backward `goto`
* spaghetti flow

### 12.6 Recursion

* Recursion is prohibited by default
* Any exception must be explicitly justified and bounded

For most real-world C projects, recursion causes more trouble than value.

### 12.7 Loops

Every loop must have one of these:

* an explicit numeric bound
* a clearly documented external termination condition
* an obvious and reviewable finite traversal

Unbounded loops require clear justification.

### 12.8 Allocation

Allocation must be explicit and reviewable.

Rules:

* do not allocate casually in hot paths
* do not allocate inside tight loops unless necessary
* check allocation failure
* free on all failure paths
* document ownership transfer

---

## 13. Ownership, Lifetime, and Resource Management

Every non-trivial API must make ownership obvious.

Use these meanings consistently:

* **borrowed**: temporary access, no ownership transfer
* **acquired**: caller gains ownership or a reference
* **transferred**: ownership moves to callee
* **consumed**: callee may destroy, store, or release it

Rules:

* ownership must be visible in naming, types, or comments
* no hidden frees
* no hidden refcount expectations
* no “caller probably knows” API contracts

---

## 14. Preprocessor Rules

### 14.1 Minimize Preprocessor Use

The preprocessor is for:

* inclusion
* configuration gates
* compile-time constants where appropriate
* carefully designed utility macros

It is not for building a second language on top of C.

### 14.2 Forbidden Macro Patterns

Forbidden:

* statement-like complex macros
* side-effect macros
* multi-evaluation macros
* control-flow macros that hide behavior
* fake generic programming unless truly justified

Prefer:

* `static inline`
* enums
* constants
* typed helpers

### 14.3 Conditional Compilation

Keep `#if`, `#ifdef`, and `#ifndef` usage minimal.

Rules:

* platform split for Windows 11 only should be rare
* feature switches must be few and meaningful
* nested conditional compilation is a smell

### 14.4 Header Guards

Use `#pragma once`, not classic include-guard boilerplate.

---

## 15. Comments and Documentation

### 15.1 Good Comments

Comments should explain:

* why
* invariant
* ownership
* safety contract
* tricky hardware or binary requirements
* non-obvious design choices

### 15.2 Bad Comments

Do not write comments that merely restate the code.

Bad:

```c
/* Increment i */
i++;
```

### 15.3 Public API Comments

Document public APIs when the contract is not obvious from the type alone.

Include where relevant:

* ownership
* thread-safety
* blocking behavior
* valid states
* failure behavior
* input restrictions

### 15.4 TODO Policy

TODOs must be specific.

Good TODO:

```c
/* TODO(raul): Replace linear scan with sorted lookup after parser v2 lands. */
```

Bad TODO:

```c
/* TODO: improve */
```

---

## 16. Error Handling and Diagnostics

### 16.1 Error Handling Philosophy

Handle errors explicitly.

Rules:

* detect failure early
* unwind completely
* do not continue after corrupted assumptions
* fail loudly in debug builds
* fail predictably in release builds

### 16.2 Assertions

Assertions are for programmer invariants, not external input validation.

Use assertions to catch:

* impossible states
* broken internal contracts
* corrupted assumptions

Do not use assertions to validate:

* file input
* user input
* network input
* device input
* untrusted data

### 16.3 Logging

Logging should be structured by severity and subsystem.

Suggested levels:

* `LOG_FATAL`
* `LOG_ERROR`
* `LOG_WARN`
* `LOG_INFO`
* `LOG_DEBUG`
* `LOG_TRACE`

Rules:

* logs must be cheap when disabled
* high-frequency logging must be carefully controlled
* do not spam logs instead of reasoning about behavior

---

## 17. Concurrency and Shared State

### 17.1 Shared State

Mutable shared state must be minimized.

Rules:

* every mutable global must have a clear owner
* synchronization must be explicit
* lock rules must be documented when not obvious

### 17.2 Atomics

Use C17 atomics where required.

Do not scatter raw memory-order reasoning casually throughout the tree.

If atomics become common, wrap common patterns in local helpers.

### 17.3 Locking

For any non-trivial shared structure, document:

* which lock protects it
* whether readers need the same lock
* whether operations may block
* whether callbacks run under lock

### 17.4 Data Races

Data races are bugs.

There is no “benign race” policy unless that claim is proven and documented very clearly.

---

## 18. Undefined Behavior Avoidance

The project must aggressively avoid undefined behavior.

Forbidden patterns include:

* signed overflow as logic
* out-of-bounds access
* use-after-free
* double free
* invalid shifts
* strict aliasing abuse
* reading uninitialized storage
* invalid lifetime extension
* pointer arithmetic outside valid objects

Rule:

* if code relies on UB “working on this compiler”, the code is wrong

---

## 19. Hardware and Binary Data Access

### 19.1 Binary Layouts

When reading or writing binary layouts:

* be explicit about width
* be explicit about endianness
* validate bounds
* avoid aliasing tricks
* use copies where needed

### 19.2 Endianness

Do not assume serialized data matches host endianness.

Always use explicit conversion helpers for:

* file formats
* network formats
* protocol fields
* hardware-defined structures

### 19.3 MMIO and Special Access

If the project touches MMIO or hardware-like memory:

* use dedicated helpers
* centralize ordering assumptions
* centralize volatile usage
* do not open-code fragile access sequences everywhere

---

## 20. Assembly Rules

### 20.1 Allowed Assembly

Assembly is allowed only when there is a real reason, such as:

* startup/entry code
* context save/restore
* low-level CPU instructions not expressible cleanly in C
* ABI bridges
* special fast paths backed by measurement

### 20.2 Required Assembler Style

Assembly must use:

* **NASM**
* **Intel syntax**
* **x86_64**

### 20.3 Assembly File Rules

Rules:

* keep assembly isolated
* document calling convention assumptions
* document clobbers and preserved registers
* keep symbol names consistent with the C side
* comment tricky instruction sequences

### 20.4 Assembly and C Boundary

The C/assembly boundary must clearly document:

* input registers or stack arguments
* return value convention
* preserved registers
* stack alignment requirements
* shadow space requirements when relevant on Windows x64

---

## 21. Testing, Analysis, and Verification

### 21.1 Definition of Done

Code is not done when it merely compiles.

It is done when:

* it builds cleanly
* warnings are clean
* the code is readable
* error paths are handled
* tests exist where appropriate
* invariants are documented where needed
* the implementation matches the intended contract

### 21.2 Static Analysis

Static analysis is strongly encouraged.

Run analysis especially for:

* memory safety
* nullability
* dead stores
* unchecked conversions
* suspicious control flow

### 21.3 Tests

Tests should focus on:

* boundary conditions
* invalid inputs
* ownership behavior
* parser correctness
* arithmetic edge cases
* state machine correctness
* regression coverage

---

## 22. Rules Inspired by NASA’s The Power of Ten

These are binding unless a documented exception exists.

1. Functions must remain small and single-purpose.
2. Recursion is prohibited by default.
3. Loops must have an explicit bound or a clear termination condition.
4. Every return value must be checked or intentionally ignored with a comment.
5. Pointer use must be minimized and ownership must be explicit.
6. Dynamic allocation must be justified in sensitive or hot code.
7. Assertions protect invariants, not external input.
8. Preprocessor use must be minimized.
9. Concurrency rules must be stated, not implied.
10. Warning-clean builds and analysis are part of normal development, not optional extras.

---

## 23. Forbidden Patterns

The following are prohibited unless a documented low-level exception exists:

* variable length arrays
* hidden fallthrough
* multi-variable declarations on one line
* assignment in conditionals unless exceptionally clear and justified
* side-effect macros
* deep nesting
* recursion
* magic numbers with unclear meaning
* unchecked narrowing conversions
* unchecked size arithmetic
* unchecked allocation failure
* reliance on transitive includes
* fake portability layers that only add complexity
* broad warning suppression
* using the preprocessor as architecture

---

## 24. Review Checklist

Every review should inspect at least:

* correctness of types
* ownership and lifetime
* null handling
* bounds and overflow handling
* cleanup paths
* warning cleanliness
* readability
* unnecessary complexity
* global state impact
* concurrency safety where relevant
* API clarity
* test impact
* binary layout assumptions if present
* Windows-specific ABI correctness if relevant
* C/assembly boundary correctness if relevant

---

## 25. Style Summary

In short:

* Windows 11 only
* Clang only
* C17 only
* `#pragma once` preferred
* NASM Intel x86_64 for assembly
* `lld-link` or `link.exe`
* no CMake
* no GCC
* `make` only when the project is large enough
* otherwise `.bat` or `.ps1`
* 2-space indentation
* braces always
* simple code
* explicit ownership
* minimal macros
* warning-clean builds
* no clever nonsense unless it is truly justified

---

## 26. Non-Normative Practical Advice

Good C code is usually a bit boring.
And that is a compliment

When in doubt:

* choose the version that is easier to review
* choose the version that fails more obviously
* choose the version that hides less
* choose the version that will still make sense in two years

C becomes dangerous when people try to make it “smart”.
For most projects, **simple beats fancy** almost every time.

### 27. Extra information

The unsigned char type must be called BYTE using typedef

```c
typedef unsigned char BYTE;
```

And you must create a .clang-format file only for C and C++, the rest of the languages should follow their own rules like Python with PEP
And for the biggest proyects that are really important you should use clang-cl for Windows with link.exe instead of lld-link.exe

### 28. Deprecation

Many functions like scanf() are deprecated, you must use the recommended ones like in the case of scanf(), replace it with scanf_s()

### 29. C++

For C++ the styleguide is the same but the standard is C++20
