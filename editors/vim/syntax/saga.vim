" Saga language syntax file
" Language: Saga (.sg)
" Maintainer: saga project

if exists('b:current_syntax')
  finish
endif

" ── Keywords ──────────────────────────────────────────────────────────────────

" Control-flow keywords
syntax keyword sagaKeyword  break case else for if next return switch

" Declaration keywords
syntax keyword sagaKeyword  const enum fn import interface pub struct

" Keyword operators (used as expression-level operators in the grammar)
syntax keyword sagaKeywordOperator  or spawn

" ── Built-in types ────────────────────────────────────────────────────────────

syntax keyword sagaType
      \ Bool Byte Int Float String
      \ Float32 Float64
      \ Int8 Int16 Int32 Int64
      \ Uint8 Uint16 Uint32 Uint64
      \ Void

" ── Boolean literals ──────────────────────────────────────────────────────────

syntax keyword sagaBoolean  true false

" ── Comments ──────────────────────────────────────────────────────────────────

syntax region sagaComment start="//" end="$" oneline keepend contains=@Spell

" ── Numeric literals ──────────────────────────────────────────────────────────

" Floats — must come before integers so the dot is consumed
syntax match sagaFloat  "\<\d[0-9_]*\.[0-9][0-9_]*\%([eE][+-]\?\d[0-9_]*\)\?\>"

" Integers: decimal, binary (0b), hexadecimal (0x), octal (0o)
syntax match sagaInteger  "\<\d[0-9_]*\>"
syntax match sagaInteger  "\<0[bB][01_]\+\>"
syntax match sagaInteger  "\<0[xX][0-9a-fA-F_]\+\>"
syntax match sagaInteger  "\<0[oO][0-7_]\+\>"

" ── Strings ───────────────────────────────────────────────────────────────────

" Escape sequences (contained)
syntax match sagaStringEscape  "\\[ntr\\\"{}]" contained

" Interpolation region: { expr } inside a string.
" 'extend' lets the interpolation reach past what the parent might see as its
" end, and 'keepend' prevents a nested string from eating the outer closing ".
" We allow most top-level items inside interpolation but exclude sagaString to
" avoid the outer string being terminated by a " inside the interpolated expr.
syntax region sagaInterpolation
      \ matchgroup=sagaInterpolationDelim
      \ start="{"  end="}"
      \ contained keepend extend
      \ contains=ALLBUT,sagaString,sagaMultilineString,sagaInterpolation

" Multi-line strings (triple-quoted) — defined BEFORE single-line so """
" is matched greedily as one token.
syntax region sagaMultilineString
      \ start='"""'  end='"""'
      \ contains=sagaStringEscape,sagaInterpolation
      \ keepend

" Single-line strings
syntax region sagaString
      \ start='"'  skip='\\"'  end='"'
      \ contains=sagaStringEscape,sagaInterpolation
      \ keepend oneline

" ── Operators ─────────────────────────────────────────────────────────────────

syntax match sagaOperator  ":="
syntax match sagaOperator  "+="
syntax match sagaOperator  "-="
syntax match sagaOperator  "\*="
syntax match sagaOperator  "/="
syntax match sagaOperator  "=="
syntax match sagaOperator  "!="
syntax match sagaOperator  "<="
syntax match sagaOperator  ">="
syntax match sagaOperator  "&&"
syntax match sagaOperator  "||"
syntax match sagaOperator  "\*\*"
syntax match sagaOperator  "\.\."
syntax match sagaOperator  "\.\.\."
syntax match sagaOperator  "[-+*/%!<>&|^~=]"

" ── Default highlight links ───────────────────────────────────────────────────

highlight default link sagaKeyword          Keyword
highlight default link sagaKeywordOperator  Keyword
highlight default link sagaType             Type
highlight default link sagaBoolean          Boolean
highlight default link sagaComment          Comment
highlight default link sagaOperator         Operator
highlight default link sagaInteger          Number
highlight default link sagaFloat            Float
highlight default link sagaString           String
highlight default link sagaMultilineString  String
highlight default link sagaStringEscape     SpecialChar
highlight default link sagaInterpolationDelim  PreProc
highlight default link sagaInterpolation    Normal

let b:current_syntax = 'saga'
