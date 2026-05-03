# Language, Draft v0.3

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

Identifiers that start with with, or consist only of, an underscore are
"ignored" variables. They can not be accessed once they are assigned a value
and the compiler will not flag them as an unused variable.

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

Constants are immutable. They can be used to create: type-aliases, value
constants, and named imports. A value constant can be any simple expression
that can be resolve at compile time.

```
const MyType = Int
const Pi = 3.14159
const MaxSize = 2 * 1024 * 1024 // 2MB
const Math = import "std/math"
```

Mutation of global objects is prohibited.

## Initialisation

Constants whose value can be represented as a single compile-time literal —
scalars, strings, struct literals built from such values — are stored
directly in the binary. Constants whose value requires allocation at
runtime — arrays and maps — are populated before `Main` runs.

```
pub const Pi Float = 3.14159           // baked into the binary
pub const Primes [Int] = [2, 3, 5, 7]  // populated before Main
pub const Ports {String: Int} = {"http": 80, "https": 443}
```

When a binary imports another package, the imported package's
initialisation runs first. The order is determined by the import graph: a
package's constants are guaranteed to be initialised before any package
that imports it, transitively.

Within a single package, files are processed in alphanumeric order, and
declarations within a file are processed in textual order. A constant
initialiser may read another constant declared earlier — earlier in the
same file, earlier within the package's file order, or in any imported
package. A constant initialiser must not read a constant declared later
in the same package; the read will see the type's zero value with no
diagnostic.

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
pub fn Connect(url String) Connection { ... }
```

Saga today does not run any of its own code before `Main` — no
spawn-driven work, no signal handlers, no threads. A package's
initialisation can therefore allocate freely without guarding against
concurrent access. This is a property of the current runtime; it is
documented here because the initialisation model relies on it.

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
For methods multiple return types, a return statement is required.

### Multiple return values

A method can declare multiple return types in its signature. There are two
caveats multiple return values:

1. You must use a `return` statement to return multiple expressions.
2. The `or` expression can not be used directly to purify the types.

```
pub fn MultiTypes() Int, String {
  return 42, "Hello"
}

x, y := MultiTypes() // lhs receiver values must match count of return types

pub fn MultiTypeError() Int, String | Error {
  return 42, Missing
}

x, y := MultiTypeError() or { ... } // invalid
s := y or { "Unknown" } // okay
```

_Note: This could be relaxed in the future for tail types._

### Variadics

Methods can accept an arbitrary number of arguments by declaring a variadic
parameter. It must be the final type in the signature. By prepending the type
name with `...`, the parameter can accept any number of arguments. The
variable itself is just an array.

```
fn Sum(args ...Int) Int { // args is of type [Int]
  for i : args |acc| { acc += i }
}
```

Arrays of the same type do not need to be spread into a variadic. There is
literally no work for the compiler to do if you pass an array in.

```
// multiple arguments
sum1 := Sum(1, 2, 3, 4, 5)

// or pass in an array
sum2 := Sum([1, 2, 3, 4, 5]) // [Int] matches the argument's actual type

// but you can't mix and match
sum3 := Sum(1, 2, [3, 4]) // The types are Int and [Int], no match
```

## Types

There are nine standard intrinsic types. Six have identifiers: Bool, Byte,
Float, Int, String, and Void. Three are identfied by their shapes:
`[Type]` (array), `{Type: Type}` (map), and `(Expr..Expr)` (range). The two
numeric types (Int and  Float) are aliases of the word size variant (Int32 and
Float32 on a 32bit  system, and Int64 and Float64 on a 64bit system, and so
on). Byte is an alias of UInt8.

Arrays, maps, and ranges are shapes, so the names `array`, `map`, and `range`
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

Arrays, maps, ranges, and strings are all fat types. Each is backed by struct
that contains a reference to backing data and metadata, like size.

```
arr := [1, 2, 3] // type is inteffered by the first value: [Int]
arr.Size() // => 3
```

To convert a type like `[Byte]` to a String, a conversion utility method
must be used, since String supports UTF-8.

Complex types can be self-referential provided they're defined on or before
their first use.

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

Function types are just signatures.

```
const CallbackFunc = fn(Int) Int

struct MyStruct {
  action fn(Int) Int
}
```

### Pure union types

A pure union is one that contains any combinations of types that is not an 
Error type. Example: `Bool | Int` is a union of a boolean and integer type.
The size of this type is the size of the largest type.

Including more than one of the same Type is an error: `Int | Int`. A type
alias of another type is not the same type: `Int : MyInt`. They are treated
as distinct types with different behaviours.

### Impure union types

Impure types are type unions that contain an Error type: `Int | Error`. Errors
must be resolved with the `or` keyword before they can be narrowed, or 
"purified". This strips the error from the type and returns the resulting
type, union or not, without an `Error` type.

### Error type and Missing

The [Error](./language.md#error) type is an interface which expects a concrete
type to implement a public method called `Message()` that returns the type `String`.
The language also includes a type [Missing](./language.md#missing) that 
implements this  interface. To require a specific error type, you can use that
type directly or use Unions for a list of specific error types. Like any type,
Errors can be narrowed with type matching.

Creating your own error type can be as simple as creating a type that 
implements the `Error` interface. This can be a powerful tool for adding extra
metadata to your errors.

```
struct BasicError {
  message String
  
