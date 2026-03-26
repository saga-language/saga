# Tools

The language encourages the use of its core tools. One program with multiple sub-commands.

## Compiler

Compiles or runs source code.

## Test harness

Run tests.

## Formatter

Opinionated formatter.

## Language server

Language server protocol.

## MCP docs server

Serves the documentation for the standard library and any downloaded packages.

## Package Manager

Manages the downloading and versions of external packages.

## Command Line Tool

This is a more detailed exploration of the tools and their implementation.

**Commands:**
- check: Compile files that have changed to .ll files. Maybe into a build directory.
- run: Use the JIT to run the program. In-memory only compilation.
- build: Build an actual binary. 
  - Use a user-cache directory for incremental building (.cache/mycc, or .mycc in project root).
  - Always use reasonable defaults
  - Debugging symbols by default; pass --no-debug to disable
  - Optimize to O0 by default; pass --o-max for O3
  - Allow disabling IR verification (or maybe having it enabled it just for development?
  - Task performance tuning (see Concurrency Model)
- clean: Delete all incremental build artifacts in cache for the project.
- test: Run test framework.
- mcp: Run documentation MCP.
- get: Download and install or update a package
  - With no arguments, download and install packages from the manifest
  - With an argument, install or update a package
    - The basename (tail) of the package is used as an alias
    - `--alias` allows setting an alternative name in case of conflict
    - The user can edit the manifest to set their own alias.
  - If the package is a library, it is placed in the [dependencies] section
  - If the package is a binary, it is placed in the [tools] section
  - Tools are downloaded, built, installed into `.cache/mycc/hash`.
  - Symlink to the compiled binary to `<project>/.mycc/bin`
  - The compiler tool knows about and executes these commands.
  - --global could be used to install global binaries or packages
  - Searches project local, then global
- init: Initialize a new project.
  - `--type` binary | library (default: binary)
- vendor: Copy package source into a vendor directory of project
- lsp: Run language server protocol
- env: Language/Tool environment information
- bench: Run benchmarks
- doc: Run local document server (probably combine with MCP

Use BLAKE3 or SHA256 hashing for exact versions.
Cache package files into the project or language cache (.cache./mycc/pkgs).
Cached packages are stored by their hash. That way, two projects can either have different versions of the same package (different hashes) or use the exact same package on disk (no duplication).
Use direct package paths. It should be provider agnostic. (github.com/user/package)
The package file is the manifest. The tool updates the manifest directly.

# Manifest File
- No separate lockfile.
- Packages are kept in alphabetical order.
- When a branch or tag is provided, the tool resolves the commit SHA and generates the content HASH.
- The content HASH protects against supply-chain attacks and version re-tagging.
- Users can point a package to a local copy, allowing them to edit it. This sets the path field.
- It treats the URL as a Git path, prepending `https://` and potentially appending `.git`; if its a vanity URL, looks for a `meta` tag to resolve the real address.

```toml
# project.saga
[package]
name = "my_app"
kind = “binary” # default; or “library”
description = “very short description”
license = “mit”

[dependencies]
# User typed: "github.com/std/http @ main"
# Tool resolved and locked it:
http = { 
    url = "github.com/std/http", 
    commit = "a1b2c3d4e5f6...", 
    hash = "sha256-ABC123XYZ...", 
    branch = "main" 
}

# User typed: "my_local_lib @ ../lib"
# Tool recognizes local path and bypasses hash
my_local_lib = { path = "../lib" }

# These are installed into the project cache as a binary tool
[tools]
author/something = { 
    url = "github.com/author/something_or_other", 
    commit = "a1b2c3d4e5f6...", 
    hash = "sha256-ABC123XYZ...", 
    branch = "main" 
}
```
