# Spec/Implementation Drift

Open:
- **A nominal alias of a struct reads as empty** (found 2026-08-15). Every
  field access through one yields nothing, silently — no error, no crash:

  ```
  struct Point { x int  y int }
  type Coord Point

  c := Coord{x: 3, y: 4}
  io.Println("{c.x} {c.y}")   // prints an empty line
  ```

  Codegen's `semantic_type` unwraps *structural* aliases only, so
  `struct_lvalue` and `emit_selector` both see `TypeKind::Alias`, fail their
  `kind == Struct` test, and fall through to returning null. The analyzer
  resolves the fields fine — it unwraps nominal aliases too — so nothing is
  reported. Fix shape: give codegen the same unwrap at the struct-shaped
  entry points, and check `struct_cache_key` agrees on the underlying
  struct's key. Destructuring already unwraps (`emit_destructure`), so
  `{x, y} := c` binds — with the same empty values.

Deferred:
- `T[]` type-erased storage (perf footnote, not correctness). Currently
  all `T[]` storage uses i64 slots; small element types (int8/16/32,
  float32, bool, byte) waste space and cache locality.  Fix shape:
  monomorphize `T[]` for primitive element types, keep the type-erased
  path for arbitrary `T`.  Decision 2026-05-06: defer.
- Re-test interface composition under the new syntax (per
  `ai/slimmer-type-system-proposal.md`).  When `interface X < Y, Z {}`
  lands, port the deleted `subset_assignable` scenario: a value of the
  composed interface should be assignable to either constituent
  interface and dispatch correctly through it.  Same canonical pattern,
  in the syntax that's actually going to ship.
