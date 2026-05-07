# Visibility — note on coverage

The `pub` rule has two halves:
1. **In-file**: marking parses, declarations are still readable
   in-file regardless of `pub` — covered by the two `.sg` files here.
2. **Cross-file/cross-package**: a private symbol must NOT be reachable
   from another package, while a `pub` symbol must be — *not* covered
   here, because the conformance runner (`run_test.sh`) builds a single
   `.sg` file. A multi-package fixture (lib + app dirs, like
   `tests/runtime/cross_package/`) is needed.

The same gap applies to:
- `tests/conformance/import/` (when added)
- `tests/conformance/packages_modules/` (when added)
- some claims in `tests/conformance/initialisation/` (cross-package
  init order)

Follow-up: extend the conformance runner to handle directory-based
test cases, then backfill these sections with cross-package tests.
