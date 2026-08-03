# Language, Draft v0.3.1

This is the third major revision of the language prior to an offical 1.0. So
far, it shows the most promise.

## Getting started
A basic "Hello, world!" program.

```
import "std/io"

pub fn Main() Void {
  io.Println("Hello, world!")
}
```

# Specification

## Identifiers

Idendifiers must start with either an upper or lowercase letter ("a" to "z")
or an underscope. They can contain any number of alphanumeric characters,
including underscores. A tailing question mark ("?") can be appended.

Identifiers that start with, or consist only of, an underscore are
"ignored" variables. They can not be accessed once they are assigned a value
and the compiler will not flag them as an unused variable. An ignored name
binds nothing, so it never collides with another name: any number of them may
appear in the same scope or in nested ones.

```
_ := setup()
for _, v : arr {} // no redeclaration, no shadowing
```

A constant that is flagged as ignored, while technically valid, is unusable
and the compiler will not generate code for it.

### Convensions

Identifier convensions are a work in progress. The current convensions for
identifers are as follows but are subject to change:

Public identifiers should be written in PascalCase or Capitalized_Snake_Case.
Private identifiers should be written in camelCase or snake_case.  Only 
boolean identifiers should have the "?" suffix.

_Note: These preferences are applied by the formatter but are not strictly
enforced by the language._

## Mutability (Memory Model)

This language does not use a garbage collector. Instead, it uses value
semantics, optimized reference counting, copy-on-write, and escape analysis
to manage allocations.

All top level constants are immutable. Local variables are mutable insofar as
they do not escape their current scope. Values that escape their scope are
copied. Large/complex types are optimized to be copy-on-write for performance
while small objects are just copied.

This applies to co-routines to prevent memory polution and resource
contention. Each coroutine gets its own copy of memory it could conceivably
touch when it is spawned.

### Shadowing and scope

This language uses lexical scoping. An inner scope can see outer scopes but not
the other way around. In the case of a closure, if a scope captures a variable
from outside its scope, it copies that variable into its own internal state. If
the closure itself escapes its current scope, the closure itself, and its
captured state, are moved onto the heap.

It is an error to shadow a variable from an outer scope or to redeclare an 
identifier from the current scope.

### Cleanup

When values reach zero references, they are automatically cleaned up with
implicity destructors. This means that all i/o handles are automatically
closed. This applies to all resources like Files, Sockets, and Network
connections. To keep them open, they must remain referenced and in-scope.

Users should not need to call `Close()`.

## Constants

Constants are defined with top-level statements. With the exception of `Main`, 
constants are not required to use CamelCase but that is the preferred 
convension.

Constants are immutable. They carry values and named imports. A value constant
can be any simple expression that can be resolved at compile time.

```
const Pi = 3.14159
const MaxSize = 2 * 1024 * 1024 // 2MB
const Math = import "std/math"
```

A constant is immutable: its storage is never written after the program
starts. A method that mutates in place — `Pop` on an array — is rejected
on a constant. Value-returning methods such as `Append`, `Insert`, and
`Set` do not write through the receiver; they return a new array and leave
the constant unchanged, so they are allowed:

```
const Primes = [2, 3, 5]

pub fn Main() void {
  bigger := Primes.Append(7)  // Primes is still [2, 3, 5]
}
```

## Compile-time values

A constant's value is computed at compile time and baked into the binary.
There is no initialisation phase and no code runs before `Main`. This
covers scalars, strings, struct literals, and arrays whose elements are
themselves constant:

```
pub const Pi float = 3.14159
pub const Primes array{int} = [2, 3, 5, 7]
```

Constant arrays live in read-only data with a sentinel reference count, so
reading one allocates nothing. The first method that needs a mutable copy
(`Append`, `Insert`, `Set`) clones the array once; the original stays in
read-only data.

Maps cannot be constants — building one requires hashing and allocation at
runtime — so a map constant is a compile-time error. Expose a function that
returns a freshly built map instead.

A constant initialiser may read another constant declared earlier — earlier
in the same file, earlier within the package's file order, or in any
imported package. Reading a constant declared later in the same package is
a compile-time error.

Saga does not provide a user-defined package initialiser (an `init`
function or block that runs implicitly on import). The omission is
deliberate. Implicit initialisation is hard to reason about: imports
become statements that quietly run code, startup ordering depends on a
graph the reader cannot see at the call site, and side effects detach
from the call sites that invoke them. Saga's preference is for setup to
be explicit. If a package needs to validate configuration, open a
connection, or assemble a registry, expose a function and let `Main` —
or the caller that needs it — invoke it at a visible point in the
program.

```
// Don't reach for a hidden init. Expose what setup needs to happen
// and let the caller decide when.
pub fn Connect(url string) Connection { ... }
```

## Visibility

Everything is private by default. To make a constant visible outside a file
scope, it must be marked as `pub`lic.

## Expression-statements

Most grammar that is normally a statement in other languages, can also be used
as an expression. `for`, `fn`, `if`, `import`, and `switch` can be  used
either as a statement or an expression.

`break`, `next`, and `return` are clauses or expression terminators.

Expressions are terminated either by closing a block (one-liner) or a newline.

## Import

```
import "std/io"
import "./local/pkg"
const Math = import "mega/long/mathematics"
```

The last segment of an import is its name. "std/io" gets bound to "io". An 
import can be bound to a different constant using a `const`. Importing the 
same package more than once is an error, even if bound to an alternate name.

Only public members of a package can be accessed. Accessing a member of a
package is done by using a selector: `io.Println()`.

## Packages/Modules

A directory forms the scope and name of a package. Sub-directories are their
own package and must be imported separately. All files in a directory are part 
of that package. The "std/io" directory could contain, for example: "print.rg",
"file.rg", and "socket.rg". Only constants marked as `pub`lic are visible 
outside the package but any file within the package has unrestricted access to
private Constants.

## Methods

Methods are defined with the `fn` keyword. The final expression in a method is
returned from the method and must match the return type; otherwise, it's an 
error. A method can be short-circuited, an "early return", with the `return`
keyword. The right hand expression of a return must match the return type of
the method. If a method has a return type of `Void`, then the `return` must 
omit a value.

The final (tail) expression of a method is it's return value. The return value
must match the return type of the function.

```
// A private function
fn Add(a, b Int) Int { a + b }

// A public function
pub fn Greeting() String { "Hello!" }

// A multi-line function
pub fn Main() Void {
  greeting := "Hello, world!"
  io.Println(greeting)
}
```

Parameters are separated by commas. Multiple parameters of the same type can be 
concatenated with commas, followed by a Type, then more parameters. 

```
fn MultiParam(a, b Int, c String) Void {}
```

### Method uniqueness

