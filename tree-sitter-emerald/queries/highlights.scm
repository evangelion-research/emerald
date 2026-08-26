[
  "def" "pure" "partial" "return" "if" "else" "elif" "while" "for"
  "in" "match" "try" "catch"
] @keyword

[(break_statement) (continue_statement) (pass_statement)] @keyword

["import" "from" "as"] @keyword.import
["type" "error" "dim"] @keyword.type
"const" @keyword.modifier
[(boolean) (none) (refl)] @constant.builtin
[(integer) (float)] @number
[(string) (f_string)] @string
(comment) @comment
(function_definition name: (identifier) @function)
(parameter name: (identifier) @variable.parameter)
(call_expression function: (identifier) @function.call)
(field_definition name: (identifier) @property)
(record_field name: (identifier) @property)
[(primitive_type) (type_definition name: (identifier))] @type
[(escape_sequence) (interpolation)] @string.escape
[
  "=" "+=" "-=" "*=" "/=" "%=" "+" "-" "*" "/" "//" "%" "**"
  "==" "!=" "<" "<=" ">" ">=" "|>" ">>" "|" "&" "^" "<<" ">>>" "=>" "->"
] @operator
[
  "(" ")" "[" "]" "{" "}" "," "." ":" ";"
] @punctuation.bracket
