" Saga language indent file
" Language: Saga (.sg)

if exists('b:did_indent')
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetSagaIndent(v:lnum)
" Re-indent on: new {, closing }, ), ], and normal o/O/e inserts
setlocal indentkeys=0{,0},0),0],!^F,o,O,e

let b:undo_indent = 'setl inde< indk<'

if exists('*GetSagaIndent')
  finish
endif

function! GetSagaIndent(lnum) abort
  " Find the previous non-blank line
  let prev_lnum = prevnonblank(a:lnum - 1)
  if prev_lnum == 0
    return 0
  endif

  let ind      = indent(prev_lnum)
  let prevline = getline(prev_lnum)
  let thisline = getline(a:lnum)

  " Increase indent when the previous line opens a block with '{'
  if prevline =~# '{\s*$'
    let ind += shiftwidth()
  endif

  " Decrease indent when this line starts with a closing brace
  if thisline =~# '^\s*[}\])]'
    let ind -= shiftwidth()
  endif

  return max([ind, 0])
endfunction
