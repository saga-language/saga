// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga lsp — Language Server Protocol server.
//
// Implements JSON-RPC 2.0 over stdin/stdout with Content-Length framing,
// as specified by the LSP 3.17 specification.
//
// Supported capabilities:
//   textDocument/hover        — type at cursor from node_types table
//   textDocument/definition   — go-to-definition via node_symbols table
//   textDocument/completion   — package-scope symbols + module members
//   textDocument/publishDiagnostics — analyzer errors (server-initiated)
//
// Architecture:
//   The analyzer runs in-process whenever a document is opened or changed.
//   Results are cached per URI and reused for hover/completion requests
//   until the next change.

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"
#include "util/json.hpp"

#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "semantic/analyzer.hpp"
#include "semantic/types.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Content-Length framing
// ---------------------------------------------------------------------------

// Read one JSON-RPC message from `in`.  Returns the JSON body or empty
// string on EOF/error.
static std::string read_message(std::istream &in) {
  size_t content_length = 0;
  std::string line;

  // Read headers until blank line.
  while (std::getline(in, line)) {
    // Strip \r
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break; // blank line separates headers from body
    const std::string prefix = "Content-Length: ";
    if (line.substr(0, prefix.size()) == prefix)
      content_length = std::stoull(line.substr(prefix.size()));
  }

  if (content_length == 0 || !in) return {};

  std::string body(content_length, '\0');
  in.read(body.data(), static_cast<std::streamsize>(content_length));
  if (!in) return {};
  return body;
}

// Write one JSON-RPC response to `out` with proper framing.
static void write_message(std::ostream &out, const std::string &body) {
  out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  out.flush();
}

// ---------------------------------------------------------------------------
// URI helpers
// ---------------------------------------------------------------------------

// Convert a file:// URI to a filesystem path.
// file:///home/user/foo.sg → /home/user/foo.sg
// Also percent-decodes %20 → space and %25 → %.
static std::string uri_to_path(const std::string &uri) {
  std::string path = uri;
  const std::string scheme = "file://";
  if (path.substr(0, scheme.size()) == scheme)
    path = path.substr(scheme.size());

  // Minimal percent-decode.
  std::string out;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      char hi = path[i + 1], lo = path[i + 2];
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(hi), l = hex(lo);
      if (h >= 0 && l >= 0) {
        out += static_cast<char>(h * 16 + l);
        i += 2;
        continue;
      }
    }
    out += path[i];
  }
  return out;
}

static std::string path_to_uri(const std::string &path) {
  return "file://" + path;
}

// ---------------------------------------------------------------------------
// Analysis result (one per open document)
// ---------------------------------------------------------------------------

struct DocumentState {
  std::string uri;
  std::string content;

  // Populated after analysis.
  saga::FileSet fileset;
  saga::ErrorList parse_errors;
  std::unique_ptr<saga::Node> ast;
  std::unique_ptr<saga::Analyzer> analyzer;
};

// ---------------------------------------------------------------------------
// AST node-at-offset finder
//
// Iterate all typed nodes and return the one with the smallest span
// that still contains `offset`.  Works for single-file documents because
// all spans are relative to that file's source (offset 0).
// ---------------------------------------------------------------------------

static const saga::Node *
find_node_at(const saga::Analyzer &az, size_t offset) {
  const saga::Node *best = nullptr;
  size_t best_len = SIZE_MAX;
  for (auto &[node, _] : az.node_types) {
    if (node->span.start <= offset && offset < node->span.end) {
      size_t len = node->span.end - node->span.start;
      if (len < best_len) {
        best_len = len;
        best = node;
      }
    }
  }
  return best;
}

