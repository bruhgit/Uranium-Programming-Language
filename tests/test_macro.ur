@define PI 3.14159
@define MULTIPLY(A, B) A * B  // wait, macros in this lexer don't take arguments, they just replace identifiers!
@define TWO 2
@define DOUBLE_PI (PI * TWO)

printn("PI is " + PI)
printn("DOUBLE_PI is " + DOUBLE_PI)
