// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga mcp — Model Context Protocol documentation server.
//
// Implements MCP 2024-11-05 over stdio (newline-delimited JSON-RPC 2.0).
// Provides AI tools with structured access to Saga package documentation
// sourced from .sgi interface files.
//
// Transport: one JSON object per line on stdin/stdout (no Content-Length).
//
// Tools exposed:
//   list_packages    — enumerate all known packages with symbol counts
//   get_package_docs — full .sgi content for a named package
//   get_symbol_docs  — doc comment + signature for a specific symbol
//   search_docs      — full-text search across all package exports

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"
#include "semantic/sgi.hpp"
#include "semantic/types.hpp"
#include "util/json.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Package discovery (shared logic with doc.cpp)
// ---------------------------------------------------------------------------

struct McpPackage {
  std::string name;
  std::string group; // "std" | "project" | "deps"
  mc::SgiFile sgi;
};

static void scan_sgi_dir_mcp(const std::string &dir,
                               const std::string &group,
                               std::vector<McpPackage> &out) {
  std::error_code ec;
  for (auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".sgi") continue;
    auto sgi = mc::load_sgi(entry.path().string());
    if (!sgi) continue;
    McpPackage p;
    p.name  = entry.path().stem().string();
    p.group = group;
    p.sgi   = std::move(*sgi);
    out.push_back(std::move(p));
  }
}

static std::vector<McpPackage> discover_packages_mcp(const char *prog) {
  std::vector<McpPackage> pkgs;

  std::string std_sgi = saga_std_sgi_dir(prog);
  if (fs::is_directory(std_sgi))
    scan_sgi_dir_mcp(std_sgi, "std", pkgs);

  auto manifest_path = mc::find_manifest(fs::current_path().string());
  if (manifest_path) {
    fs::path build_dir = fs::path(*manifest_path).parent_path() / ".build";
    std::error_code ec;
    for (auto &sub : fs::directory_iterator(build_dir, ec)) {
      if (!sub.is_directory()) continue;
      scan_sgi_dir_mcp(sub.path().string(), "project", pkgs);
    }
  }

  const char *home = std::getenv("HOME");
  if (home) {
    fs::path cache = fs::path(home) / ".cache" / "saga" / "pkgs";
    std::error_code ec1;
    for (auto &url_e : fs::directory_iterator(cache, ec1)) {
      if (!url_e.is_directory()) continue;
      std::error_code ec2;
      for (auto &commit_e : fs::directory_iterator(url_e, ec2)) {
        if (!commit_e.is_directory()) continue;
        scan_sgi_dir_mcp(commit_e.path().string(), "deps", pkgs);
      }
    }
  }

  return pkgs;
}

// ---------------------------------------------------------------------------
// MCP transport (newline-delimited JSON over stdio)
// ---------------------------------------------------------------------------

static bool send_mcp(const json::Value &msg) {
  std::string line = msg.dump() + "\n";
  std::cout << line;
  std::cout.flush();
  return true;
}

static void send_result(const json::Value &id, const json::Value &result) {
  send_mcp(json::obj({
      {"jsonrpc", "2.0"},
      {"id",      id},
      {"result",  result},
  }));
}

static void send_error(const json::Value &id, int code,
                        const std::string &message) {
  send_mcp(json::obj({
      {"jsonrpc", "2.0"},
      {"id",      id},
      {"error",   json::obj({
          {"code",    code},
          {"message", message},
      })},
  }));
}

// Wrap a plain-text result in the MCP content envelope.
static json::Value text_content(const std::string &text) {
  return json::obj({
      {"content", json::arr({
          json::obj({{"type", "text"}, {"value", text}}),
      })},
  });
}

// ---------------------------------------------------------------------------
// Tool schema helpers
// ---------------------------------------------------------------------------

static json::Value string_prop(const std::string &desc) {
  return json::obj({{"type", "string"}, {"description", desc}});
}

