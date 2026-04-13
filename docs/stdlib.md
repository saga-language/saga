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
- net: general networking
  - http: HTTP specific web client, server, etc
- io: Input/Output io.print, etc; includes a Reader and Writer interface
  - fs: Filesystem
- json: Parsing/Generating JSON strings.
- log: Structured logging
- math: Abs(), Sqrt(), Log(), etc.
- os: Exit, Args, env
  - signal: HUP, KILL, ABRT, etc.
- template: templating language
- text: String manipulation (trim, split, join)
  - regex: Regular expressions
- time: Durations, timestamps.
- ffi: gives access to C and C++ bindings

_Note: the `ffi` package is only available when the `--allow-unsafe` flag is passed to the compiler._
