// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "semantic/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// SGI — Saga Interface Files (.sgi)
//
// A compact, human-readable, versioned description of a package's public API.
// Used for fast import resolution, LSP hover docs, and documentation
// generation without requiring source files.
//
// Format (version 1):
//
//   sgi 1
//   package <name>
//   import <name> "<path>"
//
//   // Doc comment for Foo.
//   func Foo(a Int, b String) Int
//
//   // Doc comment for Bar.
//   struct Bar {
//     pub x Int
//     pub fn Method(s String) Void
//   }
//
//   enum Color { Red; Green; Blue }
//
//   interface Readable {
//     fn Read(buf [Byte]) Int
//   }
//
//   const MaxSize Int
//
// ---------------------------------------------------------------------------

/// A single exported symbol with optional documentation.
struct SgiExport {
  std::string doc;   // doc comment (lines joined with \n, no leading //)
  std::string name;
  TypePtr type;
  bool is_type = false;  // true for struct/enum/interface TYPE exports
                         // false for func/const/variable VALUE exports
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
  int version = 1;
  std::string package_name;
  std::vector<SgiImport> imports;
  std::vector<SgiExport> exports;
  std::vector<SgiReceiverMethod> receiver_methods; // stdlib intrinsic methods
};

// ---------------------------------------------------------------------------
// Writer — serialize a package's public API to .sgi text.
// ---------------------------------------------------------------------------

/// Generate .sgi content from a list of exports.
/// `package_name` is the short name (e.g. "io").
/// `imports` records which packages this package depends on.
/// `exports` is the list of public symbols to serialize.
/// `receiver_methods` is the list of intrinsic receiver methods (stdlib only).
std::string generate_sgi(const std::string &package_name,
                          const std::vector<SgiImport> &imports,
                          const std::vector<SgiExport> &exports,
                          const std::vector<SgiReceiverMethod> &receiver_methods = {});

/// Write a .sgi file to disk. Returns true on success.
bool write_sgi(const std::string &path,
               const std::string &package_name,
               const std::vector<SgiImport> &imports,
               const std::vector<SgiExport> &exports,
               const std::vector<SgiReceiverMethod> &receiver_methods = {});

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

// ---------------------------------------------------------------------------
// Type serialization helpers (also used by the writer/reader)
// ---------------------------------------------------------------------------

/// Serialize a type to its .sgi text representation.
std::string type_to_sgi(const TypePtr &t);

/// Parse a type from its .sgi text representation.
/// Returns nullptr on parse failure.
TypePtr sgi_to_type(const std::string &text);

} // namespace mc
