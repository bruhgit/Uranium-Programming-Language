#ifndef uranium_lexer_h
#define uranium_lexer_h

enum TokenType {
    // Single-character tokens
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_DOT,
    TOKEN_QUESTION,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
    TOKEN_BANG,
    TOKEN_EQUAL,
    TOKEN_GREATER, TOKEN_LESS,
    TOKEN_SEMICOLON,

    // One or two character tokens
    TOKEN_BANG_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_GREATER_EQUAL,
    TOKEN_LESS_EQUAL,

    // Literals
    TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER,

    // Keywords
    TOKEN_AND,
    TOKEN_ASYNC,
    TOKEN_AWAIT,
    TOKEN_BREAK,
    TOKEN_CASE,
    TOKEN_CATCH,
    TOKEN_CLASS, TOKEN_FN, TOKEN_LET, TOKEN_CONST,
    TOKEN_CONTINUE,
    TOKEN_DEFAULT,
    TOKEN_ELSE, TOKEN_FALSE, TOKEN_FOR,
    TOKEN_ELIF,
    TOKEN_ENUM,
    TOKEN_FINALLY,
    TOKEN_IMPLEMENTS,
    TOKEN_IF,
    TOKEN_INTERFACE,
    TOKEN_MATCH,
    TOKEN_NIL,
    TOKEN_OR,
    TOKEN_PRINT, TOKEN_RETURN,
    TOKEN_SUPER,
    TOKEN_SWITCH,
    TOKEN_THIS,
    TOKEN_TRAIT,
    TOKEN_THROW,
    TOKEN_TRUE,
    TOKEN_TRY,
    TOKEN_WHILE,

    TOKEN_ERROR, TOKEN_EOF
};

struct Token {
    TokenType type;
    const char* start;
    int length;
    int line;
    int column;
};

void initLexer(const char* source);
Token scanToken();
const char* tokenTypeName(TokenType type);

#endif
