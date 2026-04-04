" Saga language ftplugin
if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

let b:undo_ftplugin = 'setl cms< sw< et< sua<'

setlocal commentstring=//\ %s
setlocal shiftwidth=2
setlocal expandtab
setlocal suffixesadd=.sg
