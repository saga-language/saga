# Spec/Implementation Drift

Deferred:
- `T[]` type-erased storage (perf footnote, not correctness). Currently
  all `T[]` storage uses i64 slots; small element types (Int8/16/32,
  Float32, Bool, Char) waste space and cache locality.  Fix shape:
  monomorphize `T[]` for primitive element types, keep the type-erased
  path for arbitrary `T`.  Decision 2026-05-06: defer.
- Re-test interface composition under the new syntax (per
  `ai/slimmer-type-system-proposal.md`).  When `interface X < Y, Z {}`
  lands, port the deleted `subset_assignable` scenario: a value of the
  composed interface should be assignable to either constituent
  interface and dispatch correctly through it.  Same canonical pattern,
  in the syntax that's actually going to ship.
