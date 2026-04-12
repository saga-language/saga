# Benchmark and testing

- Have reasonable defaults.
- Only the most practical flags or subcommands
- Minimal colour: Green, Red, Grey
- A default Reporter that the community could extend or replace (interface)

## Dependencies

- flag package for getopt/getoptlong or Go-style

## Test

- Should allow running a single file
- Should allow running a single test within a file
- Should allow running all tests in a directory (recursive)
- Should allow running all tests (recursive)
- Run with a random seed by default
- Accept a --seed parameter
- Run in parallel by default
- Use Saga concurrency
- Accept a --max-concurrency type flag (name TBD) to reduce or disable 
- Should produce reasonable output, especially for debugging, but minimal and quiet.
- Automatic cleanup with Close()

The method name is the test name. Test names should be descriptive, like:

```
pub fn test_http_returns_404(t test.Case) { ... }
```

### Dependencies

- A `rand` package of some kind to generate a seed.

### Package

- Test doubles + implicit interfaces + dependency injection
- No mocks
- Fail without halting: t.Expect() or t.Fail() or t.FailIf()
- Fail and stop: t.Assert() or t.FailNow() or t.FailNowIf()
- Example Signature: t.Assert(cond Bool, desc String)
- Per-test, buffered logging: t.Log() or t.Debug() or both

```sg
pub struct Case {
  name String
  seed Int // Here? Or package global?
  logger Logger
  results []Failure

  pub fn Log(msg String) Void {
    logger.Log(msg)
  }

  pub fn Assert(cond Bool, desc String) Void {
    if !cond {
      failNow(desc)
    }
  }

  pub fn Expect(cond Bool, desc String) Void {
    if !cond {
      results.Append(desc)
    }
  }

  pub fn Close() Void {
    // Cleanup?
  }
}
```

### Diffs

- A pretty diff? Might be necessary for arrays and maps (JSON). Instead of pretty diffs, a straight print is a minimal start. AssertEqual()

```
// The context-aware assertion
pub fn |T| (c Case) AssertEqual(actual T, expected T, desc String) Void {
  if !actual.Equals(expected) {
    c.Log("FAIL: {desc}")
    c.Log("  Expected: {expected.String()}")
    c.Log("  Actual:   {actual.String()}")
    unsafe.intrinsic_trap("assertion failed")
  }
}
```

- To make the standard library extensible, and reduce the chance of fragmentation, it could either use embedded structs or interfaces.

### Failure capture

Leans into the Actor model. The test runner acts as a Supervisor. It spawns each `test_<method>` as an isolated Actor. If the `.Assert()` fails, it calls `intrinsic_trap()` or similar to gracefully kill the Actor and its memory arena. Since the runtime captures the failure reason in the Task before freeing, the test runner can extract each failure into a buffer for reporting.

```
// Inside std/testing
pub fn (c Case) Assert(cond Bool, desc String) Void {
  if !cond {
    c.Log(desc)
    // Kill this test's actor immediately. 
    // The test runner supervisor will catch the actor death.
    unsafe.intrinsic_trap("assertion failed") 
  }
}
```

## Bench

- **Speed:** ns/op (nanoseconds per operation)
- **Memory:** B/op (bytes per operation)
- **Allocations:** allocs/op (allocations per operation)

The Supervisor (the test runner) executes bench_<method> multiple times, increasing
b.N each time, until the function runs long enough to get a stable, reliable timing average.

```
// std/test
pub struct Bench {
  pub N Int // Number of iterations to run
  
  pub fn Reset() Void // reset timer, allocation/memory counts
  pub fn Start() Void // start benchmarking
  pub fn End() Void // end benchmarking
}
```

```
// std/time
// There's a few ways this API could take, like returning timestamps when calling
// Start and End, or End returning a time.Duration.
pub struct Timer {
  pub fn Reset() Void
  pub fn Start() Void
  pub fn End() Void
}
```

Example user code
```
pub fn bench_string_concat(b test.Bench) {
  // Setup (potentially expensive) goes here.
  base := "Hello, "
  add := "World!"

  // Reset the timer to start fresh
  b.Reset()

  // Iterate N times through the code
  for i : (0..b.N) {
    result := base + addition
  }
}

### Dependencies

Still a WIP but potential dependency packages:

- std/time: Timer, Duration, etc
- std/runtime: Memory stats (alloc size and count)
