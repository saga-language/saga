// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT
//
// saga doc — local documentation server.
//
// Reads .sgi files from the stdlib, project build cache, and the package
// cache, then serves HTML documentation over HTTP on localhost.
//
// Usage:
//   saga doc               # serve on http://localhost:7070
//   saga doc --port 8080   # custom port
//   saga doc --open        # open browser after starting

#include "cmd/cmd.hpp"
#include "cmd/common.hpp"
#include "semantic/sgi.hpp"
#include "semantic/types.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SGI discovery
// ---------------------------------------------------------------------------

struct PackageDoc {
  std::string name;        // e.g. "io"
  std::string group;       // e.g. "std", "deps", "project"
  saga::SgiFile sgi;
};

// Scan a directory for .sgi files and parse them.
static void scan_sgi_dir(const std::string &dir,
                          const std::string &group,
                          std::vector<PackageDoc> &out) {
  std::error_code ec;
  for (auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".sgi") continue;
    auto sgi = saga::load_sgi(entry.path().string());
    if (!sgi) continue;
    PackageDoc pd;
    pd.name  = entry.path().stem().string();
    pd.group = group;
    pd.sgi   = std::move(*sgi);
    out.push_back(std::move(pd));
  }
}

// Collect all known packages from stdlib, project build, and package cache.
static std::vector<PackageDoc> discover_packages(const char *prog) {
  std::vector<PackageDoc> pkgs;

  // Stdlib.
  std::string std_sgi = saga_std_sgi_dir(prog);
  if (fs::is_directory(std_sgi))
    scan_sgi_dir(std_sgi, "std", pkgs);

  // Project .build/<name>/<name>.sgi
  auto manifest_path = saga::find_manifest(fs::current_path().string());
  if (manifest_path) {
    fs::path build_dir = fs::path(*manifest_path).parent_path() / ".build";
    std::error_code ec;
    for (auto &sub : fs::directory_iterator(build_dir, ec)) {
      if (!sub.is_directory()) continue;
      scan_sgi_dir(sub.path().string(), "project", pkgs);
    }
  }

  // Package cache: ~/.cache/saga/pkgs/<url>/<commit>/<name>.sgi
  const char *home = std::getenv("HOME");
  if (home) {
    fs::path cache = fs::path(home) / ".cache" / "saga" / "pkgs";
    std::error_code ec1;
    for (auto &url_entry : fs::directory_iterator(cache, ec1)) {
      if (!url_entry.is_directory()) continue;
      std::error_code ec2;
      for (auto &commit_entry : fs::directory_iterator(url_entry, ec2)) {
        if (!commit_entry.is_directory()) continue;
        scan_sgi_dir(commit_entry.path().string(), "deps", pkgs);
      }
    }
  }

  std::sort(pkgs.begin(), pkgs.end(),
            [](auto &a, auto &b) { return a.name < b.name; });
  return pkgs;
}

// ---------------------------------------------------------------------------
// HTML rendering helpers
// ---------------------------------------------------------------------------

static std::string html_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if      (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else               out += c;
  }
  return out;
}

// Split a doc comment into paragraphs at blank lines.
static std::string doc_to_html(const std::string &doc) {
  if (doc.empty()) return "<p><em>No documentation.</em></p>";
  // Each line of the doc comment is one source line; blank lines → <br>.
  std::string out = "<p>";
  bool was_blank = false;
  for (size_t i = 0; i < doc.size(); ) {
    size_t end = doc.find('\n', i);
    std::string line = doc.substr(i, end == std::string::npos ? doc.size() - i : end - i);
    i = end == std::string::npos ? doc.size() : end + 1;

    if (line.empty()) {
      if (!was_blank) { out += "</p><p>"; was_blank = true; }
    } else {
      out += html_escape(line);
      if (i < doc.size()) out += "<br>";
      was_blank = false;
    }
  }
  return out + "</p>";
}

static const char *CSS = R"(
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: system-ui, sans-serif; color: #222; background: #fafafa; }
nav { background: #1a1a2e; color: #e0e0e0; padding: 12px 24px;
      display: flex; align-items: center; gap: 16px; }
