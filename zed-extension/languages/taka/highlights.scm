(line_comment) @comment
(block_comment) @comment
(string) @string
(char) @string
(number) @number
(keyword) @keyword
(operator) @operator
(identifier) @variable

((identifier) @type
 (#match? @type "^[A-Z]"))

((identifier) @type.builtin
 (#any-of? @type.builtin
   "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64"
   "f32" "f64" "char" "bool" "sview" "never" "Self"))

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  ","
  ";"
  "?"
  ":"
  "."
  ".."
  "::"
  "->"
  "=>"
] @punctuation
