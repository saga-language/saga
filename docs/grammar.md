```ebnf
/*
Go EBNF Syntax
  
Declinition = production_name "=" [ Expression ] ";" ;
Expression = Term { "|" Term } ;
Factor = production_name | token [ "..." token ] | Group | Option | Repetition ;
Group = '(' Expression ')' ;
Option = '[' Expression ']' ;
Repetition: '{' Expression '}' ;

PascalCase productions are non-terminal, snake_case productions are terminal.
*/

Source = { Declaration }
Declaration = [ public ] ( ConstDecl | EnumDecl | FuncDecl | ImportDecl | InterfaceDecl | StructDecl ) ;

ConstDecl = "const" Identifier [ Type ] "="  Expression ;
EnumDecl = "enum" Identifier "{" EnumField { terminal EnumField } "}" ;
FuncDecl = "fn" [Generic] [ Receiver ] Identifier Signature Block ;
ImportDecl = "import" StringLiteral ;
InterfaceDecl = "interface" [ Generic ] Identifier "{" [ InterfaceField { terminal  InterfaceField } ] "}" ;
StructDecl = "struct" [ Generic ] Identifier [ "<" IdentifierList ] "{" [ StructMember { terminal StructMember } ] "}" ;

/* Enum */
EnumField = Identifier [ EnumInitializer ] ;
EnumInitializer = "{" Identifier ":" Expression [ "," Identifier ":" Expression ]  "}" ;

/* Function */
Receiver = "(" Identifier Type ")" ; // example: (u User)
Signature = "(" [ ParameterList ] ")" TypeList ;
ParameterList = IdentifierList ParameterType { "," IdentifierList ParameterType } ;
IdentifierList = Identifier { "," Identifier } ; // example: a, b2, c?, d_e
Block = "{" { ( Expression | Statement ) [ terminal ] } "}" ;
ParameterType = Type | VariadicType ;
VariadicType = "..." Type ;

/* Interface */
InterfaceField = [ public ] Identifier Signature ;

/* Struct */
StructMember = [ public ] ( FieldSpec | FuncDecl ) ;
FieldSpec = IdentifierList Type ;

/* Expressions */

// Precedence (highest to lowest)
// 1. Access:   . [] ()
// 2. Unary:    ! - ~
// 3. Power:    **
// 4. Multiply: * / %
// 5. Add:      + -
// 6. Bitwise:  & | ^ << >> ~
// 7. Compare:  == != > < >= <=
// 8. Logical:  && ||
// 9. Or:       or
// 10. Assign:  := = += -= *= /=
Expression = LogicalExpression [ OrExpr ] ;
LogicalExpression = UnaryExpr | BinaryExpr | PrimaryExpr ;

BinaryExpr = Expression BinaryOperator PrimaryExpr ;
OrExpr = Expression "or" [ IdentifierPipe ] Block ;
IdentifierPipe = "|" Identifier "|" ;
UnaryExpr = unary_operator PrimaryExpr ;

PrimaryExpr = Identifier | LiteralExpr | Selector | IndexExpr | CallExpr | IfExpr | RangeExpr | SwitchExpr | SpawnExpr | FuncExpr | ImportExpr | TypeExpr | "(" Expression ")" ;

assignment_operator = ":=" | "+=" | "-=" | "*=" | "/=" | "=" ;

CallExpr =  PrimaryExpr "(" [ ExpressionList ] ")" ;
ExpressionList = Expression { "," Expression } ;

ForExpr = "for" [ ForMode ] [ IdentifierPipe ] Block ;
ForMode = Expression | IteratorClause | RangeClause
IteratorClause = Identifier ":=" Expression ";" Expression ";" Assignment ;
RangeClause = Identifier { "," Identifier } ":" Expression ;

FuncExpr = "fn" [ Generic ] Signature Block ;

IfExpr = "if" Expression Block [ ElseBlock ] ;
ElseBlock = "else" Block ;

ImportExpr = ImportDecl ;

IndexExpr = Selector "[" ( Expression | Slice ) "]" ;
Selector = ( Identifier | PrimaryExpr ) "." Identifier ;
Slice = [ Expression ] ".." [ Expression ] ;

LiteralExpr = ArrayLiteral | BoolLiteral | FloatLiteral | IntegerLiteral | MapLiteral | StringLiteral | StructLiteral ;

RangeExpr = "(" Expression ".." Expression ")" ;

SpawnExpr = [ Generic ] "spawn" [ IdentifierPipe ] ( Block | Identifier ) ;

SwitchExpr = "switch" Expression SwitchBlock ;
SwitchBlock = "{" CaseArm { CaseArm } [ ElseArm ] "}" ;
CaseArm = "case" Expression ":" ( Expression | Block ) ;
ElseArm = "else:" ( Expression | Block ) ;

TypeExpr = Type ;

Statement = Assignment | IncrementStatement | DecrementStatement | VarDecl | ReturnStatement | break_statement | next_statement ;
IncrementStatement = Identifier "++" ;
DecrementStatement = Identifier "--" ;
VarDecl = Identifier Type [ "=" Expression ] ; // Semantic rule: shadowing a variable from an outer scope, or redeclaring an identifier in the current scope, is an error.
Assignment = AssignTargetList assignment_operator ExpressionList ; // the number of values returned by the rhs must match the lhs
AssignTarget = Identifier | Selector | IndexExpr ;
AssignTargetList = AssignTarget { "," AssignTarget } ;
ReturnStatement = "return" [ ExpressionList ] ;
break_statement = "break" [ ExpressionList ] ;
next_statement = "next" ; // continue

BinaryOperator = arithmetic_operator | bitwise_operator | logical_operator ;
arithmetic_operator = "+" | "-" | "*" | "/" | "**" | "%" ;
bitwise_operator = "&" | "|" | "^" | "~" | "<<" | ">>" ;
logical_operator = "==" | "!=" | ">" | "<" | ">=" | "<=" | "&&" | "||" ;
unary_operator = "!" | "-"

/* Identifiers */
Identifier = letter | { letter | decimal_digit } [ "?" ]
ignored_identifier = "_" { letter | decimal_digit } ; // not allowed in defintions, only local variables

/* Visibility */
public = "pub" ; // private by default, no need to specify

/* Types */
Type = UnionType ;
UnionType = SingleType { "|" SingleType } ;
SingleType = Identifier | Selector | IntrinsicType | StructType ;
Generic = "|" TypeList "|" ;
TypeList = Type { "," Type } ;


IntrinsicType = ArrayType | FuncType | MapType | RangeType | StructType | basic_type | float_type | integer_type | void_type ;
ArrayType = "[" Type "]" ; // prevent union array types
FuncType = "fn" Signature ;
RangeType = "(" Type ")" ; 
MapType = "{" Type : Type "}" ;
StructType = "struct" "{" [ FieldSpec { "," FieldSpec } ] "}" ;

basic_type = "Bool" | "Byte" | "Int" | "Float" | "String" ;
float_type = "Float32" | "Float64" ;
integer_type = "Int8" | "Int16" | "Int32" | "Int64" | "Uint8" | "Uint16" | "Uint32" | "Uint64" ;
void_type = "Void" ;

/* Literals */
ArrayLiteral = "[" [ Expression { "," Expression } ] "]" ;

BoolLiteral = "true" | "false" ;

IntegerLiteral = ( decimal_digit { decimal_digit | "_" } ) | BinaryLiteral | HexLiteral | OctalLiteral ;
BinaryLiteral = "0b" { binary_digit | "_" } ;
HexLiteral = "0x" { hex_digit | "_" } ;
OctalLiteral = "0o" { octal_digit | "_" } ;

FloatLiteral =  decimal_digit { decimal_digit | "_" }  "." FloatEnd ;
FloatEnd = decimal_digit { decimal_digit | "_" } [ Exponent ];
Exponent = ( "e" | "E" ) [ "+" | "-" ] decimal_digit { decimal_digit | "_" } ;

MapLiteral = "{" { KeyValuePair } "}" ;
KeyValuePair = Expression ":" Expression ;

StringLiteral = SingleLineString | MultiLineString ;
SingleLineString = "\"" { StringContent } "\"" ;
MultiLineString = "\"\"\"" ( StringContent | "\n" ) "\"\"\"" ;
StringContent = unicode_char_except_special | EscapeSequence | Interpolation ;
EscapeSequence = "\\" ( "n" | "t" | "r" | "\\" | "\"" | "{" | "}" ) ;
Interpolation = "{" Expression "}" ;

StructLiteral = ( Identifier | Selector | StructType ) StructInitializer ;
StructInitializer = "{" [ FieldAssignment { ( "," | terminal ) FieldAssignment } ] "}" ;
FieldAssignment = Identifier ":" Expression ;

/* Lexical elements */
comments = "//" unicode_char newline ;

/* Letters and digits */
letter = "a" ... "z" | "A" ... "Z" | "_" ;
decimal_digit = "0" ... "9" ;
binary_digit = "0" | "1" ;
octal_digit = "0" ... "7" ;
hex_digit = decimal_digit | "a" ... "f" | "A" ... "F" ;

/* characters */
terminal = "\r\n" | "\n" ;
unicode_char = /* any visible Unicode code point */ ;
unicode_char_except_special = /* any visible Unicode code point excluding \, {, }, and " */ ;
```
