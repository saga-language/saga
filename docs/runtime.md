# Runtime

These are the core runtime intrinsics of the language. Without them, the
language simply doesn't function. It can be parsed and have code generated
but the intrinsics must be linked into the final binary to not have linking
errors due to missing symbols.

## General

`[]` accessor syntax calls `At()`. When used with assignment, it calls `Set()`.  
The compiler can optimize any calls to avoid indirection.

All types must implement the following methods:

`.Compare(other T) Comparison`: `<`, `>`, `<=`, and `>=`
`.Equals(other T) Bool`: `==` and `!=`
`.String() String`: Pretty print the type

The parser can differentiate (and optimize) for the three types of "calls"
that a user can make: function, procedure, and method.

`FunctionDecl`: No receiver, returns a value; can be optimized for tail-call
`ProcedureDecl`: No receiver, returns Void.
`MethodDecl`: Has a receiver.

## Internal

These are only available when the `--allow-unsafe` flag is passed to the compiler.

`intrinsic_alloc(size Int) Any`: Request heap memory
`intrinsic_copy(src, dst Any, len Int) Void`: Raw mem copy
`intrinsic_panic(String) Void`: Unrecoverable, kills program
`intrinsic_print(String) Void`: Print to stdout
`intrinsic_syscall(id Int, args ...Any) Int | Error`: OS syscall
`|T| intrinsic_sizeof() Int`: Byte size of the type
`|T| intrinsic_typeid() Int`: Unique ID for each type
`intrinsic_yield() Void`: Release a hardware thread and return an actor the back of the queue. Resets the reduction counter.
`|T| intrinsic_atomic_add(ptr *T, val T) T`: Thread-safe Arena memory bypassing for fetch-and-add.
`intrinsic_trap(reason String) Void`: Force actor into a "zombie" state.
 
## Enums

```
enum Comparison {
  Less
  Equal
  Greater
}
```

## Interfaces

### Error

```
interface Error {
  Message() String
}
```

### Iterator

```
interface |T| Iterable {
  Next() T | Error
}
```

## Types

|Identifier|Type|Notes|
|---|---|---|
|Bool|boolean||
|Byte|raw byte|alias to Uint8|
|Char|utf-8 character|???|
|Float|float|alias to system word size|
|Float32|float|32 bit floating point|
|Float64|float|64 bit floating point|
|Int|integer|alias to system word size|
|Int8|integer|signed 8 bit integer|
|Int16|integer|signed 16 bit integer|
|Int32|integer|signed 32 bit integer|
|Int64|integer|signed 64 bit integer|
|Uint8|integer|unsigned 8 bit integer|
|Uint16|integer|unsigned 16 bit integer|
|Uint32|integer|unsigned 32 bit integer|
|Uint64|integer|unsigned 64 bit integer|
|Void|void|no value|

All types implement the following methods:

`.String() String`: string representation of the value

### Array

`.At(Int) T`: element at index
`.Find(T) Int | Missing`: get the index of value
`.Insert(T, Int)`: insert element into index
`.Push(T) [T]`: push a new element
`.Pop() T`: pop element
`.Set(Int,T) Void`: set the element at index to value
`.Size() Int`: length of the string

### Integers

Each of the integer types need the following methods:

**Arithmetic**
`.Add(T) T`: `+`
`.Div(T) T | Error`: `/`
`.Mod(T) T`: `%`
`.Mul(T) T`: `*`
`.Pow(T) T`: `**`
`.Sub(T) T`: `-`

**Logical**
`.And(T) T`: `&&`
`.Not(T) T`: `!`
`.Or(T) T`: `||`

**Bitwise**
`.BitAnd(T) T`: `&`
`.BitOr(T) T`: `|`
`.BitXor(T) T`: `^`
`.BitShiftLeft(T) T`: `<<`
`.BitShiftRight(T) T`: `>>`

**Converters**
`.Char() Char`
`.Format() String`
`.Int() Int`
`.Int8() Int8`
`.Int16() Int16`
`.Int32() Int32`
`.Int64() Int64`
`.UInt8() UInt8`
`.UInt16() UInt16`
`.UInt32() UInt32`
`.UInt64() UInt64`

Converters are lossy.

### Float

Each of the float types need the following methods:

`.Add(T) T`: `+`
`.Div(T) T | Error`: `/`
`.Mod(T) T`: `%`
`.Mul(T) T`: `*`
`.Sub(T) T`: `-`

**Converters**
`.Format() String`
`.Float32() Float32`
`.Float64() Float64`

### Map

`[]` accessor syntax calls `At()`. The compiler can optimize any calls to avoid
indirection.

`.At(K) V` // gets value at key
`.Key?(K) Bool` // gets value at key
`.Keys() [K]` // gets value at key
`.Remove(K) Void` // gets value at key
`.Size() Int` // gets value at key
`.Set(K, V) Void`: set the element at key to value

### Missing

Implements the Error interface and represents missing fields in a struct or
reaching the end of a loop.

```
struct Missing {
  fn Message() String { "Missing data or field" }
}
```

### Task

This struct is returned from a spawn expression to the parent thread.

`.Alive?() Bool`: Checks if the coroutine is running.
`.Cancel() Void`: Sets the `.Cancelled?()` flag on the context.
`.Term() Void`: Immediately kills the thread, use with caution.
`.Wait() T | Error`: Blocks until completion, returns the `Exit()` value.

### Context

This struct is available inside a spawn block.

`.Cancelled?() Bool`: Polling method to see if the parent called `.Cancel()`
`.Exit(T) Void`: Terminates the coroutine and sets the return value
`.Send(T) Void`: Non blocking push into the task channel

### String

Strings can be sliced but they're immutable.

`.Bytes() [Byte]`: Zero copy converstion of the String into byte array
`.Count() Int`: Number UTF-8 Runes in the string
`.Float() Float | Error`: convert to a Float
`.Format(String) String`: format the string
`.Int() Int | Error`: convert to an Int
`.Lower() String`: Transforms the string into all lowercase
`.Runes() [Int32]`: Returns an array of UTF-8 points
`.Size() Int`: Returns the number of Bytes in the string
`.Upper() String`: Transforms the string into all uppercase

### Compare

An enum with the types: Less, Equal, Greater.
