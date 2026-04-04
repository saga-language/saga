# Saga — VSCode Extension

Provides syntax highlighting, language configuration, and LSP integration for
the **Saga** programming language (`.sg` files).

---

## Features

| Feature | Details |
|---|---|
| Syntax highlighting | Keywords, types, literals, strings, interpolation, comments |
| String interpolation | `{expr}` inside strings highlighted as embedded Saga code |
| Multi-line strings | `"""…"""` triple-quoted blocks |
| Comment toggling | `//` via `Toggle Line Comment` (`Ctrl+/` / `Cmd+/`) |
| Bracket matching | `{}` `[]` `()` `""` |
| Auto-close / surround | All bracket and quote pairs |
| Code folding | Brace-delimited blocks |
| LSP (hover, completion, diagnostics) | Powered by `saga lsp` |

---

## Requirements

- **VSCode** 1.75 or newer
- **Saga compiler** on your `PATH` (for LSP features — syntax highlighting
  works without it)

---

## Installation

### From the Marketplace *(once published)*

Search for **Saga** in the Extensions panel (`Ctrl+Shift+X`) and click
**Install**.

### From source

```bash
cd editors/vscode
npm install
npm run compile

# Install the vsce packaging tool if you don't have it
npm install -g @vscode/vsce

# Build the .vsix
vsce package

# Install it
code --install-extension saga-language-0.1.0.vsix
```

### Development / sideloading (no build step)

1. Copy or symlink `editors/vscode/` into your VSCode extensions folder:

   ```bash
   # macOS / Linux
   ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/saga-language

   # Windows (PowerShell — run as Administrator)
   New-Item -ItemType Junction -Path "$env:USERPROFILE\.vscode\extensions\saga-language" `
             -Target "$PWD\editors\vscode"
   ```

2. Run `npm install && npm run compile` inside `editors/vscode/`.
3. Reload VSCode (`Developer: Reload Window`).

---

## LSP Integration

The extension automatically starts `saga lsp` (over stdio) whenever a `.sg`
file is opened.  No extra configuration is needed if `saga` is on your `PATH`.

### Custom compiler path

If the compiler lives elsewhere, point VSCode at it by adding this to your
`settings.json`:

```jsonc
// .vscode/settings.json  (workspace)  or  your user settings.json
{
  "saga.serverPath": "/usr/local/bin/saga"
}
```

*(requires a small update to `src/extension.ts` to read
`vscode.workspace.getConfiguration('saga').get('serverPath')` — see the
comment in that file)*

---

## What is highlighted

| Element | TextMate scope |
|---|---|
| Control keywords (`if`, `for`, `return`, …) | `keyword.control.saga` |
| Declaration keywords (`fn`, `struct`, `pub`, …) | `keyword.declaration.saga` |
| Keyword operators (`or`, `spawn`) | `keyword.operator.word.saga` |
| Built-in types (`Int`, `String`, `Void`, …) | `storage.type.saga` |
| Boolean literals (`true`, `false`) | `constant.language.saga` |
| Integer literals (dec / `0b` / `0x` / `0o`) | `constant.numeric.integer.*.saga` |
| Float literals | `constant.numeric.float.saga` |
| Single-line strings | `string.quoted.double.saga` |
| Multi-line strings | `string.quoted.triple.saga` |
| String escape sequences | `constant.character.escape.saga` |
| String interpolation `{expr}` | `meta.interpolation.saga` |
| Interpolation delimiters `{` `}` | `punctuation.section.interpolation.*.saga` |
| Comments (`//`) | `comment.line.double-slash.saga` |
| Operators | `keyword.operator.saga` |
