```ebnf
/*
Go EBNF Syntax

Declinition = production_name "=" [ Expression ] ";" ;
Expression  = Term { "|" Term } ;
Factor      = production_name | token [ "..." token ] | Group | Option | Repetition ;
Group       = "(" Expression ")" ;
Option      = "[" Expression "]" ;
Repetition  = "{" Expression "}" ;

PascalCase productions are non-terminal, snake_case productions are terminal.
*/

Source      = { Declaration } ;
Declaration = [ public ] ( ConstDecl | TypeDecl | EnumDecl | ErrorDecl
                         | FuncDecl | ImportDecl | InterfaceDecl | StructDecl ) ;

/* Declarations */

ConstDecl  = "const" Identifier [ Type ] "=" Expression ;

ImportDecl = "import" [ Identifier ] StringLiteral ; // import Math "std/math"

TypeDecl   = "type" Identifier ( Type | "=" Type ) ;

/* Enum */
EnumDecl    = "enum" Identifier [ "string" ]
              "{" EnumField { terminal EnumField } "}" ;
EnumField   = Identifier [ "=" Expression ] ; // explicit ordinal or backing value

/* Error */
ErrorDecl   = "error" Identifier "{" { terminal ErrorMember } "}" ;
ErrorMember = "message" "=" StringLiteral
            | [ public ] FieldSpec ;

/* Interface */

InterfaceDecl   = "interface" Identifier [ Generic ]
                  "{" InterfaceMember { terminal InterfaceMember } "}" ;
InterfaceMember = EmbeddedName | MethodSig ;
MethodSig       = [ public ] Identifier Signature ;

/* Struct */
StructDecl   = "struct" Identifier [ Generic ]
               "{" { StructMember terminal } "}" ;
StructMember = EmbeddedName | [ public ] FieldSpec ;
FieldSpec    = IdentifierList Type [ "=" Expression ] ;
EmbeddedName = Identifier | Selector ; // embed by (qualified) type name

/* Functions / methods */
FuncDecl   = "fn" ( Receiver Identifier
                  | Identifier [ "." Identifier ] )
             [ Generic ] Signature [ Block ] ;
Receiver   = "(" ( Identifier | ignored_identifier ) Type ")" ;
Signature  = "(" [ ParameterList ] ")" [ Type ] ; // single optional return type
ParameterList = Parameter { "," Parameter } ;
Parameter     = IdentifierList ParameterType ;
ParameterType = Type | VariadicType ;
VariadicType  = "..." Type ;

/* Generics */

Generic    = "<" TypeParam { "," TypeParam } ">" ;
TypeParam  = Identifier [ Constraint ] ;
Constraint = "integer" | "float" | "numeric" | Identifier | Selector ;

/* Types */

Type        = UnionType ;
UnionType   = SingleType { "|" SingleType } ;
SingleType  = basic_type | ArrayType | MapType | StructType | EnumType
            | ErrorType | FuncType | GenericApp | Selector | Identifier ;

basic_type  = "bool" | "byte" | "string" | "void"
            | "int" | "int8" | "int16" | "int32" | "int64"
            | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
            | "float" | "float32" | "float64" ;

ArrayType   = "array" "{" Type [ ";" Expression ] "}" ;
MapType     = "map" "{" Type ":" Type [ ";" Expression ] "}" ;
StructType  = "struct" [ "{" [ FieldSpec { "," FieldSpec } ] "}" ] ;
EnumType    = "enum" [ "{" Identifier { "," Identifier } "}" ] ;
ErrorType   = "error" [ "{" [ ErrorMember { "," ErrorMember } ] "}" ] ;
FuncType    = "fn" "(" [ TypeList ] ")" [ Type ] ;
GenericApp  = ( Identifier | Selector ) "<" Type { "," Type } ">" ; // e.g. Box<int>
TypeList    = Type { "," Type } ;

/* Expressions */

// Precedence (highest to lowest)
//   1. Access:    . [] ()  and the safe forms ?. ?[] ?()
//   2. Unary:     ! - ~
//   3. Power:     **                 (right-associative)
//   4. Multiply:  * / %
//   5. Add:       + -
//   6. Bitwise:   & | ^ << >>
//   7. Compare:   == != > < >= <=
//   8. Logical:   && ||              (lazy / short-circuit)
//   9. Type test: is
//  10. Or:        or
//
// Precedence is enforced by the parser; the productions below are flat (an
// operator grammar) and defer to the table above, matching the implementation.
Expression  = LogicalExpression [ OrClause ] ;
OrClause    = "or" [ Pipe ] Block ;
LogicalExpression = UnaryExpr | BinaryExpr | IsExpr | PrimaryExpr ;
BinaryExpr  = Expression binary_operator Expression ;
IsExpr      = Expression "is" Type ;                    // non-exhaustive narrowing
UnaryExpr   = unary_operator Expression ;

PrimaryExpr = Operand { Access } ;
Operand     = Literal | Identifier | IfExpr | SwitchExpr | ForExpr
            | SpawnExpr | FuncExpr | "(" Expression ")" ;

Access      = [ "?" ] ( "." Identifier | IndexExpr | CallArgs ) ;
IndexExpr   = "[" ( Expression | Slice ) "]" ;
CallArgs    = "(" [ ExpressionList ] ")" ;
ExpressionList = Expression { "," Expression } ;

Slice       = [ Expression ] ".." [ Expression ] ;
Selector    = ( Identifier | Selector ) "." Identifier ; // qualified name: pkg.Type

/* Conditionals */

InitClause  = Identifier ":=" Expression
            | Identifier Type "=" Expression ;

IfExpr      = "if" [ InitClause ";" ] Expression Block [ "else" ( IfExpr | Block ) ] ;

SwitchExpr  = "switch" [ InitClause ";" ] Expression
              "{" { CaseArm } [ ElseArm ] "}" ;
CaseArm     = "case" ExpressionList ":" terminal ArmBody ;
ElseArm     = "else" ":" terminal ArmBody ;
ArmBody     = { ( Expression | Statement ) terminal } ;

/* For */

// Forms: infinite, condition, C-style iterator, for-each, and range.  An
// optional accumulator pipe `|acc T = e|` makes `for` an expression whose
// value is the accumulator (type inferred from the LHS or the initializer).
ForExpr       = "for" [ ForMode ] [ AccumulatorPipe ] Block ;
ForMode       = Expression                              // condition
              | IteratorClause                          // C-style
              | RangeClause ;                           // for-each or range form
IteratorClause = Identifier ":=" Expression ";" Expression ";" Assignment ;
RangeClause   = IdentifierList ":" RangeSubject ;
RangeSubject  = Expression [ ".." Expression ] ;        // collection, or `0..10`
AccumulatorPipe = "|" Identifier [ Type ] [ "=" Expression ] "|" ;

/* Spawn */
SpawnExpr   = "spawn" [ "<" Type [ "," Type ] ">" ] [ Pipe ] ( Block | Identifier ) ;

/* Functions as expressions */

FuncExpr    = "fn" [ Generic ] Signature Block ;

/* Pipe (capture) */
Pipe        = "|" Identifier "|" ;

/* Statements */
Statement   = VarDecl | DeclAssign | Assignment
            | IncrementStatement | DecrementStatement
            | ReturnStatement | break_statement | next_statement ;

Block       = "{" { ( Expression | Statement ) [ terminal ] } "}" ;

VarDecl     = Identifier Type [ "=" Expression ] ;
DeclAssign  = ( DestructurePattern | Identifier ) ":=" Expression ;

DestructurePattern = "{" DestructureField { "," DestructureField } "}" ;
DestructureField   = Identifier [ ":" Identifier ] ;

IncrementStatement = Identifier "++" ;
DecrementStatement = Identifier "--" ;
Assignment  = AssignTarget assignment_operator Expression ;
AssignTarget = Identifier | Selector | IndexExpr ;
ReturnStatement = "return" [ Expression ] ;     // single value or none
break_statement = "break" [ Expression ] ;
next_statement  = "next" ;                       // continue

assignment_operator = ":=" | "=" | "+=" | "-=" | "*=" | "/=" ;
binary_operator     = arithmetic_operator | bitwise_operator | logical_operator ;
arithmetic_operator = "+" | "-" | "*" | "/" | "**" | "%" ;
bitwise_operator    = "&" | "|" | "^" | "~" | "<<" | ">>" ;
logical_operator    = "==" | "!=" | ">" | "<" | ">=" | "<=" | "&&" | "||" ;
unary_operator      = "!" | "-" | "~" ;

/* Identifiers / visibility */

Identifier         = letter { letter | decimal_digit } [ "?" ] ;
ignored_identifier = "_" ; // receiver / binding position only
IdentifierList     = Identifier { "," Identifier } ;
public             = "pub" ; // private by default

/* Literals */

Literal      = ArrayLiteral | MapLiteral | StructLiteral
             | BoolLiteral | IntegerLiteral | FloatLiteral | StringLiteral
             | "null" ; // the sole value of `void`

// Array literal, or generation: `[0..10]` produces 0..9 of an increment type.
ArrayLiteral = "[" ( [ ExpressionList ] | Expression ".." Expression ) "]" ;

MapLiteral   = "{" [ KeyValuePair { "," KeyValuePair } ] "}" ;
KeyValuePair = Expression ":" Expression ; // key is always an expression

StructLiteral     = ( Identifier | Selector | StructType ) [ StructInitializer ] ;
StructInitializer = "{" [ FieldAssignment { ( "," | terminal ) FieldAssignment } ] "}" ;
FieldAssignment   = Identifier ":" Expression ;

BoolLiteral   = "true" | "false" ;
IntegerLiteral = ( decimal_digit { decimal_digit | "_" } )
               | BinaryLiteral | HexLiteral | OctalLiteral ;
BinaryLiteral = "0b" { binary_digit | "_" } ;
HexLiteral    = "0x" { hex_digit | "_" } ;
OctalLiteral  = "0o" { octal_digit | "_" } ;
FloatLiteral  = decimal_digit { decimal_digit | "_" } "." FloatEnd ;
FloatEnd      = decimal_digit { decimal_digit | "_" } [ Exponent ] ;
Exponent      = ( "e" | "E" ) [ "+" | "-" ] decimal_digit { decimal_digit | "_" } ;

StringLiteral    = SingleLineString | MultiLineString ;
SingleLineString = "\"" { StringContent } "\"" ;
MultiLineString  = "\"\"\"" ( StringContent | "\n" ) "\"\"\"" ;
StringContent    = unicode_char_except_special | EscapeSequence | Interpolation ;
EscapeSequence   = "\\" ( "n" | "t" | "r" | "\\" | "\"" | "{" | "}" ) ;
Interpolation    = "{" Expression "}" ;

/* Lexical elements */
comments = "//" unicode_char newline ;

letter        = "a" ... "z" | "A" ... "Z" | "_" ;
decimal_digit = "0" ... "9" ;
binary_digit  = "0" | "1" ;
octal_digit   = "0" ... "7" ;
hex_digit     = decimal_digit | "a" ... "f" | "A" ... "F" ;

terminal = "\r\n" | "\n" ;
unicode_char = /* any visible Unicode code point */ ;
unicode_char_except_special = /* any visible Unicode code point excluding \, {, }, and " */ ;
```