A each method on a type must be unique. Re-defining, or rebinding, a method on
a type is an error. This uniqueness rule is distinct from **shadowing**. A method
on a type may shadow a method inherited from an embedded struct or inherited from 
a nomiminal type's underlying type. This is because the shadowing method lives on
a different type, thereby maintaining the uniqueness.

Compiler-provided methods count as the type's own definitions. For instance, an 
enum's `String`, `Int`, and `From` are synthesized for the enum itself, so 
redefining one of these methods is a redefinition error. A nominal alias's 
`String` is inherited from the underlying type and so may be shadowed.

### Return statement

Methods support a `return` statement to exit at any time. A return statement
exits the outer-most block. It must contain the same number of expressions to
match the return type.

```
pub fn Greeting(evening? Bool) String {
  if evening? {
    return "Goodnight"
  }
  
  "Good day" // could also use `return "Good day"`
}
```

By convension, prefer a "naked" return value in the tail position of a method.

### Variadics

Methods can accept an arbitrary number of arguments by declaring a variadic
parameter. It must be the final type in the signature. By prepending the type
name with `...`, the parameter can accept any number of arguments. The
variable itself is just an array.

```
fn Sum(args ...Int) Int { // args is of type Int[]
  for i : args |acc| { acc += i }
}
```

Arrays of the same type do not need to be spread into a variadic. There is
literally no work for the compiler to do if you pass an array in.

```
// multiple arguments
sum1 := Sum(1, 2, 3, 4, 5)

// or pass in an array
sum2 := Sum([1, 2, 3, 4, 5]) // Int[] matches the argument's actual type

// but you can't mix and match
sum3 := Sum(1, 2, [3, 4]) // The types are Int and Int[], no match
```

## External declarations

External (non-Saga) functions are declared with the `extern` keyword as a
prefix to a bodiless `fn` declaration. The Saga code that wraps them lives in
the same file.

```
extern fn saga_int_to_string(i Int) String
extern fn saga_int_hash(i Int) Int

fn ToString(i Int) String { saga_int_to_string(i) }
fn Hash(i Int) Int       { saga_int_hash(i) }
```

The type checker validates calls against the extern declaration the same way
it validates any other call.

### Form

An `extern fn` declaration is a `fn` declaration with no body. The signature
must be complete — parameter types and the return type are required and are
used by the type checker exactly like Saga function signatures. Receivers
and variadic parameters are not permitted. A generic type parameter is
permitted so that a polymorphic C function (which typically takes elements
via `void*`) can be wrapped in a type-safe Saga signature; the single
link-time symbol stays the same regardless of `T`.

```
extern fn saga_string_size(s String) Int           // ok
extern fn |T| saga_array_push(a T[], elem T) Void  // ok — generic wrapper
extern fn saga_oops(x) Void                        // error: missing type
extern fn saga_oops(x Int) Void { return }         // error: extern has no body
```

### Visibility

Extern declarations are always package-private. `pub extern fn` and `extern
pub fn` are compile errors. The external symbol is an implementation detail
of the package; the Saga wrapper is the public surface.

This keeps the modifier count on a function declaration at exactly one, and
`extern` does real work — it tells the parser to skip body parsing and the
linker that this symbol comes from outside.

### OS-specific declarations

Extern declarations compose with the existing per-OS file pattern
(`_<os>.sg`). A package that wraps platform-divergent functions declares
them in the per-OS source file alongside the Saga code that uses them.

```
// system_linux.sg
extern fn getpid() Int
pub fn ProcessId() Int { getpid() }

// system_windows.sg
extern fn GetCurrentProcessId() Int
pub fn ProcessId() Int { GetCurrentProcessId() }
```

### Calling convention

Saga's codegen emits every function using the platform's standard C ABI
(System V AMD64 on Linux/macOS, MS x64 on Windows, AAPCS on ARM).
Saga-to-Saga and Saga-to-extern calls are byte-identical at the machine
level; there is no ABI translation at the boundary, so `extern` is purely a
declaration concern.

Extern calls are also exempt from the call-site array deep-copy that
normally enforces Saga's value semantics: an array passed to an `extern fn`
is forwarded by reference so the C side can mutate the backing buffer in
place (which is how the runtime's polymorphic `saga_array_push` /
`saga_array_set` / `saga_array_pop` operate). A pure-Saga function would
receive a defensive clone; an extern callee does not. This is the deliberate
trust contract at the C boundary — author extern wrappers accordingly.

## Types

There are eight standard intrinsic types. Six have identifiers: Bool, Byte,
Float, Int, String, and Void. Two are identfied by their shapes:
`Type[]` (array) and `{Type: Type}` (map). The two
numeric types (Int and  Float) are aliases of the word size variant (Int32 and
Float32 on a 32bit  system, and Int64 and Float64 on a 64bit system, and so
on). Byte is an alias of UInt8.

The array type form is deliberately a *suffix* — `Int[]`, not `[Int]`. The
suffix mirrors the indexing operation (`arr[i]`), so the type form and the
access form share a shape. The map type, in contrast, mirrors the map
*literal* form (`{"a": 1}`), so its declaration and construction also share a
shape. Each container's type expression is shaped like the syntactic context
where the container is used.

Arrays and maps are shapes, so the names `array` and `map`
are not reserved words. Feel free to use them in your code.

The numeric types also have the full compliment of types: Int8, Int16, Int32,
Int64, UInt8, UInt16, UInt32, UInt64, Float32, Float64.

Types can not be cast to a different size or type. They must be converted, 
which allocates a new variable.

```
i Int     // declare a local value as an integer, equivalent to the next line
i Int = 0 // standard assignment
i := 0    // inferred type

// type type of a value is infered from the right-hand expression
f := 3.14 // system dependant
f32 := f.Float32() // converts to a 32 bit float
f64 := f.Float64() // converts to a 64 bit float
```

These conversions are a one-way trip, meaning they're lossy. Lowering the
precision of a type is a non-reversible action.

### Fat Types

Arrays, maps, and strings are all fat types. Each is backed by struct
that contains a reference to backing data and metadata, like size.

```
arr := [1, 2, 3] // type is inteffered by the first value: Int[]
arr.Size() // => 3
```

To convert a type like `Byte[]` to a String, a conversion utility method
must be used, since String supports UTF-8.