static json::Value make_tool(const std::string &name,
                              const std::string &desc,
                              json::Value props,
                              json::Value required) {
  return json::obj({
      {"name",        name},
      {"description", desc},
      {"inputSchema", json::obj({
          {"type",       "object"},
          {"properties", std::move(props)},
          {"required",   std::move(required)},
      })},
  });
}

// ---------------------------------------------------------------------------
// Tool list
// ---------------------------------------------------------------------------

static json::Value build_tool_list() {
  json::Value tools = json::make_array();

  tools.push(make_tool(
      "list_packages",
      "List all Saga packages with their exported symbol counts.",
      json::make_object(),
      json::arr({})));

  tools.push(make_tool(
      "get_package_docs",
      "Get full documentation for a Saga package (all exported symbols "
      "with their doc comments and type signatures).",
      json::obj({{"package", string_prop("Package name, e.g. \"io\" or \"os\"")}}),
      json::arr({"package"})));

  tools.push(make_tool(
      "get_symbol_docs",
      "Get the doc comment and type signature for a specific exported symbol.",
      json::obj({
          {"package", string_prop("Package name, e.g. \"io\"")},
          {"symbol",  string_prop("Exported symbol name, e.g. \"Println\"")},
      }),
      json::arr({"package", "symbol"})));

  tools.push(make_tool(
      "search_docs",
      "Search for exported symbols by name across all known packages.",
      json::obj({{"query", string_prop("Search term (case-insensitive substring)")}}),
      json::arr({"query"})));

  return json::obj({{"tools", std::move(tools)}});
}

// ---------------------------------------------------------------------------
// Tool implementations
// ---------------------------------------------------------------------------

static json::Value tool_list_packages(const std::vector<McpPackage> &pkgs) {
  std::string out;
  for (auto &pkg : pkgs) {
    out += std::format("[{}] {} — {} exported symbol{}\n",
                       pkg.group, pkg.name,
                       pkg.sgi.exports.size(),
                       pkg.sgi.exports.size() == 1 ? "" : "s");
  }
  if (out.empty()) out = "No packages found.\n";
  return text_content(out);
}

static json::Value tool_get_package_docs(const std::vector<McpPackage> &pkgs,
                                          const std::string &pkg_name) {
  for (auto &pkg : pkgs) {
    if (pkg.name != pkg_name) continue;

    std::string out;
    out += std::format("package {}\n\n", pkg.name);

    if (pkg.sgi.exports.empty()) {
      out += "No exported symbols.\n";
    } else {
      for (auto &exp : pkg.sgi.exports) {
        out += "---\n";
        if (!exp.doc.empty())
          out += exp.doc + "\n";
        out += std::format("{} {}\n", exp.name, mc::type_to_string(exp.type));
      }
    }
    return text_content(out);
  }
  return text_content(std::format("Package '{}' not found.\n", pkg_name));
}

static json::Value tool_get_symbol_docs(const std::vector<McpPackage> &pkgs,
                                         const std::string &pkg_name,
                                         const std::string &sym_name) {
  for (auto &pkg : pkgs) {
    if (pkg.name != pkg_name) continue;
    for (auto &exp : pkg.sgi.exports) {
      if (exp.name != sym_name) continue;
      std::string out;
      if (!exp.doc.empty())
        out += exp.doc + "\n\n";
      out += std::format("{}::{} {}\n", pkg.name, exp.name,
                          mc::type_to_string(exp.type));
      return text_content(out);
    }
    return text_content(
        std::format("Symbol '{}' not found in package '{}'.\n",
                    sym_name, pkg_name));
  }
  return text_content(std::format("Package '{}' not found.\n", pkg_name));
}

