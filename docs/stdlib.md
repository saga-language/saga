# Standard Library

Everything in the "std" namespace. Exact naming and organization is still heavily in flux.

## Type Packages

Type packages provide methods on intrinsic types. They are compiled with the
`--stdlib` flag (which enables `intrinsic_*` calls) and bundled into
`libsaga_std.a`. Users do not import them directly — the compiler
automatically loads their `.sgi` interface files and links their object code.

| Package | Receiver type | Notes |
|---------|--------------|-------|
| `std/int` | `Int` | Converters, formatting, comparison |
| `std/float` | `Float` | Converters, formatting, comparison |
| `std/bool` | `Bool` | String, Equals |
| `std/string` | `String` | Size, case conversion, parsing, formatting |
| `std/array` | `[T]` | Generic — compiled once with type-erased `T` |
| `std/map` | `{K:V}` | Generic — compiled once with type-erased `K`, `V` |

### Intrinsics available in stdlib packages

These operations are **stdlib-only** — the analyzer rejects `intrinsic_*` calls
from user code. They provide the low-level building blocks that type packages
use to implement methods on intrinsic types.

- `intrinsic_runtime(name, args...)` — call a C runtime function by name
- `intrinsic_runtime_try(name, args...)` — call a C function that returns a
  status code; wraps the result into the function's declared union return type
- `intrinsic_field(value, index)` — GEP + load on a struct field
- `intrinsic_sitofp(value)` — LLVM `sitofp i64 → f64`
- `intrinsic_sitofp32(value)` — LLVM `sitofp i64 → f32`
- `intrinsic_fptosi(value)` — LLVM `fptosi f64 → i64`
- `intrinsic_fptrunc(value)` — LLVM `fptrunc f64 → f32`
- `intrinsic_fpext(value)` — LLVM `fpext f32 → f64`
- `intrinsic_sext(value, bits)` — LLVM `sext` to target bit width (signed)
- `intrinsic_zext(value, bits)` — LLVM `zext` to target bit width (unsigned)

## Application Packages

- crypto: typical cryptographic hashing algorithms, etc.
- db: database access/driver
- ffi: gives access to C and C++ bindings
- io: Input/Output io.print, etc; includes a Reader and Writer interface
  - fs: Filesystem
- json: Parsing/Generating JSON strings.
- log: Structured logging
- math: Abs(), Sqrt(), Log(), etc.
- net: general networking
  - http: HTTP specific web client, server, etc
- os: Exit, Args, env
  - signal: HUP, KILL, ABRT, etc.
- strconv: String conversion
- sys: Syscall
- template: templating language
- text: String manipulation (trim, split, join)
  - regex: Regular expressions
- time: Durations, timestamps.

_Note: the `ffi` package is only available when the `--allow-unsafe` flag is passed to the compiler._

## Crypto
## DB
## IO
Interfaces for input/output

```
pub interface Reader {
  Read() String | Error
}

pub interface Writer {
  Write(String) Int | Error
}

pub interface ReadWriter < Reader, Writer {}
```

## FFI
Foriegn function interface.

```
pub fn CpuCount() Int // Number of logical CPUs available
pub fn PageSize() Int // 
pub fn Platform() String // OS name ("linux", "darwin", "windows")
```
## JSON
## Log
## Math
## Net
### HTTP
## OS

Process and Control
```
pub fn Args() [String] // Command-line arguments
pub fn Env(String) String | Error // Environmental variables
pub fn Exit(Int) Void // Exit with status code
pub fn Pid() Int // Process ID assigned by OS
pub fn WorkingDir() String // Current working directory
```

Filesystem
```
pub Dir struct {}
pub File struct {}

pub const StdIn = File
pub const StdOut = File
pub const StdErr = File

pub fn Open(String) File | Error
pub fn Create(String) File | Error
pub fn Remove(String) File | Error
pub fn ReadDir(String) Dir | Error
```

## Path

Operates on paths, generally filesystem paths.
```
pub struct Path {}

pub fn Abs()
pub fn Base()
pub fn Ext() String
pub fn Join()
pub fn RealPath()
```
### Signal
## Strconv
## Sys
This package in inherently unsafe. It will likely be locked behind an `--unsafe` flag so that only packages have access to it, like the Standard Library.

It provides access to Syscall.

```
pub enum Operation {
  Read
}

pub fn SystemCall() Error
```

## Template
## Text
### Regexp
## Time

Time
```
pub fn Now() Int
pub fn Sleep(Int) Void // Suspend corouting/thread for duration
```

Duration
```
pub struct Duration {}
```