nav a { color: #90caf9; text-decoration: none; }
nav a:hover { text-decoration: underline; }
nav h1 { font-size: 1.1rem; color: #fff; }
.container { max-width: 860px; margin: 0 auto; padding: 24px 16px; }
h2 { margin: 24px 0 8px; font-size: 1.2rem; }
h3 { margin: 20px 0 4px; font-size: 1rem; color: #444; }
pre { background: #f0f0f0; border-radius: 4px; padding: 10px;
      font-size: 0.875rem; overflow-x: auto; }
code { font-family: "Cascadia Code", "Fira Code", monospace; font-size: 0.875rem; }
.symbol { border-left: 3px solid #1565c0; padding: 12px 16px;
          margin-bottom: 16px; background: #fff; border-radius: 0 4px 4px 0; }
.symbol code { color: #1565c0; }
.doc { color: #555; margin-top: 6px; font-size: 0.9rem; }
.tag { display: inline-block; font-size: 0.75rem; background: #e3f2fd;
       color: #0d47a1; border-radius: 12px; padding: 2px 8px; margin-right: 4px; }
.pkg-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px,1fr));
            gap: 12px; margin-top: 16px; }
.pkg-card { background: #fff; border: 1px solid #ddd; border-radius: 6px;
            padding: 14px; text-decoration: none; color: #222; display: block; }
.pkg-card:hover { border-color: #1565c0; }
.pkg-card .name { font-weight: 600; color: #1565c0; }
.pkg-card .count { font-size: 0.8rem; color: #888; margin-top: 4px; }
input[type=search] { width: 100%; padding: 8px 12px; border: 1px solid #ccc;
                     border-radius: 6px; font-size: 1rem; margin-bottom: 16px; }
</style>
)";

// ---------------------------------------------------------------------------
// Page generators
// ---------------------------------------------------------------------------

static std::string render_index(const std::vector<PackageDoc> &pkgs) {
  std::string body;
  body += "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"
          "<title>Saga Packages</title>";
  body += CSS;
  body += "</head><body>"
          "<nav><h1>⚡ Saga Docs</h1><a href='/'>Packages</a></nav>"
          "<div class='container'><h2>Packages</h2>";

  // Group by group name.
  for (auto &group : {"std", "project", "deps"}) {
    bool header = false;
    for (auto &pkg : pkgs) {
      if (pkg.group != group) continue;
      if (!header) {
        body += std::format("<h3>{}</h3><div class='pkg-list'>", group);
        header = true;
      }
      size_t count = pkg.sgi.exports.size();
      body += std::format(
          "<a class='pkg-card' href='/{}'>"
          "<div class='name'>{}</div>"
          "<div class='count'>{} exported symbol{}</div></a>",
          pkg.name, html_escape(pkg.name), count, count == 1 ? "" : "s");
    }
    if (header) body += "</div>";
  }
  body += "</div></body></html>";
  return body;
}

static std::string render_package(const PackageDoc &pkg) {
  std::string body;
  body += std::format(
      "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"
      "<title>{} — Saga Docs</title>",
      pkg.name);
  body += CSS;
  body += std::format(
      "</head><body>"
      "<nav><h1>⚡ Saga Docs</h1><a href='/'>Packages</a>"
      " / <strong>{}</strong> "
      "<span class='tag'>{}</span></nav>"
      "<div class='container'>",
      html_escape(pkg.name), html_escape(pkg.group));

  if (pkg.sgi.exports.empty()) {
    body += "<p><em>No exported symbols.</em></p>";
  } else {
    for (auto &exp : pkg.sgi.exports) {
      std::string type_str = saga::type_to_string(exp.type);
      body += "<div class='symbol'>"
              "<code>" + html_escape(exp.name) + " " +
              html_escape(type_str) + "</code>";
      if (!exp.doc.empty())
        body += "<div class='doc'>" + doc_to_html(exp.doc) + "</div>";
      body += "</div>";
    }
  }

  body += "</div></body></html>";
  return body;
}

static std::string render_search(const std::vector<PackageDoc> &pkgs,
                                  const std::string &query) {
  std::string body;
  body += "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"
          "<title>Search — Saga Docs</title>";
  body += CSS;
  body += "</head><body>"
          "<nav><h1>⚡ Saga Docs</h1><a href='/'>Packages</a></nav>"
          "<div class='container'>"
          "<form action='/search' method=get>"
          "<input type=search name=q placeholder='Search symbols…' value='";
  body += html_escape(query);
  body += "'></form>";

  if (!query.empty()) {
    body += std::format("<h3>Results for \"{}\"</h3>", html_escape(query));
    std::string q_lower = query;
    for (char &c : q_lower) c = static_cast<char>(std::tolower(c));

    bool found = false;
    for (auto &pkg : pkgs) {
      for (auto &exp : pkg.sgi.exports) {
        std::string name_lower = exp.name;
        for (char &c : name_lower) c = static_cast<char>(std::tolower(c));
        if (name_lower.find(q_lower) == std::string::npos) continue;
        found = true;
        body += "<div class='symbol'>"
                "<code><a href='/" + pkg.name + "'>" +
                html_escape(pkg.name) + "</a>." +
                html_escape(exp.name) + " " +
                html_escape(saga::type_to_string(exp.type)) + "</code>";
        if (!exp.doc.empty())
          body += "<div class='doc'>" + doc_to_html(exp.doc) + "</div>";
        body += "</div>";
      }
    }
    if (!found)
      body += "<p><em>No symbols match \"" + html_escape(query) + "\".</em></p>";
  }

  body += "</div></body></html>";
  return body;
}

static std::string render_404(const std::string &path) {
  return "<!DOCTYPE html><html><head><title>Not Found</title></head>"
         "<body><h1>404 Not Found</h1><p>No package: " +
         html_escape(path) + "</p><p><a href='/'>Back to index</a></p></body></html>";
}

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 server
// ---------------------------------------------------------------------------

struct HttpRequest {
  std::string method; // "GET", "POST", …
  std::string path;   // e.g. "/io"
  std::string query;  // e.g. "q=hello" (without leading ?)
};

// Parse the first line of an HTTP request ("GET /path?query HTTP/1.1").
static HttpRequest parse_request_line(const std::string &req) {
  HttpRequest r;
  size_t sp1 = req.find(' ');
  if (sp1 == std::string::npos) return r;
  r.method = req.substr(0, sp1);
  size_t sp2 = req.find(' ', sp1 + 1);
  std::string full_path = (sp2 == std::string::npos)
                              ? req.substr(sp1 + 1)
                              : req.substr(sp1 + 1, sp2 - sp1 - 1);
  auto qm = full_path.find('?');
  if (qm != std::string::npos) {
    r.query = full_path.substr(qm + 1);
    r.path  = full_path.substr(0, qm);
  } else {
    r.path = full_path;
  }
  return r;
}

// Minimal URL-decode for query string values (%xx → char, + → space).
static std::string url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') { out += ' '; continue; }
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(s[i + 1]), l = hex(s[i + 2]);
      if (h >= 0 && l >= 0) { out += static_cast<char>(h * 16 + l); i += 2; continue; }
    }
    out += s[i];
  }
  return out;
}

// Extract query parameter value (key=value&...).
static std::string query_param(const std::string &query,
                                const std::string &key) {
  std::string search = key + "=";
  size_t pos = query.find(search);
  if (pos == std::string::npos) return {};
  pos += search.size();
  size_t end = query.find('&', pos);
  return url_decode(query.substr(pos, end == std::string::npos ? query.size() - pos : end - pos));
}

// Send a complete HTTP response over a file descriptor.
static void send_http(int fd, int status_code, const std::string &status_text,
                       const std::string &content_type,
                       const std::string &body) {
  std::string header = std::format(
      "HTTP/1.1 {} {}\r\n"
      "Content-Type: {}; charset=utf-8\r\n"
      "Content-Length: {}\r\n"
      "Connection: close\r\n"
      "\r\n",
      status_code, status_text, content_type, body.size());
  write(fd, header.data(), header.size());
  write(fd, body.data(), body.size());
}

static void handle_connection(int fd, const std::vector<PackageDoc> &pkgs) {
  // Read request (stop at blank line).
  std::string raw;
  char buf[4096];
  while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    raw.append(buf, n);
    if (raw.find("\r\n\r\n") != std::string::npos ||
        raw.find("\n\n")     != std::string::npos)
      break;
  }

  // Parse first line.
  size_t first_end = raw.find('\n');
  if (first_end == std::string::npos) { close(fd); return; }
  std::string first_line = raw.substr(0, first_end);
  if (!first_line.empty() && first_line.back() == '\r')
    first_line.pop_back();

  HttpRequest req = parse_request_line(first_line);
  if (req.method != "GET") {
    send_http(fd, 405, "Method Not Allowed", "text/plain", "Method Not Allowed");
    close(fd);
    return;
  }

  std::string body;
  int status = 200;
  std::string status_text = "OK";

  if (req.path == "/" || req.path.empty()) {
    body = render_index(pkgs);
  } else if (req.path == "/search") {
    body = render_search(pkgs, query_param(req.query, "q"));
  } else {
    // Package page: /io → package name "io".
    std::string pkg_name = req.path.substr(1); // strip leading /
    auto it = std::find_if(pkgs.begin(), pkgs.end(),
                           [&](auto &p) { return p.name == pkg_name; });
    if (it == pkgs.end()) {
      body = render_404(req.path);
      status = 404;
      status_text = "Not Found";
    } else {
      body = render_package(*it);
    }
  }

  send_http(fd, status, status_text, "text/html", body);
  close(fd);
}

// ---------------------------------------------------------------------------
// cmd_doc
// ---------------------------------------------------------------------------

static void usage_doc() {
  std::cerr <<
    "Usage: saga doc [options]\n"
    "\n"
    "Serves Saga package documentation as HTML over HTTP.\n"
    "\n"
    "Options:\n"
    "  --port <n>   Port to listen on (default: 7070)\n"
    "  --open       Open the browser after starting\n"
    "  -v           Verbose output\n";
}

int cmd_doc(const char *prog, int argc, char **argv) {
  int port = 7070;
  bool open_browser = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc)     port = std::stoi(argv[++i]);
    else if (arg == "--open")                 open_browser = true;
    else if (arg == "-v")                     verbose = true;
    else if (arg == "--help" || arg == "-h") { usage_doc(); return 0; }
    else {
      std::cerr << std::format("saga doc: unknown option '{}'\n", arg);
      usage_doc();
      return 1;
    }
  }

  // Discover packages.
  auto pkgs = discover_packages(prog);
  if (verbose)
    std::cerr << std::format("Found {} package(s)\n", pkgs.size());

  // Create TCP socket.
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Error: cannot create socket\n";
    return 1;
  }
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = htons(static_cast<uint16_t>(port));

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::cerr << std::format("Error: cannot bind to port {}\n", port);
    close(server_fd);
    return 1;
  }
  listen(server_fd, 8);

  std::string url = std::format("http://localhost:{}", port);
  std::cerr << std::format("Serving Saga documentation at {}\n", url);
  std::cerr << "Press Ctrl+C to stop.\n";

  if (open_browser)
    std::system(std::format("xdg-open {} 2>/dev/null || open {} 2>/dev/null",
                             url, url).c_str());

  // Accept loop (single-threaded; one connection at a time).
  while (true) {
    sockaddr_in client{};
    socklen_t client_len = sizeof(client);
    int client_fd = accept(server_fd,
                           reinterpret_cast<sockaddr *>(&client), &client_len);
    if (client_fd < 0) continue;
    if (verbose)
      std::cerr << "Connection accepted\n";
    handle_connection(client_fd, pkgs);
  }

  close(server_fd);
  return 0;
}
