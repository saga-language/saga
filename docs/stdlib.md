# Standard Library

Everything in the "std" namespace. Exact naming and organization is still heavily in flux.

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