Complex types can be self-referential. Declaration order does not matter — a
field may name a type declared further down the file. See
[Self-referential structs](#self-referential-structs).

### Methods on Intrinsic Types

Intrinsic types (Int, Float, Bool, String, arrays, and maps) have methods like
`String()`, `Equals()`, `Compare()`, and type-specific operations. These methods
are defined in Saga source files in the standard library's type packages
(`std/int`, `std/float`, `std/bool`, `std/string`, `std/array`, `std/map`).

The compiler automatically loads these packages — no import is needed. See
[stdlib.md](stdlib.md) for the full list of available methods and intrinsics.

User code cannot define new methods on intrinsic types; only stdlib packages
may do so using privileged `intrinsic_*` operations.

### Function types

Function types are just signatures. Name one with a structural `type` alias.

```
type CallbackFunc = fn(int) int

struct MyStruct {
  action CallbackFunc
}
```

A type union holds one of several **concrete** types, tagged so you can recover
which one it is with `is` or `switch`. Its size is a small tag plus the largest
alternative's payload.

Rules:

- **Concrete types only.** An interface alternative is an error: `string | Reader`
  does not compile. A union answers "which *type* is it"; an interface answers
  "what *behaviour* does it have" — different axes. To accept any type with a
  behaviour, take the interface itself and narrow with `is`; to require several
  behaviours at once, embed interfaces (`interface RW { Reader, Writer }`).
- **Unique set.** A repeated type is an error: `int | int`. A union of unions
  flattens into the combined set — `float | (string | int)` becomes
  `float | string | int` — and a duplicate that flattening exposes is likewise an
  error. A **nominal** alias (`type ID int`) is a distinct type, so it is *not*
  flattened into its underlying.
- **Zero value is the leftmost alternative.** `string | int` zeroes to `""`,
  `int | string` to `0`; an unset union reads as its leftmost type.

### Optional values (`T | void`)

`void` is the absence of a value, so nothing can hold one. It is legal in
exactly two positions: as a function's return type, and as a union alternative.
A variable, parameter, constant, struct field, or collection element typed
`void` — directly or through an `array{void}` / `map{K: void}` — is an error.

A `T | void` union is an optional: the value, or its absence. Its main use is
over-the-wire data (a JSON `null`); in Saga-only code an error union is usually
preferable, since it carries context. Narrow with `is` (`or` does not apply —
`void` is not an error).

```
fn lookup(key string) int | void {
  // ... return an int, or:
  return null
}

x := lookup("a")
if x is int {
  // present
} else {
  // x is void — absent
}
```

### Error unions

Errors are **nominal** types (`error Name { ... }`), not an interface, so they
compose in unions like any other concrete type: `int | error`. Base `error`
accepts any error; a concrete error type accepts only itself. Comparing errors
uses `is` (same error type) and `==` (same type and fields); the message is a
plain `.message` field.

The `or` clause resolves the error before the value is used: it strips the error
alternative(s) and yields the remaining type — a value, or a smaller union.

```
value int | error = 0
i := value or { 1 } // i is int

many int | string | error
narrowed := many or {} // strips error, int | string remains

// capturing the error with the pipe syntax; `err` is scoped to the block.
json := http.Get("example.com/api/data") or |err| {
  log.Error(err.message)
  "{}" // a valid (empty) JSON string
}
```

The pipe variable is typed as the union's error alternative(s), so a concrete
error union exposes that error's own fields:

```
data := parse(raw) or |err| {
  switch err {
    case NetworkError: log.Error(err)
    case ParseError:   log.Warn(err)
    else {}
  }
}
```

**Ordering convention.** Write a union as `T... | void | error` — the value
types first, then an optional `void`, then the error. This is only a convention:
errors are nominal and carry no positional requirement, so any order is legal.

### Generics

A generic parameterises a declaration over a type. Structs and functions take
type parameters; they are named in `<>` after the declaration's name and used
like any other type within it.

```
struct Box<T> {
  value T
}

fn Identity<T>(x T) T { x }
```

Generics are monomorphic. The compiler emits one copy per concrete type
argument, so every argument must be known at compile time.

A type argument is inferred from the values at the use site and is never
written there — there is no `Box<int>{...}` form:

```
n := Box{value: 21}      // Box<int>
s := Box{value: "saga"}  // Box<string>
```

Used with interfaces and union types, generics let one declaration serve many
concrete types without giving up static checking.

#### Methods on a generic struct

A method on a generic struct is declared at top level with a receiver, like
any other method. The receiver repeats the struct's type parameters, which the
signature and body may then use:

```
struct Box<T> {
  value T
}

pub fn (b Box<T>) Get() T { b.value }
pub fn (b Box<T>) Fallback(other T) T { b.value }

b := Box{value: 21}
b.Get()   // 21
```

The call site binds the type parameters from the receiver's type arguments,
and each binding gets its own specialisation. Two instantiations in one
program are separate functions, not one shared symbol:

```
n := Box{value: 21}
s := Box{value: "saga"}
n.Get()   // 21
s.Get()   // saga
```

A struct with several type parameters binds them positionally:

```
struct Pair<A, B> {
  first  A
  second B
}

pub fn (p Pair<A, B>) First() A { p.first }
pub fn (p Pair<A, B>) Second() B { p.second }

p := Pair{first: 1, second: "two"}
```

Every type parameter a method uses must come from its receiver — that is the
only place a call site has anything to bind one from. A method that declares
its own is rejected:

```
pub fn (b Box<T>) Map<U>(f fn(T) U) U { f(b.value) }  // error: U is unbound
```

A signature may name a generic struct, including the receiver's own type. The
receiver's arguments reach it, so `Same` returns `Box<int>` when called on one:

```
pub fn (b Box<T>) Same() Box<T> { b }
```

A free function binds its parameters from the arguments, so a generic struct
works in either position:

```
fn Wrap<T>(x T) Box<T> { Box{value: x} }
fn Unwrap<T>(b Box<T>) T { b.value }

Wrap(5)          // Box<int>
Unwrap(Wrap(5))  // 5
```

### Bounded generics

A type parameter can be constrained by naming the constraint immediately after
it. The constraint follows by adjacency, matching the patterns used elsewhere
in the language (`enum Color string`, receiver declarations):

```
fn Add<T numeric>(a T, b T) T { a + b }

Add(1, 2)       // T = int
Add(1.0, 2.0)   // T = float
Add("a", "b")   // compile error: type string does not satisfy constraint numeric
```

The constraint slot is optional — bare `<T>` still means "any type":

```
fn Identity<T>(x T) T { x }
```

Multiple type parameters each carry their own optional constraint:

```
fn Pick<T integer, U numeric>(a T, b U) U { b }
```

Three named constraints are built into the compiler. They are compiler-only
identifiers and cannot appear as the type of a variable, parameter, or return
value — only in a constraint slot.

| Constraint | Members |
|---|---|
| `integer` | `int`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `byte` |
| `float`   | `float`, `float32`, `float64` |
| `numeric` | All members of `integer` and `float` |

Each constraint implicitly permits the operators that are valid across every
member of its set:

- `integer`: `+`, `-`, `*`, `/`, `%`, comparison (`<`, `<=`, `>`, `>=`,
  `==`, `!=`), bitwise (`&`, `|`, `^`, `<<`, `>>`)
- `float`: `+`, `-`, `*`, `/`, comparison
- `numeric`: `+`, `-`, `*`, `/`, comparison (no `%` or bitwise — not
  valid on floats)

Inside a function with a constrained generic, the listed operators are usable
on the constrained type without further declaration. The compiler validates at
instantiation that the actual type belongs to the constraint set; if not, the
call fails with an error naming both the constraint and the offending type.

An interface is also nameable as a constraint, which is how a generic requires
behaviour rather than a built-in type set. The bound's methods are callable on
the type parameter and dispatch to whichever concrete type it was instantiated
with:

```
interface Named {
  Name() string
}

struct Cat {}
pub fn (c Cat) Name() string { "cat" }

fn Describe<T Named>(x T) string { x.Name() }

Describe(Cat{})  // cat
```

## Type Aliases

The `type` keyword is used to define a new type. Adjacency creates a 
**nominal** type; assignment (`=`) creates a **structural** alias.

A **nominal** type (`type ID T`) is a new, distinct type. It is not
interchangeable with its underlying type but it inherits the underlying's
methods and operators, and you can add or shadow methods of your own. Like
structs, methods can only be bound within the same file scope (no 
monkey-patching across files). Conversion methods can be used to extract
the original, underlying type.

```
type UserID int            // a new, distinct type
i := id.Int()              // convert/extract the underlying integer
io.Println(id.String())    // inherits int's String()

pub fn (u UserID) Validate() bool { ... }  // add behaviour
```

A **structural** alias (`type ID = T`) is a transparent second name for the
same type. It is fully interchangeable with its underlying type in both
directions, and it cannot carry methods of its own. Function types are named
this way.

```
type Identifier = int | string   // mirror: same type, another name
type Callback   = fn(int) int    // define function types
```

## Type Literals

All types have zero values. If a type is defined without specifying a value, it
is assigned a zero value by the compiler.

| Type | Zero Value | Values |
|---|---|---|
| bool | false | true, false |
| byte | 0 | 42, 255 |
| float | 0.0 | 3.14, 0.4e-10 |
| int | 0 | 42, 0b1010, 0o775, 0x1f |
| string | "" | "single-line", """multi-line""", "{expr}" |
| void | | |
| array{T} | [] | [1, 2, 3] |
| map{K:V} | {} | {"key": 42} |

Multiline strings also support interpolation. The newline directly after the
opening `"""` is layout rather than content and is dropped, so a multiline
string may start on its own line.

In the case where a type might be ambiguous, either it must be made explicit
or it will be a type error.

```
arr1 := []              // invalid, no inferrable type
arr2 array{int} = []    // valid, type is known
arr3 array{int}         // the empty value is already the zero value; `= []` is redundant
arr4 := [1]             // type can be inferred
```

### Map Literal

A map literal is a brace-delimited list of `key: value` pairs. Its type is
inferred from the entries; when there are none, annotate the binding so the
key/value types are known (a bare `{}`, like `[]`, has no inferrable type).

```
// Type inferred from the entries — nested maps infer transitively
registry := {
  "production": {"port": 80, "timeout": 30},
  "staging":    {"port": 8080, "timeout": 60}
}

// Empty map — the annotation supplies the key/value types
type EnvironmentMap = map{string: map{string: int}}
staging EnvironmentMap = {}
```

## Array and Map Access

Accessing an entry in an array or map uses the same syntax: `[expr]`. With 
arrays, the expression must be a numeric index. With maps, the expression 
must be the key value you with to extract. Trying to access an invalid key
(out of bounds) results in a `Missing` error. `Missing` is a type that
implements the `Error` interface. By using the `or` clause, you can handle
these key errors effortlessly.

```
arr := [1, 2, 3]
value := arr[99] // value is the type Int | Error
i := value or { 42 } // to use the value, you need to handle the Missing error

// you can also use the short-hand to handle the out-of-bound immediate and
// let the compiler set the zero value for you
i = arr[99] or {} // i == 0

map {String: String} = {"name": "jane"}
name := map["name"] or {} // name == "jane" but would default to an empty string if not found
email := map["email"] or { "unknown" } // defaults to "unknown"
```

### `arr[i]` vs `arr.At(i)`

**Default to `arr[i]`.** It is the canonical, recommended way to read an
element of an array or map. It returns `T | Error` and the compiler
forces you to resolve the error with `or`, which keeps out-of-bounds
bugs out of the language by construction. ~90% of indexed accesses in
real Saga code should use this form.

```
arr := [10, 20, 30]

n := arr[idx] or { 0 }                  // resolve to a default
v := arr[idx] or |err| { return err }   // propagate the error
i := arr[99] or {}                      // shorthand: zero value of T
```

`arr.At(i)` is an escape hatch. It returns `T` directly and does not
involve the error type, so the caller assumes responsibility for the
index being in bounds — out-of-range access is undefined behaviour at
the language level (the runtime currently traps the actor, but do not
rely on that). Reach for it only when the `or` clause is genuinely
adding noise without adding safety:

- A tight inner loop where profiling has shown the error union is the
  bottleneck.
- A small, local proof-by-construction that the index is in range and
  the `or` clause would be unreachable noise.

Even then, prefer iterating directly (`for x : arr`) over indexing.
`At()` should be a last resort, not a habit.

## Strings

Strings behave like most language. They're wrapped in double quotes and can
contain any printable character. Tabs and newlines can be used and special
characters can be escaped with '\'.

Example characters: '\n' (newline), '\t' (tab), '\\' (backslash), '\"', and
'\{'. The grammar contains an exhaustive list.

### String Access

String can be sliced into smaller strings or iterated over in a UTF-8 safe
manner. Strings are very similar to arrays in that individual characters can
be accessed.

Slicing takes two optional values, an inclusive starting value and an
exclusive ending value. Omitting the first value will start the slice from
the first character, omitting the second value will end at the length
of the string. Omitting both performs a full copy.

```
str := "hello"
c := str[1] // => "e"
substr := str[1..4] // => "ell"
front := str[..3] // => "hel"
back := str[3..] // => "lo"
copy := str[..] // => "hello"
```

Slices should not be treated as references. While the compiler may choose to
treat the underlying data as a reference for performance reasons, if the 
runtime detects a write, then it performs a full copy, ensuring all slices are
unique data.

## String Interpolation

A `{...}` inside a string holds an expression; its value is substituted where it
appears. `\{` writes a literal brace instead.

```
name := "world"
io.Println("hello, {name}")       // => hello, world
io.Println("{a} + {b} = {a + b}") // as many as you like
io.Println("not interp: \{x}")    // => not interp: {x}
```

The expression may open braces of its own — the interpolation ends at the `}`
matching its `{`, not at the first one seen. Struct literals, map literals and
block-bearing expressions all interpolate, as do strings nested inside them.

```
io.Println("point: {Point{x: 1, y: 2}.x}")
io.Println("parity: {if n % 2 == 0 { "even" } else { "odd" }}")
```

Multi-line strings interpolate on the same rule.

Any type that implements a `Stringable` interface can be interpolated in a String.
All the basic types can return their values as a string. Arrays and maps also
implement a String() method to provide a representation of their data.

For a struct type to be used as an expression in an interpolation, it must
implement the `Stringable` interface.

The interface lives in `std/proto`:
```
pub interface Stringable {
  String() String
}
```

Formatting must be done with a formatting method.

## Structs

Structs are not types themselves but describe the shape of a type. Only once
a struct is bound to an Identifier, does it become an actual type.

Structs don't have constructors, they're just the shape of data, but they do 
have a literal format for initializing them. Methods are bound to a struct
externally, with a receiver, within the file scope where the struct is defined.

```
struct Point {
  x, y Int
}

fn (p Point) Add(other Point) Point {
  Point{x: p.x + other.x, y: p.y + other.y}
}

struct User {
  pub firstName, lastName String // public
  email String                   // private by default
}

// Methods are bound with a receiver; field access goes through it.
// A public method can expose otherwise-private data.
fn (u User) FullName() String {
  "{u.firstName} {u.lastName}"
}

pub fn (u User) Email() String {
  u.email
}
```

There is no anonymous struct form. A shape is spelled once, as a declaration,
and referred to by name — so there is only one way to write it and a method can
always be bound to it.

Instead of `this` or `self`, you name the receiver. Field access always goes
through the receiver name — struct fields are never injected as bare locals.

```
struct Foo {
  name string // private
}

fn (f Foo) Named(value string) Foo {
  Foo{name: value}
}
```

A receiver method is a plain function namespaced to its type; there is no
hidden receiver or privileged field access.

The receiver is a value, exactly like a parameter, so the rules under
[Mutability](#mutability-memory-model) apply to it. Assigning to one of its
fields rewrites the method's own copy and the caller never sees it:

```
fn (c Counter) Bump() int {
  c.n += 1 // scratch: local to this call
  c.n
}
```

A method that changes a struct therefore returns the new value rather than
mutating in place — there is no by-reference receiver.

```
fn (c Counter) Incremented() Counter {
  Counter{n: c.n + 1}
}

c = c.Incremented()
```

### Self-referential structs

A struct may contain itself, as long as there is a way to stop. A struct holds
its fields inline, so a field that is just the struct again would need a value
of infinite size:

```
struct Node {
  value int
  tail Node // rejected: no finite size
}
```

Three things give it a way to stop. A union alternative:

```
struct Node {
  value int
  tail Node | Missing
}
```

An array, or a map:

```
struct Tree {
  value int
  kids array{Tree}
}
```

The union form is the list or tree you would reach for; the collection form is
the one with many children. In both cases the recursion bottoms out on a value
that holds nothing further — `Missing{}`, or an empty collection.

The cycle may also run through several structs, and they may be declared in any
order:

```
struct Expr {
  op int
  arg Operand | Missing
}

struct Operand {
  literal int
  nested Expr | Missing
}
```

A generic struct may reach itself too. Inside its own body, `Node<T>` is the
type being declared:

```
struct Node<T> {
  value T
  tail Node<T> | Missing
}
```

Walk a recursive shape by narrowing, the same as any other union:

```
fn (n Node) Sum() int {
  t := n.tail
  if t is Node {
    n.value + t.Sum()
  } else {
    n.value
  }
}
```

Note that `t := n.tail` is needed: `is` narrows a name, not an expression.

Where a struct closes a cycle, the union stores it on the heap rather than
inline — that is what bounds the size. This is not something you declare or
can observe: the value still copies, compares, and narrows like any other.

### Type bound methods

Methods can be bound directly to a type using selector syntax: 
`fn Type.Fn(...)`. This namespaces the method to `Type` and called as 
`Type.Fn(...)`. There is no `self` or `this`, and no field access. It is
intended to give a constructor-style method for building values of the type.

```
struct Point {
  x, y Int
}

fn Point.Origin() Point { Point{x: 0, y: 0} }
fn Point.Of(x, y Int) Point { Point{x: x, y: y} }

origin := Point.Origin()
p := Point.Of(2, 4)
```

The name is bound to the type, not the enclosing scope, so `Point.Origin` never
shadows an ordinary function named `Origin`. A bare type name reaches only its
type methods; `Point.x` is an error, since `x` is an instance field, not a
member of the type. 

### Default field values

A field may declare a default with `= expression`. The default must be a
compile-time value (the same expressions allowed for a `const`), and it must be
assignable to the field's type. A list of names shares one default.

```
struct Config {
  timeout int = 30
  name string = "anon"
  active bool = true
  retries int            // no default — zero value when omitted
}
```

When a struct literal omits a field, its default is applied; a field with no
default takes its zero value. A value given in the literal overrides the
default. Defaults from an embedded struct are applied too, so embedding a
struct with defaults keeps those defaults.

```
Config{}                 // timeout 30, name "anon", active true, retries 0
Config{timeout: 5}       // timeout 5,  name "anon", active true, retries 0
```

### Struct literals

To initialize a struct, its literal form must be used. The struct's identifier,
or a `pkg.Type` selector, must precede the literal.

```
struct Point {
  x, y Int
}

p := Point{x: 2, y: 4}
q := geom.Point{x: 2, y: 4} // from another package
```

### Struct access

Since the structs of a field are stronly typed, they can be accessed with dot
access, called a selector.

```
struct Inner {
  b int
}

struct Outer {
  a Inner
}

data := Outer{a: Inner{b: 0}}
b := data.a.b
```

When parsing non-deterministic data like JSON (optional keys), structs and union
types come to the rescue. By declaring a field with the union `Type | Missing`,
where Type is whatever type you expect the field to be when present, you unlock
an optional field. Optional fields must be resolved to a concrete type to use
them, either with a type match or the `or` keyword.

```
struct Payload {
  optional String | Missing
}

raw := net.Get("/some/api")
data := json.Parse(raw, Payload)
value := data.optional or { "unknown" }
```

### Struct embedding (mix-ins)

A struct may be embedded inside another struct. Unlike class inheritance, a
struct that is embedded passes its members and methods on to the child struct
but does not create a parent-child hierarchical inheritance.

The "child" struct, the one receiving the embedding, gains all the fields and
methods of the embedded struct as if they were its own. The embedded struct
cannot reach out to the child; the child can reach in. This is like onion
architecture: the outside can see in, the inside can't see out.

```
struct Timestamps {
  created int
  updated int
}

// A method bound to Timestamps, with a named receiver.
pub fn (t Timestamps) Age() int { t.updated - t.created }

// This is NOT inheritance — User embeds Timestamps.
struct User {
  Timestamps
  name string
}

u := User{name: "Alice", created: 100, updated: 175}
u.created   // promoted field
u.Age()     // promoted method
```

A promoted method still runs against the embedded struct's own fields; calling
it through the child simply finds the embedded value to act on. The embedded
struct keeps its own memory inside the child.

Embedding is transitive: if `User` embeds `Base` and `Base` embeds `Timestamps`,
then `Timestamps`' fields and methods are promoted all the way up to `User`. A
field or method shadows embedded methods of any depth.

Structs with only methods and no fields make useful mix-ins of pure behaviour.

A child member shadows an embedded member of the same name: if the child
declares a field or method whose name an embedded struct also uses, the child's
wins for direct access.

```
struct Base {}
pub fn (b Base) Kind() string { "base" }

struct Child {
  Base
}
pub fn (c Child) Kind() string { "child" }

c := Child{}
c.Kind() // returns "child" because the child's method shadows the embedded one
```

#### Reaching the embedded value

Shadowing hides a name, not the storage behind it. The embedded struct keeps
its own memory inside the child, and it answers to its bare type name — so
`u.Timestamps` reaches that value directly, for reading, writing, and
initialising:

```
struct User {
  Timestamps
  created int // shadows the embedded `created`
}

u := User{Timestamps: Timestamps{created: 100}, created: 5}
u.created            // 5   — the child's
u.Timestamps.created // 100 — the embedded one
u.Timestamps.Age()   // the method, run against the embedded value

u.Timestamps.created = 77 // writes the embedded field; u.created stays 5
```

Without this, a child field would make the embedded field it shadows
unreachable in both directions, and a promoted method reading that field would
quietly see a zero.

This is **not** `super`. `u.Timestamps.Age()` is ordinary field access followed
by ordinary dispatch: it finds the embedded value and calls the method on
*that*. There is no chain to walk up and no way to re-enter the child, which
is the same onion rule as above — the inside can't see out.

Qualified access composes one name at a time, so a struct embedded two levels
down is reached as `u.Base.Timestamps.created`.

An embedded type is always named by its **unqualified** name, even when it
comes from another package: `lib.Timestamps` is declared with the package
qualifier but reached as `u.Timestamps`. Because that name has to stay
unambiguous, it is an error for a declared field or a second embed to claim it.

```
struct Bad {
  Timestamps
  Timestamps string // Error: collides with the embedded type's name
}
```

```
struct Greeter {
    pub fn Greet() string {
      "Hello!"
    }
}

// Note that the visibility and return type are erased by the new definition
struct Different < Greeter {
    fn Greet() void {
        io.Println(Greeter.Greet()) // ...but can still access the original
    }
}
```

### Struct-shape literals

A shape with no fields may omit the `{}` when the type is known. The bare type
name and the empty literal are equivalent:

```
struct Marker {}

a := Marker    // same as Marker{}
b := Marker{}
```

This omission is the only case where a bare type name stands for a value. A type
name used as a value anywhere else — including a shape that has fields — is a
compile error; use the literal form instead.

## Interfaces

Interfaces describe a set of desired behaviour. An interface is a list of 
methods that a type must implement in order to match the interface. Interface
matching is implicit. A type satisfies an interface simply by having every
method.

```
interface Reader {
  Read() string
}
interface Writer {
  Write(s string) void
}
interface Closer {
  Close() void
}
```

An interface must declare at least one method. An empty interface would
describe no behaviour and is a compile error.

### Composing interfaces

Interfaces are intended to be small and composed. An interface composes others
by *embedding* them. Including an interface as a member merges its whole method
set into the composed interface.

```
interface ReadWriter {
  Reader
  Writer
}
interface ReadWriteCloser {
  ReadWriter
  Closer
}
```

A value matches `ReadWriter` only if it implements both `Read` and `Write`;
`ReadWriteCloser` requires all three. Embedding is transitive (a composed
interface carries methods from interfaces embedded several levels deep) and
order-independent. An embedded interface may be qualified by its package
(`io.Reader`).

Method names must be unique. Redefining a method with the same shape is
accepted but redefining a method with a different shape is an error. This
allows composing interfaces that might have overlap.

For example, combining a ReadWriter and ReadCloser to make a
ReadWriteCloser would be safe provided the Read() shapes are identical.

```
interface NewReader {
  Reader
  Read() string       // Okay, same shape
  Read() string | int // Error, different shape
}
interface ReadWriteCloser {
  ReadWriter
  ReadCloser // Okay, provided Read() has the same shape as ReadWriter
}
```

## Enums

An enum is a type whose values are a fixed set of named variants. Each variant
is a distinct value of the enum type. By default an enum is backed by an `int`
ordinal: the first variant is `0` and each following variant counts up by one.
Variants are separated by newlines, not commas.

```
enum Color {
  Red    // 0
  Green  // 1
  Blue   // 2
}

fn Paint(c Color) void { ... }

Paint(Color.Red)
Paint(Color.Purple) // error, no variant named Purple
```

### Backing values

A variant's ordinal can be set explicitly with `= Expression`. Later variants
without an explicit value keep counting from the previous one. Ordinals must be
unique.

```
enum Suit {
  Clubs = 1
  Diamonds  // 2
  Hearts = 5
  Spades    // 6
}
```

An enum may instead be backed by a string by writing `string` after its name.
Each variant's backing string defaults to the variant's own name and can be
overridden with `= "..."`.

```
enum Color string {
  Red = "r"
  Green      // "Green"
  Blue = "b"
}
```

### Accessors

Every enum carries three synthesized methods:

- `.Int()` returns the ordinal as an `int`.
- `.String()` returns the variant name for an int-backed enum, or the backing
  string for a string-backed enum.
- `Enum.From(value)` is the inverse of the backing: it looks up the variant
  whose backing value equals `value` (an ordinal, or a string for a
  string-backed enum) and returns `Enum | error` — the variant on a match, a
  `Missing` error on none.

```
Color.Red.Int()                    // 0
Color.Red.String()                 // "r"
Suit.From(5) or |e| { Suit.Clubs } // Suit.Hearts
```

`Int`, `String`, and `From` are reserved: a user method may not redefine them
(see [Method uniqueness](#method-uniqueness)).

### Methods

Methods can be attached to an enum. The receiver is the enum value, passed by
value.

```
enum Dir {
  North
  East
  South
  West
}

fn (d Dir) Opposite() Dir {
  switch d {
    case Dir.North: Dir.South
    case Dir.East:  Dir.West
    case Dir.South: Dir.North
    else:           Dir.East
  }
}

Dir.East.Opposite() // Dir.West
```

### Variant shorthand

Where the enum type is already known from context — a typed declaration, a
return, a call argument, a struct field, a switch case, or the other side of a
`==`/`!=` — a variant can be written with a leading dot and no enum name.

```
c Color = .Red
if c == .Red { ... }
Paint(.Green)
```

The shorthand is only allowed where the target enum is unambiguous; a bare
`.Red` with no expected type is an error.

### Enums are identity types

An enum names a choice, not a quantity. Enums support equality (`==`, `!=`) but
no arithmetic: `+`, `-`, `*`, `/`, `%`, and unary `-` are rejected, and an enum
cannot overload them. To compute with the ordinal, take it explicitly with
`.Int()`.

## Method Calling

Calling a method requires referencing a method by name and providing optional
arguments, encapsulated by parentheses.

```
fn Nothing() Void {}
Nothing() // call Nothing, compiler makes this a no-op

pub fn Add(a, b Int) Int { a + b }
r := Add(1, 2) // result is 3
```

## Closures

Anonymous functions, or a function expressions, can be used inline within a
block and can close over local variables.

A function expression has a signature and a body that can be assigned to a
value (struct field, local variable, or method parameter).

```
// simple anonymous function 
anon := fn () Int { 42 }
answer := anon()

// closure
i := 1
closure := fn (x Int) Int { x + i } // closes over i
x := closure(2) // => 3

// supports generics
generic := fn |T| (value T) Void { io.Print(value) }
|Int| generic(42)
```

## Selectors

To access sub-elements of a package, map, or struct you need to use the dot
access syntax. It is an identifier, a dot ("."), followed by another
identifier.

```
pkg.ExportedElement

struct Box{ value Int }
i := Box{value: 42}.value // => 42
```

## Conditionals

There are two conditional expression-statements: if and switch. Like with
methods, the tail expressions form the return value when used an expression.
When used as a statement, the conditional has no type and the tail expression
can be any type.

If can also be used as a ternary. Since newlines terminate an expression, the
language naturally discourages complex block in its ternary form.

```
// as statement
if x > 10 {
  do_something()
} else {
  do_something_else()
}

// as expression
x := if y > 10 {
  0
} else {
  y
}

// as ternary
x := if y > 10 { 0 } else { y }
```

Switches handle multiple branches, performing a value comparison. The first
branch determines the type when used as an expression and the left hand
value is being initialized without a declared type. The right hand side of
a case statement can be an expression or a block. The first case also
determines the comparison type. It is illegal to mix type matching and value
matching in the same switch. Switch cases do not fall through. The else clause
is optional.

To have a multi-line case clause, use a block.

Case clauses can have multiple expressions separated by commas to match
multiple values to a single clause.

```
// as statement
switch value {
  case 0: 0 // expression
  case 1: {} // block
  else: 1 // optional else
}

// as expression
x := switch value {
  case 0: 0  // first value determines return type when inferring
  case 1: {} // an empty block returns the zero value of the return type
  else: 'a' // error, type mismatch
}
```

### Conditional types

A note on the return type: The type of the left hand side variable determines
the expected return type. If the left hand side is being initialized, and no
type was provided, then then `then` block's tail expression determines the
type.

### Type matching

Conditionals perform double duty. They can be used in value form and match
form. Performing a comparison with a type performs a type assertion, and
narrows the type. This is useful for union types.

Type matches are required to be exhaustive if an `else` block is not supplied.

```
value Int | Float = getValue()
if value == Int {
  // value is an integer, the type is narrowed to an Int
} else {
  // value is a float, the type is narrowed to a Float
}

if value == Int {} // error, non-exhaustive

value Int | Float | String = getValue()
if value == Int {
  // value is an integer, the type is narrowed to an Int
} else {
  // value is Float | String; must be further narrowed to be used
}

// exhaustive, okay
switch value {
  case Int: 0
  case Float: 0
  case String: 0
}

// exhaustive, okay
switch value {
  case Int: 0
  else: 0
}

// non-exhastive, error
switch value {
  case Int: 0
}
```

The same rules apply with the expression version's return type as with values.

```
value Int8 | Int16
i8 := if value == Int8 {
  value // i8 MUST be an Int8 now
} else {
  value.Int8() // value is Int16 and must be converted to an Int8
}
```

## Looping

There is only one looping constract: "for". It has multiple forms: infinite,
condition-only, and iteration. For loops can be be advanced with 
`next` and exited early with `break`. A `return` inside the loops exits the
function scope entirely but can also "early exit" a loop.

`for` is an expression-statement.

`next` skips any further processing and advances to the next iteration of the
loop.

`break` can be given an argument, which is useful when using `for` in its
expression form.

The type of a for expression is inferred from the first value that the
compiler finds being returned from the loop. This is likely a break statement
or the tail expression. It is recommened to explicitly type the left hand
value to avoid ambiguity.

```
// loop indefinitely, requires a break or return to exit
for {
  break
}

// single conditional
running := true
for running {
  running = false
}

// index iterator
for i Int; i < 10; i += 1 {}
```

Collections like arrays, maps, and strings can be iterated over using
the for-range form. If only a single variable is supplied, the value of each
element is captured. If using two variables, both the key and value are
returned.

```
// arrays
arr := [1, 2, 3]
for v : arr {} // 1 => 2 => 3
for k, v : arr {} // index, value form (0,1) => (1,2) => (2,3)

// maps
map {String: Int} = {"a": 1, "b": 2, "c": 3}
for v : map {} // 1 => 2 => 3
for k, v : map {} // ("a", 1) => ("b", 2) => ("c", 3)

// strings
string := "abc"
for k : string {} // "a" => "b" => "c"
for k, v : string {} // (0, "a") => (1, "b") => (2, "c")
```

Any type could conceivable by adapted to be used in a `for` loop. It needs to
satisfy the `Iterable` interface, which has the following signature:

```
interface |T| Iterable {
  Next() T | Error
}
```

See [Generics](#Generics) for more information. The standard library supplies
the type `Missing` to signal there are no more elements to iterate over, which
satisfied the `Error` interface.

### Accumulation

This is the secret superpower of `for`. When using `for` as an expression, the
compiler uses the type of the left-hand value to initialize an internal
accumulator. This accumulator is initialized to the zero value of the type. The
result of the expression is the accumulator. 

A user can name the accumulator anything they want, though `acc` will probably
be common. To access the accumulator, use the pipe syntax.

```
arr := [1, 2, 3]
sum := for i : arr |acc| {
 acc += i 
}
```

If `break` is present anywhere in the block, the expression becomes impure,
returning from the loop immediately, and returning a `Missing` error. This
allows for search patterns. The return type of the `for` expression becomes
`T | Error` and the value from the accumulator is ignored.

```
arr := [1, 2, 3]
sum := for i : arr |acc| {
  if i == 2 { break 1 } // sum => 1
  acc += i
}
```

The type determines the behaviour of the accumulator. Types not listed here do 
not generate an accumulator.

```
// Filtering
array := [1, 2, 3, 4]
// The left hand type is an integer array, so that's the type of the accumulator
evens Int[] = for i : array |acc| { if i % 2 == 0 { acc.Push(i) } } // => [2, 4]

// Mapping
array := [1, 2, 3, 4]
doubles Int[] = for i : array |acc| { acc.Push(i * 2) } // => [2, 4, 6, 8]

// Reducing
array := [1, 2, 3, 4]
sum Int = for i : array |acc| { acc += i } // => 10

// Searching
array := ["a", "b", "c"]
result := for word : array { if word < "b" { break word } } // => "a"
```

For finding the product, difference, or quotient, the user must handle that
themselves. A future consideration is to allow an `acc` or `accumulator`
variable to be injected into the loop's scope.

_Performance Note: If the accumulator isn't asked for, the compiler does not
generate any code for it._

## Variable declarations

A local (block) variable can be declared in two ways: explicitly typed or
inferred type with assignment.

```
x Int // explicit type
y := 1 // implicit type
```

Using a variable before it is declared is an error. Redeclaring a variable
(shadowing) is also an error. Declarations are statements.

### Unused variables

A local that nothing ever reads is an error. It is dead code, a typo, or an
abandoned intent, and the fix is always the same: remove it, or name it as
[ignored](#Identifiers).

Assignment is not a read. A variable only ever written to is still dead —
every write to it is discarded — so `=`, the compound operators, and `++` /
`--` do not keep a variable alive. Writing *through* a variable does read it:
`p.x = 1` and `arr[0] = 1` need `p` and `arr` to find where to write.

```
x := 1        // invalid, nothing reads x
y := 1
y = 2         // invalid, still nothing reads y
_ := 1        // fine, ignored
```

The rule covers every local you name yourself, including loop variables and
the `or` pipe — both are optional, so an unread one can always be dropped or
ignored:

```
for _ : arr {}       // rather than an unread `v`
f() or { 0 }         // rather than an unread `|err|`
```

Parameters are exempt. A parameter's name is part of a signature that a
caller, an interface, or a callback shape may fix, so removing it is not
available as a fix. A `for` accumulator is exempt for a different reason: the
loop's value *is* the accumulator, so the expression reads it even when the
body only assigns to it.

## Assignment

There are several assignment operators: 

`=`: Standard assignment
`+=`: Addition assignment (`x += 2` == `x = x + 2`)
`-=`: Subtraction assignment (`x -= 2` == `x = x - 2`)
`*=`: Multiplication assignment (`x *= 2` == `x = x * 2`)
`/=`: Division assignment (`x /= 2` == `x = x / 2`)

`++`: Increment (`x++` == `x = x + 1)`)
`--`: Decrement (`x--` == `x = x - 1)`)

Assignments return type `Void`, thereby making them statements. Therefore,
they can not be used in an expression.

## Unary Operators

A unary expression only has on operand.

**Operators** = ! (logical not), - (negation)

```
x := !true // => false
z := -5 // => -5
```

### Truth

There is no "truthy" or "falsy". Logical operators can only be used with
logical expressions. Using `!` with any expression that does not resolve
to `true` or `false` is an error.

## Binary Operators

A binary expression has two operands, a left and right hand side, separated by
operator.

**Arithmetic**: `+` (add), `-` (subtract), `*` (multiply), `/` (divide),
  `**` (exponential), `%` (modulus)
**Bitwise**: `&` (AND), `|` (OR), `^` (XOR), `~` (NOT), `<<` (Left shift),
  `>>` (Right shift)
**Logical**: `==` (Equal), `!=` (Not Equal), `>` (Greater), `<` (Less), 
  `>=` (Greater than or Equal), `<=` (Less than or Equal), `&&` (and), `||` (or)

Arithmetic operators apply to numeric types (`+` also concatenates strings).
Enums and errors are identity/data types: they support equality but no
arithmetic, and they cannot overload it. Structs may overload the operators
through methods (`Add`, `Sub`, `Mul`, `Div`, `Equals`, `Compare`).

### Division by zero

Division is special amongst operators in that it can exhibit exceptional
behaviour. Namely, dividing by zero is an error. In most languages this would
raise an exception but in this language it produces an impure type. There are
two ways the type checker can be assured that a division operation is safe.

The first, is to use an `or` expression to resolve the impure return type and
return a zero value. The second is to pre-check that the divisor is safe
(non-zero). If the divisor is checked to be non-zero and it can't be mutated
between the check and the usage, then the compiler will allow the inline 
division.

```
// wrapping the division in parenthesis is preferred but not manditory
x := (6 / 0 or { 0 }) + 1 // => 1

divisor := 1
if divisor == 0 { 
  // do some logging, return an error, etc
  return BadCalculation{message: "Division by zero"}
}

// divisor was checked and divisor is not zero and has not been mutated
result := 42 / divisor + 10 // safe, no `or` check necessary
```

This eliminates the possibility of a "div by zero" crashing a program. The
compiler will warn you if the division is unsafe and not handled.

## Concurrency

The "spawn" keyword is used to spawn a new thread. It returns a Task struct 
that can be used to stream data via a channel and synchronize th thread. It
takes either a block or function. A type argument specifies the channel type.

Channels are just functions. Essentially, they're a callback into the parent
thread. This lets data get passed between threads safely. If a function is
passed to a thread, the channel callback can be received by defining a 
callback.

Note: Likely, the standard library will provide a handful of channel types for
the user. Something like `spawn.IntChannel`

```
spawn {}
spawn concurrentFunction
```

### Tasks

While tasks will be convered in detail within the [standard library](stdlib.md)
documentation, it's worth stating there are several methods that should proove
useful. Tasks are a Generic type, and so the channel it operates on can be 
passed in, like any other Generic type.

```
t := spawn { ... }
t.Alive?() // is thread running?
t.Cancel() // ask the thread to stop
t.Term() // terminate the thread immediately
t.Wait() // block until the thread finishes
```

From inside the thread, you get a context task.
```
t.Cancelled?() // did the parent call Cancel()?
t.Error() // exit with an error
t.Exit() // exit with a value
t.Send() // non-blocking, buffered
```

Putting them to use:
```
task := |String| spawn |task| {
  // perform async operations
  task.Send("hello") // send a string down the pipe
}

for msg : task {
  io.Println(msg)
} 
```

When streaming, if the task ends unexpectedly or completes, it signals that
the collection has no more records.

# History

-  9 May, 2026: Draft v0.3.1 (current)
-  6 Mar, 2026: Draft v0.3
- 25 Feb, 2026: Draft v0.2
-  7 Feb, 2026: Draft v0.1
