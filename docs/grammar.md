```ebnf
(* ==========================================
   1. Program Root & Expressions
   ========================================== *)
Program        ::= (StatementExpression | Comment)*

StatementExpression ::= Expression (Terminator | EOF)

Terminator ::= “\n”
Comment ::= "//" [^\n]*

Expression     ::= Binding 
                 | VarBinding 
                 | LogicalOr 
                 | ControlExpr
                 | BreakStmt
                 | ContinueStmt

(* ==========================================
   2. Bindings (Definitions)
   ========================================== *)
Binding        ::= (Visibility)? (TypeBinding | MethodBinding)
Visibility     ::= "pub"

TypeBinding    ::= Identifier (TypeParams)? "=" TypeDefinition
TypeParams     ::= "(" Identifier (Constraint)? ("," Identifier (Constraint)?)* ")"
Constraint     ::= Identifier ( “|” Identifier )*

TypeDefinition ::= StructDef | InterfaceDef | UnionDef | Alias
UnionDef       ::= Type ( "|" Type )+
Alias          ::= Type

StructDef      ::= "struct" "{" (FieldList)? "}"
FieldList      ::= Field (Separator Field)*
Field          ::= (Visibility)? IdentifierList (Type | StructDef) | (Visibility)? Type

InterfaceDef   ::= "interface" "{" (MethodSignature | Identifier)* "}"
MethodSignature::= Identifier "=" "(" ParamList ")" (ReturnType)?

MethodBinding  ::= "(" Receiver ")" "." Identifier "=" FunctionBody
Receiver       ::= Identifier Type

(* ==========================================
   3. Variable Bindings & Logical Flow
   ========================================== *)
VarBinding     ::= (Visibility)? IdentifierList (Type)? "=" Expression (OrGuard)?

LogicalOr      ::= LogicalAnd ( "||" LogicalAnd )*
LogicalAnd     ::= BitwiseExpr ( "&&" BitwiseExpr )*

BitwiseExpr    ::= Comparison ( ("|" | "^" | "&" | "<<" | ">>") Comparison )*
Comparison     ::= Addition ( ("==" | "!=" | ">" | "<" | ">=" | "<=") Addition )*
Addition       ::= Multiplication ( ("+" | "-") Multiplication )*
Multiplication ::= Power ( ("*" | "/") Power )*
Power          ::= Unary ( "**" Power )*

Unary          ::= ("!" | "-" | "~") Primary | Primary

(* ==========================================
   4. Control & Loop Expressions
   ========================================== *)
ControlExpr    ::= IfExpr | SwitchExpr | SpawnExpr | Block | ForExpr

IfExpr         ::= "if" Condition Block (ElseBlock)?
ElseBlock      ::= "else" (Block | IfExpr)

Condition      ::= BindingMatch | Expression
BindingMatch   ::= (Identifier)? Type "=" Expression

SwitchExpr     ::= "switch" (Expression)? "{" (CaseArm)* (ElseArm)? "}"
CaseArm        ::= (Identifier)? (Condition | Type) ":" Block
ElseArm        ::= "else" ":" Block

ForExpr        ::= "for" (ForControl)? Block
ForControl     ::= IdentifierList ":" Expression   (* Iterator style *)
                 | Expression                      (* While style *)
                 | ""                              (* Infinite style *)

BreakStmt      ::= "break" (PositionalArgs)?
NextStmt       ::= "next"

OrGuard        ::= "or" (Identifier)? Block
SpawnExpr      ::= "spawn" Block
ImportExpr     ::= "import" String

(* ==========================================
   5. Primary Units & Functions
   ========================================== *)
Primary        ::= Literal | Identifier | Instantiation | Access | Call | IndexAccess | "(" Expression ")"

IndexAccess    ::= Primary "[" PositionalArgs "]"
Access         ::= Primary "." Identifier
Call           ::= Primary "(" (PositionalArgs)? ")"

FunctionBody   ::= "(" ParamList ")" (ReturnType)? Block
ParamList      ::= (Identifier Type (Constraint)? ("," Identifier Type (Constraint)?)*)?
ReturnType     ::= Type | "(" TypeList ")"
TypeList       ::= Type ("," Type)*

Block          ::= "{" (Expression)* "}" 

(* ==========================================
   6. Arguments, Instantiation & Literals
   ========================================== *)
Instantiation  ::= Type "{" (PositionalArgs | NamedArgs)? "}"
PositionalArgs ::= Expression ("," Expression)*
NamedArgs      ::= Identifier ":" Expression ("," Identifier ":" Expression)*

Literal        ::= Integer | Float | String | MapLiteral | Bool | "void"
MapLiteral     ::= "{" (Expression ":" Expression ("," Expression)*)? "}"
String         ::= "\"" ("{" Expression "}")* "\""

Type           ::= Identifier (TypeArgs)? 
                 | "[]" Type 
                 | "[" Integer "]" Type 
                 | "Task(" Type ")"

TypeArgs       ::= Identifier ("," Identifier)*
IdentifierList ::= Identifier ("," Identifier)*
```
