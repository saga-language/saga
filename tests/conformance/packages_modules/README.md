# Packages/Modules — coverage gap

The claims in `docs/language.md` §Packages/Modules (lines 177-184) are
inherently multi-file:

- "A directory forms the scope and name of a package."
- "Sub-directories are their own package and must be imported separately."
- "All files in a directory are part of that package."
- "Only constants marked as `pub`lic are visible outside the package."
- "Any file within the package has unrestricted access to private
  Constants."

The conformance runner (`run_test.sh`) builds a single `.sg` file, so
none of these can be exercised here without a runner extension.

The `tests/runtime/cross_package/` suite covers many of these cases
already (cross-package consts, structs, methods, embedding, generics)
but is feature-driven rather than spec-claim-mapped. When the
conformance runner gains directory-mode support, this section should
be backfilled with focused tests for each of the five claims above.

See also `tests/conformance/visibility/README.md` for the same gap.
