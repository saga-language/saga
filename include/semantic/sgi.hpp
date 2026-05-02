// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace saga {

// ---------------------------------------------------------------------------
// SGI — Saga Interface Files (.sgi)
//
// A compact, human-readable, versioned description of a package's public API.
// Used for fast import resolution, LSP hover docs, and documentation
// generation without requiring source files.
//
// Format (version 2):
//
//   sgi 2
//   package <name>
//   import <name> "<path>"
//
//   // Doc comment for Foo.
//   @origin "pkg"
//   func Foo(a Int, b String) Int
//
//   // Doc comment for Bar.
//   @origin "pkg"
//   struct Bar |T#0| {
//     pub x T
//     pub fn |U#1| Method(other U, s String) Void
//   }
//
//   @origin "pkg"
//   enum Color { Red; Green; Blue }
//
//   @origin "pkg"
//   interface Readable {
//     fn Read(buf [Byte]) Int
//   }
//
//   @origin "pkg"
//   const MaxSize Int
//
// Notable changes from v1:
//   - Version bumped from 1 to 2; readers MUST reject v1 (regenerate).
//   - Each export may carry an `@origin "pkg"` line. The origin is the
//     package that originally defined the type/value; this lets nominal
//     types carry their `origin_package` across compilation units so
//     (origin, name) equality is preserved.
//   - Generic type parameters declared in a struct/interface header use
//     the `|T#id[, U#id]|` form where `id` is stable within the file.
//     Method-local generics are declared the same way: `pub fn |T#id|
//     Method(...)`. Bare references (`T`) are resolved through a lexical
//     scope maintained by the parser.
// ---------------------------------------------------------------------------

/// A single exported symbol with optional documentation.
struct SgiExport {
  std::string doc;           // doc comment (lines joined with \n, no leading //)
  std::string name;
  TypePtr type;
  bool is_type = false;      // true for struct/enum/interface TYPE exports
                             // false for func/const/variable VALUE exports
  std::string origin_path;   // originating package (empty = enclosing file's
                             // `package_name`)
};

/// A dependency on another package (recorded so that type references
/// to foreign types can be resolved).
struct SgiImport {
  std::string name;        // local name (e.g. "os")
  std::string import_path; // full path (e.g. "std/os")
};

/// Receiver methods bound to an intrinsic type in a stdlib package.
struct SgiReceiverMethod {
  std::string type_name; // "Int", "Float", "Bool", "String"
  std::vector<MethodInfo> methods;
};

/// Parsed contents of a .sgi file.
struct SgiFile {
  int version = 2;
  std::string package_name;
  /// Absolute path to the directory containing the package's .sg sources.
  /// Empty if not recorded (e.g. older SGI or stdlib without a registered
  /// path). Used by the importer's codegen to lazily parse generic method
  /// bodies that the origin package could not pre-instantiate.
  std::string source_dir;
  std::vector<SgiImport> imports;
  std::vector<SgiExport> exports;
  std::vector<SgiReceiverMethod> receiver_methods; // stdlib intrinsic methods
};

/// Current SGI format version. Readers reject other versions outright.
constexpr int kSgiVersion = 2;

// ---------------------------------------------------------------------------
// Writer — serialize a package's public API to .sgi text.
// ---------------------------------------------------------------------------

/// Generate .sgi content from a list of exports.
/// `package_name` is the short name (e.g. "io").
/// `imports` records which packages this package depends on.
/// `exports` is the list of public symbols to serialize.
/// `receiver_methods` is the list of intrinsic receiver methods (stdlib only).
/// `source_dir` is the absolute path to the package's source directory; when
/// non-empty, the importer can use it to load generic method bodies that
/// could not be pre-instantiated.
std::string generate_sgi(const std::string &package_name,
                          const std::vector<SgiImport> &imports,
                          const std::vector<SgiExport> &exports,
                          const std::vector<SgiReceiverMethod> &receiver_methods = {},
                          const std::string &source_dir = "");

/// Write a .sgi file to disk. Returns true on success.
bool write_sgi(const std::string &path,
               const std::string &package_name,
               const std::vector<SgiImport> &imports,
               const std::vector<SgiExport> &exports,
               const std::vector<SgiReceiverMethod> &receiver_methods = {},
               const std::string &source_dir = "");

// ---------------------------------------------------------------------------
// Reader — parse a .sgi file back into a ModuleType.
// ---------------------------------------------------------------------------

/// Parse .sgi text content into an SgiFile structure.
/// Returns std::nullopt on parse error.
std::optional<SgiFile> parse_sgi(const std::string &content);

/// Load and parse a .sgi file from disk.
/// Returns std::nullopt if the file can't be read or parsed.
std::optional<SgiFile> load_sgi(const std::string &path);

/// Convert a parsed SgiFile into a ModuleType suitable for import resolution.
/// `import_path` is the full import path (e.g. "std/io").
TypePtr sgi_to_module_type(const SgiFile &sgi, const std::string &import_path);

/// True when the package declares a constant whose value must be allocated at
/// program start (Array or Map at the top level).  Drives whether the linker
/// must call `<pkg>__init__` from the binary's main wrapper.
bool sgi_needs_init(const SgiFile &sgi);

// ---------------------------------------------------------------------------
// Type serialization helpers (also used by the writer/reader)
// ---------------------------------------------------------------------------

/// Serialize a type to its .sgi text representation.
std::string type_to_sgi(const TypePtr &t);

/// Parse a type from its .sgi text representation.
/// Returns nullptr on parse failure.
TypePtr sgi_to_type(const std::string &text);

} // namespace saga
