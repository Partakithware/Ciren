# Ciren

A statically typed, compiled systems language with C interop, generics, interfaces, and inline assembly. Ciren compiles to native binaries via LLVM, with an optional C source backend.

## Side Note:
I am only a coding hobbyist, so there are likely flaws and various features that could be improved. I invite anyone that can and may know better to pitch in. It would be very appreciated.
There are many things that need adjustments and minor fixes or simply just updated functionality. I decided to post it now as it gets further out of the range of what I feel I can accomplish alone.
As always my documentation is not where it should be, so I apologize for that. I would just like to see Ciren swim along and manage to become something more! I would not describe it's style as 'new' but more like an adapted C style of sorts (in my opinion).  

---

## Table of Contents

- [Installation & Build](#installation--build)
- [Quick Start](#quick-start)
- [Compiler CLI](#compiler-cli)
- [Language Reference](#language-reference)
  - [Types](#types)
  - [Variables & Constants](#variables--constants)
  - [Functions](#functions)
  - [Control Flow](#control-flow)
  - [Structs](#structs)
  - [Enums](#enums)
  - [Interfaces & impl](#interfaces--impl)
  - [Generics](#generics)
  - [Pointers & Memory](#pointers--memory)
  - [Inline Assembly](#inline-assembly)
  - [Modules](#modules)
  - [Expressions](#expressions)
- [Access Modifiers](#access-modifiers)
- [C Interop](#c-interop)
- [Debug Flags](#debug-flags)

---

## Installation & Build

**Dependencies:** LLVM (for the primary backend), a C compiler (`cc`) for linking.

```sh
make
```

This produces the `cirenc` binary.

---

## Quick Start

```ciren
using stdio;

public main() {
    printf(c"Hello, world!\n");
    return 0;
}
```

```sh
cirenc hello.ci          # compiles to ./hello
./hello
```

---

## Compiler CLI

```
usage: cirenc [options] <file.ci>

options:
  -o <file>         output file (default: input stem)
  --emit-c          emit C source instead of compiling
  --emit-ir         emit LLVM IR (.ll)
  --emit-obj        emit object file (.o)
  -O0 -O1 -O2 -O3  optimization level (default: O0)
  --dump-tokens     print token stream and exit
  --dump-ast        print AST and exit
  --verbose         print linker command
  -l<lib>           pass extra linker flag (e.g. -lSDL2)
  --help            show this message
```

**Examples:**

```sh
cirenc hello.ci                  # → ./hello
cirenc hello.ci -o greet         # → ./greet
cirenc hello.ci --emit-ir        # → hello.ll
cirenc hello.ci --emit-obj       # → hello.o
cirenc hello.ci -O2              # optimized binary
cirenc hello.ci --emit-c         # → hello.c
```

---

## Language Reference

### Types

#### Primitives

| Type   | Description                    |
|--------|--------------------------------|
| `int`  | Platform-width signed integer  |
| `uint` | Platform-width unsigned integer|
| `i8`   | 8-bit signed                   |
| `u8`   | 8-bit unsigned                 |
| `i16`  | 16-bit signed                  |
| `u16`  | 16-bit unsigned                |
| `i64`  | 64-bit signed                  |
| `u64`  | 64-bit unsigned                |
| `f32`  | 32-bit float                   |
| `f64`  | 64-bit float                   |
| `bool` | Boolean (`true` / `false`)     |
| `char` | Single character               |
| `str`  | Ciren string                   |
| `cstr` | C-compatible null-terminated string |
| `void` | No value                       |
| `any`  | Untyped / escape hatch         |

#### Composite Types

```ciren
*T            // raw pointer to T
?*T           // nullable pointer to T
[T; N]        // fixed-size array of N elements
[]T           // slice
(A, B) -> R   // function pointer
T<A, B>       // generic instantiation
```

---

### Variables & Constants

```ciren
let x = 10;              // inferred type
let y: f64 = 3.14;       // explicit type
const PI: f64 = 3.14159; // compile-time constant
```

Multiple assignment from a multi-return call:

```ciren
let a, b = some_func();
```

---

### Functions

```ciren
public fn add(a: int, b: int) -> int {
    return a + b;
}
```

Inferred return type:

```ciren
public fn greet(name: str) {
    printf(c"Hello, %s\n", name);
}
```

Default parameter values:

```ciren
fn connect(host: str, port: int = 8080) { ... }
```

Variadic parameters:

```ciren
fn log(msg: str, args: ...any) { ... }
```

Lambdas:

```ciren
let square = (x: int) => x * x;

let add = (a: int, b: int) -> int {
    return a + b;
};
```

Function pointers:

```ciren
let f: (int, int) -> int = add;
```

Attributes:

```ciren
[inline]
fn fast_abs(x: int) -> int { ... }

[extern("C")]
fn my_c_export() { ... }
```

---

### Control Flow

#### if / else

```ciren
if x > 0 {
    printf(c"positive\n");
} else if x < 0 {
    printf(c"negative\n");
} else {
    printf(c"zero\n");
}
```

`if` as an expression:

```ciren
let label = if x > 0 { "pos" } else { "neg" };
```

#### while

```ciren
while x > 0 {
    x -= 1;
}
```

#### for — range

```ciren
for i in 0..10 { ... }    // exclusive
for i in 0..=10 { ... }   // inclusive
```

#### for — iterable

```ciren
for item in array { ... }
```

#### for — index + value

```ciren
for i, item in array { ... }
```

#### for — C-style

```ciren
for (let i = 0; i < 10; i++) { ... }
```

#### loop

```ciren
loop {
    if done { break; }
}
```

Labeled loops:

```ciren
outer: loop {
    loop {
        break outer;
    }
}
```

#### match

```ciren
match x {
    0       => printf(c"zero\n"),
    1..=9   => printf(c"single digit\n"),
    n if n < 0 => printf(c"negative\n"),
    _       => printf(c"other\n"),
}
```

Enum pattern matching:

```ciren
match shape {
    Shape::Circle(r)      => printf(c"circle r=%f\n", r),
    Shape::Rect(w, h)     => printf(c"rect %fx%f\n", w, h),
}
```

#### defer

Executes when the enclosing scope exits, in reverse order of declaration.

```ciren
let f = open_file("data.txt");
defer close_file(f);
// f is guaranteed to be closed on all exit paths
```

#### panic

```ciren
panic("index out of bounds");
```

#### Error propagation

```ciren
let val = might_fail()?;   // propagates error on failure
```

---

### Structs

```ciren
public struct Vec2 {
    x: f64,
    y: f64,
}

// Instantiation
let v = Vec2 { x: 1.0, y: 2.0 };

// Inferred type literal
let v: Vec2 = .{ x: 1.0, y: 2.0 };
```

Struct methods:

```ciren
public struct Counter {
    value: int,

    fn increment(self: Counter) -> Counter {
        return Counter { value: self.value + 1 };
    }
}
```

Field defaults:

```ciren
public struct Config {
    port: int = 8080,
    debug: bool = false,
}
```

Embedding:

```ciren
public struct ColoredPoint {
    embed Point,
    color: int,
}
```

Generic structs:

```ciren
public struct Pair<T> {
    first: T,
    second: T,
}
```

---

### Enums

Simple value enum:

```ciren
public enum Direction {
    North,
    South,
    East,
    West,
}
```

Explicit discriminant values:

```ciren
public enum Status {
    Ok    = 0,
    Error = 1,
}
```

Tagged union variants:

```ciren
public enum Shape {
    Circle(radius: f64),
    Rect(width: f64, height: f64),
    Point,
}

let s = Shape::Circle(5.0);
```

---

### Interfaces & impl

```ciren
interface Drawable {
    fn area(self: Self) -> f64;
    fn name(self: Self) -> str;
}

impl Drawable for Circle {
    fn area(self: Circle) -> f64 {
        return 3.14159265 * self.radius * self.radius;
    }
    fn name(self: Circle) -> str {
        return "circle";
    }
}
```

Interface inheritance:

```ciren
interface Serializable : Printable, Hashable {
    fn serialize(self: Self) -> str;
}
```

---

### Generics

Generic functions:

```ciren
private fn identity<T>(val: T) -> T {
    return val;
}

let x = identity(42);
let y = identity(3.14);
```

Constrained generics:

```ciren
private fn describe<T: Printable>(val: T) {
    printf(c"item: %s\n", val.to_str());
}
```

Explicit type argument:

```ciren
let v = make<int>(10);
```

---

### Pointers & Memory

```ciren
let x: int = 42;
let p: *int = &x;       // address-of
let val = *p;           // dereference

// Heap allocation
let ptr: *MyStruct = new MyStruct { field: 1 };
delete ptr;

// Nullable pointer
let maybe: ?*int = null;
```

Pointer arithmetic is supported via standard operators.

`sizeof` and `alignof`:

```ciren
let s = sizeof(MyStruct);
let a = alignof(f64);
```

---

### Inline Assembly

Assembly functions with explicit register bindings:

```ciren
public asm doSysCall(num: int in rax, arg1: int in rdi) -> rax
    clobbers rcx, r11
{
    syscall
}
```

Inline asm blocks inside regular functions:

```ciren
fn flush_cache() {
    asm {
        mfence
        lfence
    }
}
```

---

### Modules

#### Declaring a module

```ciren
module net.http;
```

#### Importing

```ciren
using stdio;
using net.http;
using net.http as http;
```

#### Re-exporting

```ciren
public using net.http;
```

Module search paths: `.` and `std/`. The compiler resolves `using foo` to `./foo.ci` or `std/foo.ci` automatically.

**Auto-linked C libraries** (linked automatically when the corresponding `using` is present):

| `using` name | Linked library   |
|--------------|------------------|
| `gtk`        | `-lgtk-3`        |
| `sdl` / `sdl2` | `-lSDL2`       |
| `gl`         | `-lGL`           |
| `glfw`       | `-lglfw`         |
| `ncurses`    | `-lncurses`      |
| `pthread`    | `-lpthread`      |
| `ssl`        | `-lssl`          |
| `crypto`     | `-lcrypto`       |
| `z`          | `-lz`            |
| `cairo`      | `-lcairo`        |

Additional libraries can always be passed directly: `cirenc file.ci -lmylib`.

---

### Expressions

#### Literals

```ciren
42          // int
3.14        // float
"hello"     // str
c"hello"    // cstr (C string literal)
'A'         // char
true false  // bool
null        // null pointer
[1, 2, 3]  // array literal
```

#### Operators

```
Arithmetic:    +  -  *  /  %
Bitwise:       &  |  ^  ~  <<  >>
Logical:       &&  ||  !
Comparison:    ==  !=  <  >  <=  >=
Assignment:    =  +=  -=  *=  /=
Increment:     ++  --
Range:         ..  ..=
Cast:          expr as Type
Propagate:     expr?
```

#### Slicing

```ciren
let s = arr[2..5];    // exclusive slice
let s = arr[2..=5];   // inclusive slice
```

#### Casting

```ciren
let n = x as int;
let p = buf as *u8;
```

---

## Access Modifiers

All top-level declarations take an access modifier. The default is `private`.

| Modifier   | Visibility                        |
|------------|-----------------------------------|
| `public`   | Exported; visible to other modules|
| `private`  | Current file only                 |
| `internal` | Current module only               |

---

## C Interop

Any C symbol is callable as long as it is declared or pulled in through a `using` statement that maps to a known C library. C strings are expressed with the `c""` literal prefix and have type `cstr`. Casting between Ciren pointer types and C pointer types is done with `as`.

```ciren
using stdio;

public main() {
    let msg: cstr = c"Hello from C\n";
    printf(msg);
    return 0;
}
```

Raw pointers (`*int` as an opaque handle) are the standard pattern for wrapping opaque C types.

---

## Debug Flags

| Flag            | Effect                                  |
|-----------------|-----------------------------------------|
| `--dump-tokens` | Print the full token stream and exit    |
| `--dump-ast`    | Print the full AST and exit             |
| `--verbose`     | Print the linker command before running |