// Check the auxiliary span_types vector for a match tighter than best_len.
// Returns the type if found, nullptr otherwise.
static saga::TypePtr
find_span_type_at(const saga::Analyzer &az, size_t offset, size_t best_len) {
  saga::TypePtr best = nullptr;
  for (auto &[span, type] : az.span_types) {
    if (span.start <= offset && offset < span.end) {
      size_t len = span.end - span.start;
      if (len < best_len) {
        best_len = len;
        best = type;
      }
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// LSP position ↔ byte-offset conversion
// ---------------------------------------------------------------------------

// LSP positions are 0-based (line, character).
// File::line_offsets[i] is the byte offset of the start of line i (0-based).
static size_t lsp_to_offset(const saga::File &file, int line, int character) {
  if (line < 0) return 0;
  size_t li = static_cast<size_t>(line);
  if (li >= file.line_offsets.size()) return file.source.size();
  size_t base = file.line_offsets[li];
  return base + static_cast<size_t>(character < 0 ? 0 : character);
}

// Byte offset → LSP {line, character} (0-based).
static std::pair<int, int> offset_to_lsp(const saga::File &file, size_t offset) {
  // Binary search: find the last line_offset <= offset.
  const auto &lo = file.line_offsets;
  size_t line = 0;
  for (size_t i = 0; i < lo.size(); ++i) {
    if (lo[i] <= offset) line = i;
    else break;
  }
  int character = static_cast<int>(offset - lo[line]);
  return {static_cast<int>(line), character};
}

// ---------------------------------------------------------------------------
// Analysis helper
// ---------------------------------------------------------------------------

static void run_analysis(DocumentState &doc, const char *prog) {
  doc.fileset = saga::FileSet{};
  doc.parse_errors = saga::ErrorList{};
  doc.ast.reset();
  doc.analyzer.reset();

  std::string path = uri_to_path(doc.uri);
  auto file = saga::File::from_source(path, doc.content);
  doc.fileset.add_file(std::move(file));

  saga::Parser parser(doc.fileset);
  doc.ast = parser.parse();
  doc.parse_errors = std::move(parser.errors);
  if (!doc.ast || !doc.parse_errors.errors.empty()) return;

  doc.analyzer = std::make_unique<saga::Analyzer>(doc.fileset);

  std::string dir = fs::path(path).parent_path().string();
  doc.analyzer->current_package_dir = dir;

  std::vector<std::string> sp, sgi;
  apply_manifest_deps(prog, dir, sp, sgi);
  for (auto &s : sp)
    doc.analyzer->package_resolver->search_paths.push_back(s);
  for (auto &s : sgi)
    doc.analyzer->package_resolver->sgi_search_paths.push_back(s);

  std::string std_sgi = saga_std_sgi_dir(prog);
  if (fs::is_directory(std_sgi))
    doc.analyzer->package_resolver->sgi_search_paths.push_back(std_sgi);

  try {
    doc.analyzer->analyze(*doc.ast);
  } catch (...) {
    // Gracefully handle unexpected analyzer crashes so the LSP server
    // stays alive.  Diagnostics from the partial run (if any) will
    // still be published below.
  }
}

// ---------------------------------------------------------------------------
// Diagnostic emission
// ---------------------------------------------------------------------------

static json::Value errors_to_diagnostics(const saga::FileSet &fileset,
                                          const saga::ErrorList &errors) {
  json::Value diags = json::make_array();
  for (auto &err : errors.errors) {
    // Position is 1-based; LSP wants 0-based.
    int line = static_cast<int>(err.p.line) - 1;
    int col  = static_cast<int>(err.p.column) - 1;
    if (line < 0) line = 0;
    if (col  < 0) col  = 0;

    json::Value diag = json::obj({
        {"range", json::obj({
            {"start", json::obj({{"line", line}, {"character", col}})},
            {"end",   json::obj({{"line", line}, {"character", col + 1}})},
        })},
        {"severity", 1}, // Error
        {"source",   "saga"},
        {"message",  err.message},
    });
    diags.push(std::move(diag));
  }
  return diags;
}

// ---------------------------------------------------------------------------
// LSP server state
// ---------------------------------------------------------------------------

struct LspServer {
  const char *prog;
  bool shutdown_requested = false;

  // open documents: URI → state
  std::unordered_map<std::string, DocumentState> documents;

  // ── JSON-RPC helpers ──────────────────────────────────────────────────

  void send(const json::Value &msg) {
    write_message(std::cout, msg.dump());
  }

  void send_response(const json::Value &id, const json::Value &result) {
    send(json::obj({
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"result",  result},
    }));
  }

  void send_error(const json::Value &id, int code, const std::string &message) {
    send(json::obj({
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"error",   json::obj({
            {"code",    code},
            {"message", message},
        })},
    }));
  }

  void send_notification(const std::string &method, const json::Value &params) {
    send(json::obj({
        {"jsonrpc", "2.0"},
        {"method",  method},
        {"params",  params},
    }));
  }

  // ── Publish diagnostics for a document ───────────────────────────────

  void publish_diagnostics(const std::string &uri, const DocumentState &doc) {
    json::Value diags = json::make_array();
    if (!doc.parse_errors.errors.empty()) {
      diags = errors_to_diagnostics(doc.fileset, doc.parse_errors);
    } else if (doc.analyzer) {
      diags = errors_to_diagnostics(doc.fileset, doc.analyzer->errors);
    }

    send_notification("textDocument/publishDiagnostics", json::obj({
        {"uri",         uri},
        {"diagnostics", std::move(diags)},
    }));
  }

  // ── Handlers ──────────────────────────────────────────────────────────

  void handle_initialize(const json::Value &id,
                          const json::Value & /*params*/) {
    send_response(id, json::obj({
        {"capabilities", json::obj({
            {"textDocumentSync", 1},            // Full sync
            {"hoverProvider",      true},
            {"definitionProvider", true},
            {"completionProvider", json::obj({
                {"triggerCharacters", json::arr({"."})},
            })},
        })},
        {"serverInfo", json::obj({
            {"name",    "saga-lsp"},
            {"version", "1.0"},
        })},
    }));
  }

  void handle_did_open(const json::Value &params) {
    auto *td = params.get("textDocument");
    if (!td) return;
    std::string uri     = td->str("uri");
    std::string content = td->str("text");

    auto &doc = documents[uri];
    doc.uri     = uri;
    doc.content = content;
    run_analysis(doc, prog);
    publish_diagnostics(uri, doc);
  }

  void handle_did_change(const json::Value &params) {
    auto *td = params.get("textDocument");
    if (!td) return;
    std::string uri = td->str("uri");

    // Full sync: take the last content change.
    auto *changes = params.get("contentChanges");
    if (!changes || !changes->is_array() || changes->size() == 0) return;
    std::string content = changes->at(changes->size() - 1).str("text");

    auto &doc   = documents[uri];
    doc.uri     = uri;
    doc.content = content;
    run_analysis(doc, prog);
    publish_diagnostics(uri, doc);
  }

  void handle_hover(const json::Value &id, const json::Value &params) {
    auto *td  = params.get("textDocument");
    auto *pos = params.get("position");
    if (!td || !pos) { send_response(id, json::Value{}); return; }

    std::string uri = td->str("uri");
    int line = static_cast<int>(pos->integer("line"));
    int col  = static_cast<int>(pos->integer("character"));

    auto it = documents.find(uri);
    if (it == documents.end() || !it->second.analyzer) {
      send_response(id, json::Value{});
      return;
    }
    auto &doc = it->second;
    auto &file = *doc.fileset.files[0];

    size_t offset = lsp_to_offset(file, line, col);
    const saga::Node *node = find_node_at(*doc.analyzer, offset);

    if (!node) { send_response(id, json::Value{}); return; }

    auto type_it = doc.analyzer->node_types.find(node);
    if (type_it == doc.analyzer->node_types.end()) {
      send_response(id, json::Value{}); return;
    }

    // Check span_types for a tighter match (e.g. struct-literal field names).
    size_t node_len = node->span.end - node->span.start;
    auto span_type = find_span_type_at(*doc.analyzer, offset, node_len);

    std::string type_str = saga::type_to_string(
        span_type ? span_type : type_it->second);

    // Include symbol name if available.
    std::string hover_text = "```saga\n" + type_str + "\n```";
    auto sym_it = doc.analyzer->node_symbols.find(node);
    if (sym_it != doc.analyzer->node_symbols.end() && !span_type) {
      hover_text = "```saga\n" + sym_it->second.name + ": " + type_str + "\n```";
    }

    auto [sl, sc] = offset_to_lsp(file, node->span.start);
    auto [el, ec] = offset_to_lsp(file, node->span.end);

    send_response(id, json::obj({
        {"contents", json::obj({
            {"kind",  "markdown"},
            {"value", hover_text},
        })},
        {"range", json::obj({
            {"start", json::obj({{"line", sl}, {"character", sc}})},
            {"end",   json::obj({{"line", el}, {"character", ec}})},
        })},
    }));
  }

  void handle_definition(const json::Value &id, const json::Value &params) {
    auto *td  = params.get("textDocument");
    auto *pos = params.get("position");
    if (!td || !pos) { send_response(id, json::Value{}); return; }

    std::string uri = td->str("uri");
    int line = static_cast<int>(pos->integer("line"));
    int col  = static_cast<int>(pos->integer("character"));

    auto it = documents.find(uri);
    if (it == documents.end() || !it->second.analyzer) {
      send_response(id, json::Value{}); return;
    }
    auto &doc = it->second;
    auto &file = *doc.fileset.files[0];

    size_t offset = lsp_to_offset(file, line, col);
    const saga::Node *node = find_node_at(*doc.analyzer, offset);
    if (!node) { send_response(id, json::Value{}); return; }

    auto sym_it = doc.analyzer->node_symbols.find(node);
    if (sym_it == doc.analyzer->node_symbols.end()) {
      send_response(id, json::Value{}); return;
    }
    auto &sym = sym_it->second;

    // decl_span of {0,0} means the symbol is a builtin with no source location.
    if (sym.decl_span.start == 0 && sym.decl_span.end == 0) {
      send_response(id, json::Value{}); return;
    }

    auto [sl, sc] = offset_to_lsp(file, sym.decl_span.start);
    auto [el, ec] = offset_to_lsp(file, sym.decl_span.end);

    send_response(id, json::obj({
        {"uri", uri},
        {"range", json::obj({
            {"start", json::obj({{"line", sl}, {"character", sc}})},
            {"end",   json::obj({{"line", el}, {"character", ec}})},
        })},
    }));
  }

  void handle_completion(const json::Value &id, const json::Value &params) {
    auto *td  = params.get("textDocument");
    auto *pos = params.get("position");
    if (!td || !pos) { send_response(id, json::make_array()); return; }

    std::string uri = td->str("uri");
    int line = static_cast<int>(pos->integer("line"));
    int col  = static_cast<int>(pos->integer("character"));

    auto it = documents.find(uri);
    if (it == documents.end() || !it->second.analyzer) {
      send_response(id, json::make_array()); return;
    }
    auto &doc = it->second;
    auto &file = *doc.fileset.files[0];

    // Check if this is a member access completion (text ends with '.').
    // Find the text of the current line up to the cursor.
    size_t offset = lsp_to_offset(file, line, col);
    size_t line_start = (line < (int)file.line_offsets.size())
                            ? file.line_offsets[line] : 0;
    std::string line_text;
    if (line_start < file.source.size() && offset >= line_start)
      line_text = file.source.substr(line_start, offset - line_start);

    json::Value items = json::make_array();

    // Member access: "moduleName." completion.
    if (!line_text.empty() && line_text.back() == '.') {
      // Find the module name immediately before the dot.
      size_t dot = line_text.size() - 1;
      size_t start = dot;
      while (start > 0 && (std::isalnum(line_text[start - 1]) || line_text[start - 1] == '_'))
        --start;
      std::string member_of = line_text.substr(start, dot - start);

      if (!member_of.empty() && doc.analyzer->package_scope_) {
        auto sym_it = doc.analyzer->package_scope_->symbols.find(member_of);
        if (sym_it != doc.analyzer->package_scope_->symbols.end()) {
          auto &sym = sym_it->second;
          if (sym.type && sym.type->kind == saga::TypeKind::Module) {
            auto &mod = std::get<saga::ModuleTypeInfo>(sym.type->detail);
            for (auto &exp : mod.exports) {
              // completionItem kind: 3=Function, 5=Field, 6=Variable
              int item_kind = exp.type && exp.type->kind == saga::TypeKind::Func ? 3 : 5;
              items.push(json::obj({
                  {"label",  exp.name},
                  {"kind",   item_kind},
                  {"detail", saga::type_to_string(exp.type)},
              }));
            }
          }
        }
      }
    } else {
      // Package-scope symbol completion.
      if (doc.analyzer->package_scope_) {
        for (auto &[name, sym] : doc.analyzer->package_scope_->symbols) {
          if (sym.is_builtin) continue;
          // completionItem kind: 3=Function, 6=Variable, 7=Class, 9=Module
          int item_kind = 6;
          if (sym.type) {
            switch (sym.type->kind) {
            case saga::TypeKind::Func:      item_kind = 3; break;
            case saga::TypeKind::Struct:    item_kind = 7; break;
            case saga::TypeKind::Interface: item_kind = 8; break;
            case saga::TypeKind::Enum:      item_kind = 13; break;
            case saga::TypeKind::Module:    item_kind = 9; break;
            default: break;
            }
          }
          items.push(json::obj({
              {"label",  name},
              {"kind",   item_kind},
              {"detail", sym.type ? saga::type_to_string(sym.type) : ""},
          }));
        }
      }
    }

    send_response(id, std::move(items));
  }

  void handle_shutdown(const json::Value &id) {
    shutdown_requested = true;
    send_response(id, json::Value{});
  }

  // ── Dispatch ──────────────────────────────────────────────────────────

  void dispatch(const json::Value &msg) {
    std::string method = msg.str("method");
    auto *id_ptr = msg.get("id");
    json::Value id = id_ptr ? *id_ptr : json::Value{};
    auto *params_ptr = msg.get("params");
    json::Value params = params_ptr ? *params_ptr : json::make_object();

    if (method == "initialize")              handle_initialize(id, params);
    else if (method == "initialized")        { /* no-op notification */ }
    else if (method == "shutdown")           handle_shutdown(id);
    else if (method == "exit")               { /* handled in run() */ }
    else if (method == "textDocument/didOpen")   handle_did_open(params);
    else if (method == "textDocument/didChange") handle_did_change(params);
    else if (method == "textDocument/hover")       handle_hover(id, params);
    else if (method == "textDocument/definition")   handle_definition(id, params);
    else if (method == "textDocument/completion") handle_completion(id, params);
    else if (!id.is_null()) {
      // Unknown request — return method-not-found error.
      send_error(id, -32601, "Method not found: " + method);
    }
    // Unknown notifications are silently ignored.
  }

  int run() {
    while (true) {
      std::string body = read_message(std::cin);
      if (body.empty()) break;

      json::Value msg = json::parse(body);
      if (msg.is_null()) continue;

      std::string method = msg.str("method");
      dispatch(msg);

      if (method == "exit")
        return shutdown_requested ? 0 : 1;
    }
    return 0;
  }
};

// ---------------------------------------------------------------------------
// cmd_lsp
// ---------------------------------------------------------------------------

static void usage_lsp() {
  std::cerr << "Usage: saga lsp\n"
               "\n"
               "Runs the Saga Language Server Protocol server on stdin/stdout.\n"
               "Configure your editor to launch 'saga lsp' as the language server\n"
               "for .sg files.\n";
}

int cmd_lsp(const char *prog, int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { usage_lsp(); return 0; }
    std::cerr << std::format("saga lsp: unknown option '{}'\n", arg);
    return 1;
  }

  // Switch stdout to binary mode (required on Windows; no-op on Unix).
  // Disable stdio buffering so responses are written immediately.
  std::cout.setf(std::ios::unitbuf);

  LspServer server;
  server.prog = prog;
  return server.run();
}
