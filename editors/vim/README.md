# Saga — Vim / Neovim Plugin

Provides syntax highlighting, filetype detection, indentation, and LSP
integration for the **Saga** programming language (`.sg` files).

---

## Files

| Path | Purpose |
|---|---|
| `ftdetect/saga.vim` | Detect `*.sg` files as `filetype=saga` |
| `ftplugin/saga.vim` | Per-buffer settings (`commentstring`, `shiftwidth`, …) |
| `syntax/saga.vim` | Keyword / literal / string / comment highlighting |
| `indent/saga.vim` | Brace-based automatic indentation |

---

## Installation

### [vim-plug](https://github.com/junegunn/vim-plug)

```vim
Plug 'your-org/saga', { 'rtp': 'editors/vim' }
```

### [lazy.nvim](https://github.com/folke/lazy.nvim)

```lua
{
  dir = '/path/to/saga',
  ft  = 'saga',
  config = function()
    -- nothing extra required; rtp is set automatically when dir is used
  end,
}
```

### Manual (Vim 8+ native packages)

```bash
# Vim
mkdir -p ~/.vim/pack/saga/start/saga
cp -r editors/vim/* ~/.vim/pack/saga/start/saga/

# Neovim
mkdir -p ~/.local/share/nvim/site/pack/saga/start/saga
cp -r editors/vim/* ~/.local/share/nvim/site/pack/saga/start/saga/
```

### [Packer](https://github.com/wbthomason/packer.nvim)

```lua
use { 'your-org/saga', rtp = 'editors/vim' }
```

---

## LSP Integration (Neovim)

The Saga compiler exposes an LSP server via `saga lsp`.  
Add the following to your Neovim config (requires
[nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) is **not** needed —
this uses the built-in `vim.lsp.start` API available since Neovim 0.8):

```lua
vim.api.nvim_create_autocmd('FileType', {
  pattern  = 'saga',
  callback = function()
    vim.lsp.start({
      name = 'saga',
      cmd  = { 'saga', 'lsp' },
      root_dir = vim.fs.dirname(
        vim.fs.find({ '*.sg' }, { upward = true })[1]
      ),
    })
  end,
})
```

This gives you:

- Hover (`K`) — type info and documentation
- Go-to-definition (`gd`)
- Completion (triggered automatically or via `<C-x><C-o>`)
- Inline diagnostics (errors and warnings)

### Via nvim-lspconfig (alternative)

If you prefer managing servers through nvim-lspconfig you can register a
custom server entry:

```lua
local lspconfig = require('lspconfig')
local configs   = require('lspconfig.configs')

if not configs.saga then
  configs.saga = {
    default_config = {
      cmd          = { 'saga', 'lsp' },
      filetypes    = { 'saga' },
      root_dir     = lspconfig.util.root_pattern('*.sg'),
      settings     = {},
    },
  }
end

lspconfig.saga.setup({})
```

---

## What is highlighted

| Element | Vim highlight group |
|---|---|
| Keywords (`fn`, `if`, `for`, …) | `Keyword` |
| Keyword operators (`or`, `spawn`) | `Keyword` |
| Built-in types (`Int`, `String`, …) | `Type` |
| Booleans (`true`, `false`) | `Boolean` |
| Integer literals (dec / `0b` / `0x` / `0o`) | `Number` |
| Float literals | `Float` |
| String literals (single-line & triple-quoted) | `String` |
| String escape sequences | `SpecialChar` |
| String interpolation `{expr}` delimiters | `PreProc` |
| Comments (`//`) | `Comment` |
| Operators | `Operator` |