  pub fn Message() String { message }
}

struct NetworkError < BasicError {
  code Int
  
  pub fn Message() String { "Network error [{code}]: {message}" } 
}
```

### Or clause

The or clause is used to resolve Error types. It's an incredibly powerful tool
for "correctness" or provide default values. To capture the error itself, use 
the pipe syntax prior to the block and give it a name. This is really useful 
for logging and default values.

While `err` is probably the most common name, you can call it whatever you like.
The variable is locked to the following block's scope, so no need to worry if
the outer block has a variable with the same name, it won't be an error.

```
value Int | Error = 0
i := value or { 1 } // i is now an Int

many_types Int | String | Error
int_or_string := many_types or {} // strips Error, Int | String remains

// capturing the error
json := http.Get("example.com/api/data") or |err| {
  log.Error(err)
  "{}" // return a valid (empty) JSON string
}
```

By convension, the Error type should be the final type of a union. If code
needs to return multiple error types, use `Error`. Including more than one
Error type in a type union will result in an error.

```
data := http.Get(...) or |err| {
    switch err {
      case NetworkError: log.Error(err)
      case ParseError: log.Warn(err)
      else {}
    }
}
```

### The 'Any' type

`Any` is a special type and behaves a little differently than in most
languages. Any isn't an empty interface that all types implicitly implement,
nor is handy way to avoid strict typing. Any is more like a union of all
intrinsic types.

`Any` is intended for use for the rare moments when the type can be many
non-deterministic types. Like all types, it can only be a type of itself, and
it must be narrowed to actually use it. In order to instantiate an `Any` type,
you must use the `"std/unsafe"` package.

Like most data structures in the language, it's a fat struct that has multiple
conversion methods on it. Every conversion method returns an impure union and
must be checked.

The `Any` type comes with many helper methods to check what type, or types, it
contains. It is meant to be a bridge between non-deterministic input like JSON.

```
// inside a library
import "std/unsafe"

data := unsafe.Any{value: 0} // creating a variable of type any from an integer

fn ReturnAny() Any { data }

// inside a consumer
any := lib.ReturnAny()
i Int = any // error, Any can not be type Int
i Int = any.Int() or {} // attempt to extract an integer
i Int = if any == Int { any } else {} // Error, Any is not an Int
i Int = if any.Int?() { any.Int() } // Compiler detects the type was narrowed
```

### Generics

Generics are not tacked; types flow through pipes. Generics are available to
only structs and functions. When used appropriately, in combination with
interfaces, union types, and `Any`, they can be a powerful tool.

When a generic is instantiated, if it's type can be inferred, the generic type
can be omitted.

Generics are monomorphic. The type must be known at compile time since the
compiler must generate a typed copy of the data or expression.

```
// declare a generic struct
struct |T| Box { value T }

// instantiate a generic struct
box := |Int| Box{ value: 0 } // explicit type, piped into the Generic box type
box := Box{ value: 0 } // Int is inferred.
```

Here's an example of mapping a Generiic List of type T to a List of type U:
```
// A generic function that takes a list of T and returns a list of U
fn |T, U| Map(list |T| List, transform fn(T) U) |U| List {
    return for item : list |acc| {
        acc.Push(transform(item))
    }
}
```

Putting it all together using an example of a Linked List:

```
struct |T| Node {
  value T
  next  |T| Node | Missing
}

struct |T| List {
  head |T| Node | Missing
  curr |T| Node | Missing

  pub fn Push(val T) Void {
    new_node := |T| Node{value: val, next: head}
    head = new_node
  }

  pub fn Peek() T | Error {
    node := head or { return Error{"Empty"} }
    return node.value
  }
  
  pub fn Next() T | Missing {
    // If curr is Missing, start at head. Otherwise, move to next.
    node := curr or { head } or { return Missing }
      
    curr = node.next
    return node.value
  }

  pub fn Reset() { curr = Missing }
}

// Usage
list := |Int| List{}
list.Push(42)
val := list.Peek() or { 0 }