static json::Value tool_search_docs(const std::vector<McpPackage> &pkgs,
                                     const std::string &query) {
  std::string q = query;
  for (char &c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::string out;
  int count = 0;

  for (auto &pkg : pkgs) {
    for (auto &exp : pkg.sgi.exports) {
      std::string name_lower = exp.name;
      for (char &c : name_lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name_lower.find(q) == std::string::npos) continue;

      out += std::format("{}::{} {}\n", pkg.name, exp.name,
                          mc::type_to_string(exp.type));
      if (!exp.doc.empty()) {
        // Show only the first line of the doc comment.
        size_t nl = exp.doc.find('\n');
        out += "  " + exp.doc.substr(0, nl) + "\n";
      }
      ++count;
    }
  }

  if (count == 0)
    out = std::format("No symbols matching '{}'.\n", query);
  else
    out = std::format("{} result{}:\n\n", count, count == 1 ? "" : "s") + out;

  return text_content(out);
}

// ---------------------------------------------------------------------------
// MCP server
// ---------------------------------------------------------------------------

struct McpServer {
  const char *prog;
  std::vector<McpPackage> pkgs;
  bool initialized = false;

  void handle(const json::Value &msg) {
    std::string method = msg.str("method");
    auto *id_ptr = msg.get("id");
    json::Value id = id_ptr ? *id_ptr : json::Value{};
    auto *params_ptr = msg.get("params");
    json::Value params = params_ptr ? *params_ptr : json::make_object();

    if (method == "initialize") {
      initialized = true;
      send_result(id, json::obj({
          {"protocolVersion", "2024-11-05"},
          {"capabilities",    json::obj({{"tools", json::make_object()}})},
          {"serverInfo",      json::obj({
              {"name",    "saga-mcp"},
              {"version", "1.0"},
          })},
      }));
      return;
    }

    if (method == "notifications/initialized") return; // no-op

    if (method == "tools/list") {
      send_result(id, build_tool_list());
      return;
    }

    if (method == "tools/call") {
      std::string name;
      auto *name_ptr = params.get("name");
      if (name_ptr && name_ptr->is_string()) name = name_ptr->as_string();

      auto *args_ptr = params.get("arguments");
      json::Value args = args_ptr ? *args_ptr : json::make_object();

      json::Value result;
      if (name == "list_packages") {
        result = tool_list_packages(pkgs);
      } else if (name == "get_package_docs") {
        result = tool_get_package_docs(pkgs, args.str("package"));
      } else if (name == "get_symbol_docs") {
        result = tool_get_symbol_docs(pkgs, args.str("package"), args.str("symbol"));
      } else if (name == "search_docs") {
        result = tool_search_docs(pkgs, args.str("query"));
      } else {
        send_error(id, -32602, "Unknown tool: " + name);
        return;
      }
      send_result(id, std::move(result));
      return;
    }

    // Unknown notification: ignore.  Unknown request: method-not-found.
    if (!id.is_null())
      send_error(id, -32601, "Method not found: " + method);
  }

  int run() {
    // Discover packages once at startup.
    pkgs = discover_packages_mcp(prog);

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      json::Value msg = json::parse(line);
      if (msg.is_null()) continue;
      handle(msg);
    }
    return 0;
  }
};

// ---------------------------------------------------------------------------
// cmd_mcp
// ---------------------------------------------------------------------------

static void usage_mcp() {
  std::cerr <<
    "Usage: saga mcp\n"
    "\n"
    "Runs the Saga MCP (Model Context Protocol) documentation server on\n"
    "stdin/stdout. Configure your AI tool to launch 'saga mcp' as an MCP\n"
    "server to give it access to Saga package documentation.\n"
    "\n"
    "Exposed tools:\n"
    "  list_packages     List all available packages\n"
    "  get_package_docs  Get documentation for a package\n"
    "  get_symbol_docs   Get documentation for a specific symbol\n"
    "  search_docs       Search for symbols by name\n";
}

int cmd_mcp(const char *prog, int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { usage_mcp(); return 0; }
    std::cerr << std::format("saga mcp: unknown option '{}'\n", arg);
    return 1;
  }

  std::cout.setf(std::ios::unitbuf);

  McpServer server;
  server.prog = prog;
  return server.run();
}
