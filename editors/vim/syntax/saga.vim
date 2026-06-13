" Saga language syntax file
" Language: Saga (.sg)
" Maintainer: saga project
"
" Ordering note: at the same start position the last-defined Match/Region wins
" (:syn-priority), so general/short items come first and specific/long ones last.

if exists('b:current_syntax')
  finish
endif

" ── Built-in types ────────────────────────────────────────────────────────────

syntax keyword sagaType
      \ bool byte string void
      \ int int8 int16 int32 int64
      \ uint uint8 uint16 uint32 uint64
      \ float float32 float64
      \ array map

" ── Type-shapes & aliases ─────────────────────────────────────────────────────

" struct/enum/interface introduce a type name; error is a bare expression type.
syntax keyword sagaStructure  struct enum interface  nextgroup=sagaTypeName skipwhite
syntax keyword sagaStructure  error
syntax keyword sagaTypedef    type  nextgroup=sagaTypeName skipwhite

" ── Keywords ──────────────────────────────────────────────────────────────────

syntax keyword sagaKeyword       fn const or spawn
syntax keyword sagaInclude       import
syntax keyword sagaStorageClass  pub extern
syntax keyword sagaConditional   if else switch case
syntax keyword sagaRepeat        for
syntax keyword sagaStatement     break next return
syntax keyword sagaOperatorWord  is

" ── Literals ──────────────────────────────────────────────────────────────────

syntax keyword sagaBoolean  true false
syntax keyword sagaNull     null

" ── Identifiers ───────────────────────────────────────────────────────────────

" A name directly before '(' (optionally with a generic arg list) is a def or
" call. Keywords outrank matches, so if/for/switch/spawn are never caught here.
syntax match sagaFunction  "\<\h\w*\ze\%(<[^<>]*>\)\?("

" Type name following struct/enum/interface/type (reached only via nextgroup).
syntax match sagaTypeName  "\<\h\w*\>"  contained

" ── Comments ──────────────────────────────────────────────────────────────────

syntax region sagaComment start="//" end="$" oneline keepend contains=@Spell

" ── Numeric literals ──────────────────────────────────────────────────────────

syntax match sagaInteger  "\<\d[0-9_]*\>"
syntax match sagaInteger  "\<0[bB][01_]\+\>"
syntax match sagaInteger  "\<0[xX][0-9a-fA-F_]\+\>"
syntax match sagaInteger  "\<0[oO][0-7_]\+\>"

" Floats last so they win over the decimal-integer match on the leading digits.
syntax match sagaFloat  "\<\d[0-9_]*\.[0-9][0-9_]*\%([eE][+-]\?\d[0-9_]*\)\?\>"

" ── Strings ───────────────────────────────────────────────────────────────────

syntax match sagaStringEscape  "\\[ntr\\\"{}]" contained

" Interpolation { expr }: keepend stops a nested string eating the outer ";
" exclude sagaTypeName so bare identifiers here aren't mistaken for type names.
syntax region sagaInterpolation
      \ matchgroup=sagaInterpolationDelim
      \ start="{"  end="}"
      \ contained keepend extend
      \ contains=ALLBUT,sagaString,sagaMultilineString,sagaInterpolation,sagaTypeName

" Single-line first, triple-quoted last so """ wins as one token.
syntax region sagaString
      \ start='"'  skip='\\"'  end='"'
      \ contains=sagaStringEscape,sagaInterpolation
      \ keepend oneline

syntax region sagaMultilineString
      \ start='"""'  end='"""'
      \ contains=sagaStringEscape,sagaInterpolation
      \ keepend

" ── Operators ─────────────────────────────────────────────────────────────────

" Single-char first, multi-char last so the longer operator wins.
syntax match sagaOperator  "[-+*/%!<>&|^~=]"
syntax match sagaOperator
      \ "\.\.\.\|:=\|+=\|-=\|\*=\|/=\|==\|!=\|<=\|>=\|&&\|||\|\*\*\|<<\|>>\|++\|--\|\.\."
" Safe-access '?' in x?.f / x?[i] / x?() — leaves the trailing token alone.
syntax match sagaOperator  "?\ze[.[(]"

" ── Default highlight links ───────────────────────────────────────────────────

highlight default link sagaType               Type
highlight default link sagaTypeName           Type
highlight default link sagaStructure          Structure
highlight default link sagaTypedef            Typedef
highlight default link sagaKeyword            Keyword
highlight default link sagaInclude            Include
highlight default link sagaStorageClass       StorageClass
highlight default link sagaConditional        Conditional
highlight default link sagaRepeat             Repeat
highlight default link sagaStatement          Statement
highlight default link sagaOperatorWord       Operator
highlight default link sagaFunction           Function
highlight default link sagaBoolean            Boolean
highlight default link sagaNull               Constant
highlight default link sagaComment            Comment
highlight default link sagaOperator           Operator
highlight default link sagaInteger            Number
highlight default link sagaFloat              Float
highlight default link sagaString             String
highlight default link sagaMultilineString    String
highlight default link sagaStringEscape       SpecialChar
highlight default link sagaInterpolationDelim  PreProc
highlight default link sagaInterpolation      Normal

let b:current_syntax = 'saga'