for node : list {} // Next() lets you iterate over the list.
```

## Type Aliases

A type may be aliased to a new identifier. Aliases are constants and therefore
are defined with the `const` keyword.

Aliases are not shadows of the original type, they are unique types, but they 
inherit all the methods from the aliased type. This can make for a powerful
tool where all types are open for extension but closed for modification. Like 
structs, methods can be bound to any type provided it is within the same file
scope. You can't, for instance, bind methods to types declared in other files
(called monkey-patching).

```
const UserID = Int  // UserID is a unique type
i := UserID.Int() // to convert and extract the underlying integer

const MyArray = [MyArray] // MyArray is an array of itself, infinitely
size := MyArray.Size()  // Inherits Size() from the Array type.

const MyPoint = math.Point
// The new type inherits the methods from the original type
p := MyPoint{x: 1, y: 2}.Add(math.Point{x: 3, y: 4})
// ...but can also be extended with new methods
pub fn (p MyPoint) Draw() Void { ... }
```

## Type Literals

All types have zero values. If a type is defined without specifying a value, it
is assigned a zero value by the compiler.

| Type | Zero Value | Values |
|---|---|---|
| Bool | false | true, false |
| Byte | 0 | 42, 255 |
| Float | 0.0 | 3.14, 0.4e-10 |
| Int | 0 | 42, 0b1010, 0o775, 0x1f |
| String | "" | "single-line", """multi-line""" | "{expr}" |
| Void | | |
| [Type] | [] | [1, 2, 3] |
| {Type:Type} | {} | {"key": 42} |

Multiline strings also support interpolation.

In the case where a type might be ambiguous, either it must be made explicit
or it will be a type error.

```
arr1 := [] // invalid, no inferrable type
arr2 [Int] = [] // valid, type is known
arr3 [Int] // assigning an empty value isn't actually needed, that's the zero value
arr4 := [1] // type can be inferred
```

### Map Literal

```
// Nested map literal
registry := {String: {String: Int}}{
  "production": {"port": 80, "timeout": 30},
  "staging":    {"port": 8080, "timeout": 60}
}

// Nested literal with map type
const EnvironmentMap = {String: {String: Int}}
registry EnvironmentMap = {
  "production": {"port": 80, "timeout": 30},
  "staging":    {"port": 8080, "timeout": 60}
}
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

## String Iterpolation

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
have a literal format for initializing them. Methods are bound to structs when
they are defined or they can be post-declaration-bound provided the binding
occurs within the scope in which the struct was defined (File scope).

```
struct Point {
  x, y Int
}

// Post declaration binding, or "out-bound"
fn (p Point) Add(other Point) Point {
  Point{x: p.x + other.x, y: p.y + other.y}
}

struct User {
  pub firstName, lastName String // public
  email String                   // private by default
 
 // a structs fields enter the local scope when not using a receiver 
  pub fn FullName() String {
    "{firstName} {lastName}"
  }

  // Reciever binding, or "in-bound", struct fields are not added to the local
  // scope. a public method can be used to extract a copy of private data 
  pub fn (u User) Email() String {
    u.email
  }
}
```

Structs can also be bound to local variables or passed to methods by using an
anonymous struct. Methods can not be bound to anonymous structs.

```
pub fn Foo() String {
  s := struct{id Int, name String}
  user := net.Get("User", s)
  "{s.id}: {s.name}"
}
```

The choice between in-bound and out-bound methods is whether you need to "shadow"
a variable or provide clarity on the receiver of the field access. Instead of
`this` or `self`, you name the receiver. Prefer in-bound definitions unless
direct receiver access is required.

```
struct Foo {
  name String // private
  
  // shadows the struct's field, which would raise an error
  fn (f Foo) SetName(name String) String {
    f.name = name
  }
}

struct Bar < Foo {
  name String // erases Foo's name field from Bar's scope
 
  // disambiguates ownership 
  fn (b Bar) SetName(name String) String {
    b.name = Foo.SetName(name)
  }
}
```

Internally, the compiler treats all struct functions as having a hidden
receiver. The receiver syntax is only a method for the user to gain access
to a reference to the struct for disambiguation. 

### Struct literals

To initialize a struct, its literal form must be used. The structs Identifier,
or an anonymous struct, must proceed the literal.

```
struct Point {
  x, y Int
}

// named struct
p := Point{x: 2, y: 4}

tmp := struct{name, email String}{name: "Jane", email: "jane@example.com"}
```

### Struct access

Since the structs of a field are stronly typed, they can be accessed with dot
access, called a selector.

```
data := struct{a struct {b Int}}{a: {b: 0}}
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

### Structural embedding (mix-ins)

Structs can be merged, or "mixed in" to each other. The syntax will look
familiar to those used to inheritance but it's important to note this is
NOT inheritance.

The "child" struct, the struct receiving the embedding, inherits all the
fields and methods of the other struct. The embedded struct can not access
any fields or methods outside of its own scope but the child struct can
access all the fields and methods of the embedded struct. This is similar
to onion architecture. Things on the outside can see in but things on the
inside can't see out.

```
struct Timestamps {
    created_at Int
    updated_at Int
    
    pub fn Touch() Void {
        updated_at = time.Now() // Implicit scope access!
    }
}

// this is NOT inheritance
struct User < Timestamps {
    name String
}

u := User{name: "Alice"}
u.Touch() // Feels like a native method, acts on 'updated_at' inside 'User'
```

Structs that do not have fields and only methods become Traits. That doesn't
really do anything but make it a nice way to refer to them. That's it.

Methods in a parent struct can shadow those of a mix-in. If you need to access
the embedded struct's method, you can call it by its struct name.

```
struct user < Timestamps {
  name String
 
  // making this private "erases" the visibility from this struct's available
  // symbols
  fn Touch() Void { Timestamps.Touch() } 
}
```

The fields and methods are merged into the child's symbol table. If a mixin's
methods are called, it's accessing the child's fields but thinks they're its
own fields. That doesn't make the mixin's fields disappear. If a child defines
a field or method that shared the same name as
an embedded struct's, then the child erases the mixed in version. The mixin
can still "see" its original fields can can still access them. For the child
to access the embedded version, it must use a selector.

The embedded struct has its own memory so that when erasure from the embedded
target happens, it can still access its own fields. The merge is symbolic and
the field is not actually erased.

```
struct Greeter {
    pub fn Greet() String {
      "Hello!"
    }
}

// Note that the visibility and return type are erased by the new definition
struct Different < Greeter {
    fn Greet() Void {
        io.Println(Greeter.Greet()) // ...but can still access the original
    }
}

u := User{name: "Alice"}
u.Touch() // Feels like a native method, acts on 'updated_at' inside 'User'
```

## Interfaces

An interface is a list of one or more methods that a type must implement in
order to match the interface. Interface matching is implicit. 

Interfaces are intended to be small and composible with union types.

```
interface Reader {
  Read() String
}
interface Writer {
  Write(String) Void
}
interface Closer {
  Close() Void
}

const ReadWriter = Reader | Writer
const ReadCloser = Reader | Closer
const WriteCloser = Writer | Closer
const ReadWriteCloser = Reader | Writer | Closer
```

Interfaces are not narrowed by a union, they are widened. Using the above
example, a `ReadWriteCloser` expects all three methods be implemented in
order to match it. On the other hand, an interface can be narrowed 
provided the receiving interface is a subset of the wider interface.

```
// A File return by Open satisfies the interface because it implements a
// Read, Write, and Close method.
f ReadWriteCloser = io.Open("file.txt")

// Since ReadCloser is a subset of ReadWriteCloser, they are compatible.
// Both Read and Close are implemented.
fn ReadAndClose(f ReadCloser) { ... }
ReadAndClose(f) // valid because ReadCloser is a subset of the wider interface
```

## Enums

Enumerations are types of values. Each value is of the defined type but is a
unique value of that type. These values must be unique or an error is raised.

The values of an enum are structs that have both an index and name. If a name
or index is not explicitly provided, the compiler provides on based on its
position and constant. These values can be overridden but must be unique or an
error will be raised.

```
enum Colors {
  Red
  Green
  Blue
}

fn SelectColor(c Colors) Void { ... }

SelectColor(Colors.Red)
SelectColor(Colors.Purple) // error, invalid enumeration value

enum States {
  Stopped {name: "stop"} // names MUST be unique
  Running {name: "run"}
}

enum Suits {
  Clubs {index: 1}
  Diamonds // index is the previous value +1
  Hearts {index: 5} // indexes 3 and 4 are skipped, counting starts from here
  Spades // becomes 6
}
```

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

## Range

A range is an iterator type that represents a range of values. It takes two
basic expressions. Like slices, the first value is inclusive and the
second value is exclusive.

```
(0..10) // => 0, 1, 2, 3, 4, 5, 6, 7, 8, 9

for i : (0..10) {
  io.Println(i)
}
```

Only integer types and Char or Byte can be used.

It can be converted to an array of the same type with `.Array() [T]`

_Note: There is no "step" value at this time but may be added later._

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
condition-only, iteration, and range. For loops can be be advanced with 
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

Collections like arrays, maps, ranges, and string can be iterated over using
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

// ranges
range := (1..4)
for v : range {} // 1 => 2 => 3
for k, v : range {} // index, value form (0,1) => (1,2) => (2,3)

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
evens [Int] = for i : array |acc| { if i % 2 == 0 { acc.Push(i) } } // => [2, 4]

// Mapping
array := [1, 2, 3, 4]
doubles [Int] = for i : array |acc| { acc.Push(i * 2) } // => [2, 4, 6, 8]

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

-  6 Mar, 2025: Draft v0.3 (current)
- 25 Feb, 2025: Draft v0.2
-  7 Feb, 2025: Draft v0.1
