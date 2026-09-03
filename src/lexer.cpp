#include "lexer.h"
#include <cctype>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <string>

struct LexerContext {
    const char* start;
    const char* current;
    const char* lineStart;
    int line;
    std::string macroName;
};

static std::vector<LexerContext> macroContextStack;
static std::unordered_map<std::string, std::string> macros;

struct Lexer {
    const char* originalSource;
    const char* start;
    const char* current;
    const char* lineStart;
    int line;
    std::vector<int> fstringBraceDepth;
};

static Lexer lexer;

void initLexer(const char* source) {
    lexer.originalSource = source;
    lexer.start = source;
    lexer.current = source;
    lexer.lineStart = source;
    lexer.line = 1;
    lexer.fstringBraceDepth.clear();
    macroContextStack.clear();
    macros.clear();
}

static bool isAtEnd() {
    return *lexer.current == '\0';
}

static char advance() {
    lexer.current++;
    return lexer.current[-1];
}

static char peek() {
    return *lexer.current;
}

static char peekNext() {
    if (isAtEnd()) return '\0';
    return lexer.current[1];
}

static bool matchChar(char expected) {
    if (isAtEnd()) {
        return false;
    }

    if (*lexer.current != expected) {
        return false;
    }

    lexer.current++;
    return true;
}

static Token makeToken(TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer.start;
    token.length = static_cast<int>(lexer.current - lexer.start);
    token.line = lexer.line;
    token.column = static_cast<int>(lexer.start - lexer.lineStart) + 1;
    return token;
}

static Token errorToken(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = static_cast<int>(std::strlen(message));
    token.line = lexer.line;
    token.column = static_cast<int>(lexer.start - lexer.lineStart) + 1;
    return token;
}

static void skipWhitespace() {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                lexer.line++;
                advance();
                lexer.lineStart = lexer.current;
                break;
            case '/':
                if (peekNext() == '/') {
                    while (peek() != '\n' && !isAtEnd()) {
                        advance();
                    }
                } else {
                    return;
                }
                break;
            case '@': {
                if (std::strncmp(lexer.current + 1, "define", 6) == 0) {
                    lexer.current += 7; // @define
                    while (peek() == ' ' || peek() == '\t') advance();
                    const char* idStart = lexer.current;
                    while (std::isalpha(static_cast<unsigned char>(peek())) || std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') advance();
                    if (lexer.current > idStart) {
                        std::string macroName(idStart, lexer.current - idStart);
                        while (peek() == ' ' || peek() == '\t') advance();
                        const char* valStart = lexer.current;
                        while (peek() != '\n' && !isAtEnd()) advance();
                        std::string macroValue(valStart, lexer.current - valStart);
                        macros[macroName] = macroValue;
                        continue;
                    }
                }
                return;
            }
            default:
                return;
        }
    }
}

static TokenType checkKeyword(int start, int length, const char* rest, TokenType type) {
    if (lexer.current - lexer.start == start + length &&
        std::memcmp(lexer.start + start, rest, length) == 0) {
        return type;
    }

    return TOKEN_IDENTIFIER;
}

static TokenType identifierType() {
    switch (lexer.start[0]) {
        case 'a':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'n':
                        return checkKeyword(2, 1, "d", TOKEN_AND);
                    case 's':
                        return checkKeyword(2, 3, "ync", TOKEN_ASYNC);
                    case 'w':
                        return checkKeyword(2, 3, "ait", TOKEN_AWAIT);
                }
            }
            break;
        case 'b':
            return checkKeyword(1, 4, "reak", TOKEN_BREAK);
        case 'c':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'a':
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 's': return checkKeyword(3, 1, "e", TOKEN_CASE);
                                case 't': return checkKeyword(3, 2, "ch", TOKEN_CATCH);
                            }
                        }
                        break;
                    case 'l': return checkKeyword(2, 3, "ass", TOKEN_CLASS);
                    case 'o':
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 'n':
                                    if (lexer.current - lexer.start > 3 && lexer.start[3] == 's') {
                                        return checkKeyword(4, 1, "t", TOKEN_CONST);
                                    }
                                    return checkKeyword(3, 5, "tinue", TOKEN_CONTINUE);
                            }
                        }
                        break;
                }
            }
            break;
        case 'd':
            if (lexer.current - lexer.start > 2 && lexer.start[1] == 'e') {
                switch (lexer.start[2]) {
                    case 'b': return checkKeyword(3, 5, "ugger", TOKEN_DEBUGGER);
                    case 'f': return checkKeyword(3, 4, "ault", TOKEN_DEFAULT);
                }
            }
            break;
        case 'e':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'l':
                        if (lexer.current - lexer.start > 2 && lexer.start[2] == 'i') {
                            return checkKeyword(3, 1, "f", TOKEN_ELIF);
                        }
                        return checkKeyword(2, 2, "se", TOKEN_ELSE);
                    case 'n':
                        return checkKeyword(2, 2, "um", TOKEN_ENUM);
                }
            }
            break;
        case 'f':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'a': return checkKeyword(2, 3, "lse", TOKEN_FALSE);
                    case 'i': return checkKeyword(2, 5, "nally", TOKEN_FINALLY);
                    case 'n': return checkKeyword(2, 0, "", TOKEN_FN);
                    case 'o': return checkKeyword(2, 1, "r", TOKEN_FOR);
                }
            }
            break;
        case 'i':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'f':
                        return checkKeyword(2, 0, "", TOKEN_IF);
                    case 'm':
                        return checkKeyword(2, 8, "plements", TOKEN_IMPLEMENTS);
                    case 'n':
                        return checkKeyword(2, 7, "terface", TOKEN_INTERFACE);
                }
            }
            break;
        case 'l':
            return checkKeyword(1, 2, "et", TOKEN_LET);
        case 'm':
            return checkKeyword(1, 4, "atch", TOKEN_MATCH);
        case 'n':
            if (lexer.current - lexer.start > 1 && lexer.start[1] == 'a') {
                return checkKeyword(2, 7, "mespace", TOKEN_NAMESPACE);
            }
            return checkKeyword(1, 2, "il", TOKEN_NIL);
        case 'o':
            return checkKeyword(1, 1, "r", TOKEN_OR);
        case 'O':
            return checkKeyword(1, 7, "ptional", TOKEN_OPTIONAL);
        case 'p':
            if (lexer.current - lexer.start > 3) {
                if (lexer.start[1] == 'u') {
                    return checkKeyword(2, 4, "blic", TOKEN_PUBLIC);
                }
                if (lexer.start[1] == 'r' && lexer.start[2] == 'i') {
                    if (lexer.start[3] == 'v') {
                        return checkKeyword(4, 3, "ate", TOKEN_PRIVATE);
                    }
                    if (lexer.start[3] == 'n') {
                        if (lexer.current - lexer.start == 5) return checkKeyword(4, 1, "t", TOKEN_PRINT);
                        if (lexer.current - lexer.start == 6) return checkKeyword(4, 2, "tn", TOKEN_PRINTN);
                    }
                }
            }
            break;
        case 'r':
            return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
        case 's':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'u':
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 'p': return checkKeyword(3, 2, "er", TOKEN_SUPER);
                            }
                        }
                        break;
                    case 'w':
                        return checkKeyword(2, 4, "itch", TOKEN_SWITCH);
                    case 't':
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 'a': return checkKeyword(3, 3, "tic", TOKEN_STATIC);
                                case 'r': return checkKeyword(3, 3, "uct", TOKEN_STRUCT);
                            }
                        }
                        break;
                }
            }
            break;
        case 't':
            if (lexer.current - lexer.start > 1) {
                switch (lexer.start[1]) {
                    case 'h':
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 'i': return checkKeyword(3, 1, "s", TOKEN_THIS);
                                case 'r': return checkKeyword(3, 2, "ow", TOKEN_THROW);
                            }
                        }
                        break;
                    case 'r':
                        if (lexer.current - lexer.start > 3 &&
                            lexer.start[2] == 'a' &&
                            lexer.start[3] == 'i') {
                            return checkKeyword(4, 1, "t", TOKEN_TRAIT);
                        }
                        if (lexer.current - lexer.start > 2) {
                            switch (lexer.start[2]) {
                                case 'u': return checkKeyword(3, 1, "e", TOKEN_TRUE);
                                case 'y': return checkKeyword(3, 0, "", TOKEN_TRY);
                            }
                        }
                        break;
                }
            }
            break;
        case 'w':
            return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    }

    return TOKEN_IDENTIFIER;
}

static Token fstringPart(TokenType type) {
    while (peek() != '"' && peek() != '{' && !isAtEnd()) {
        if (peek() == '\n') {
            lexer.line++;
            lexer.lineStart = lexer.current + 1;
        }
        advance();
    }
    
    if (isAtEnd()) {
        return errorToken("Unterminated f-string.");
    }
    
    if (peek() == '"') {
        advance(); // consume "
        return makeToken(type == TOKEN_FSTRING_START ? TOKEN_STRING : TOKEN_FSTRING_END);
    }
    
    // peek() == '{'
    advance(); // consume {
    lexer.fstringBraceDepth.push_back(0);
    return makeToken(type);
}

static Token identifier() {
    while (std::isalpha(static_cast<unsigned char>(peek())) ||
           std::isdigit(static_cast<unsigned char>(peek())) ||
           peek() == '_') {
        advance();
    }

    std::string name(lexer.start, lexer.current - lexer.start);
    auto it = macros.find(name);
    if (it != macros.end()) {
        bool inStack = false;
        for (const auto& ctx : macroContextStack) {
            if (ctx.macroName == name) { inStack = true; break; }
        }
        if (!inStack) {
            LexerContext ctx;
            ctx.start = lexer.start;
            ctx.current = lexer.current;
            ctx.lineStart = lexer.lineStart;
            ctx.line = lexer.line;
            ctx.macroName = name;
            macroContextStack.push_back(ctx);

            lexer.start = it->second.c_str();
            lexer.current = it->second.c_str();
            
            return scanToken();
        }
    }

    if ((lexer.current - lexer.start == 1) && lexer.start[0] == 'f' && peek() == '"') {
        advance(); // consume '"'
        return fstringPart(TOKEN_FSTRING_START);
    }

    if ((lexer.current - lexer.start == 1) && lexer.start[0] == 'R' && peek() == '"' && peekNext() == '(') {
        advance(); // "
        advance(); // (
        while (!(peek() == ')' && peekNext() == '"') && !isAtEnd()) {
            if (peek() == '\n') {
                lexer.line++;
                lexer.lineStart = lexer.current + 1;
            }
            advance();
        }
        if (isAtEnd()) {
            return errorToken("Unterminated R-string.");
        }
        advance(); // )
        advance(); // "
        return makeToken(TOKEN_STRING);
    }

    return makeToken(identifierType());
}

static Token number() {
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    return makeToken(TOKEN_NUMBER);
}

static Token string() {
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') {
            lexer.line++;
            advance();
            lexer.lineStart = lexer.current;
            continue;
        }
        advance();
    }

    if (isAtEnd()) {
        return errorToken("Unterminated string.");
    }

    advance();
    return makeToken(TOKEN_STRING);
}

Token scanToken() {
    while (true) {
        skipWhitespace();
        if (isAtEnd() && !macroContextStack.empty()) {
            LexerContext ctx = macroContextStack.back();
            macroContextStack.pop_back();
            lexer.start = ctx.start;
            lexer.current = ctx.current;
            lexer.lineStart = ctx.lineStart;
            lexer.line = ctx.line;
            continue;
        }
        break;
    }

    lexer.start = lexer.current;

    if (isAtEnd()) {
        return makeToken(TOKEN_EOF);
    }

    char c = advance();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return identifier();
    }

    if (std::isdigit(static_cast<unsigned char>(c))) {
        return number();
    }

    switch (c) {
        case '(': return makeToken(TOKEN_LEFT_PAREN);
        case ')': return makeToken(TOKEN_RIGHT_PAREN);
        case '{': 
            if (!lexer.fstringBraceDepth.empty()) lexer.fstringBraceDepth.back()++;
            return makeToken(TOKEN_LEFT_BRACE);
        case '}': 
            if (!lexer.fstringBraceDepth.empty()) {
                if (lexer.fstringBraceDepth.back() > 0) {
                    lexer.fstringBraceDepth.back()--;
                } else {
                    lexer.fstringBraceDepth.pop_back();
                    return fstringPart(TOKEN_FSTRING_MID);
                }
            }
            return makeToken(TOKEN_RIGHT_BRACE);
        case '[': return makeToken(TOKEN_LEFT_BRACKET);
        case ']': return makeToken(TOKEN_RIGHT_BRACKET);
        case ',': return makeToken(TOKEN_COMMA);
        case ':': return makeToken(TOKEN_COLON);
        case '.': return makeToken(TOKEN_DOT);
        case '?': return makeToken(TOKEN_QUESTION);
        case ';': return makeToken(TOKEN_SEMICOLON);
        case '@': return makeToken(TOKEN_AT);
        case '%': return makeToken(TOKEN_PERCENT);
        case '&': return makeToken(TOKEN_AMPERSAND);
        case '|': return makeToken(TOKEN_PIPE);
        case '^': return makeToken(TOKEN_CARET);
        case '~': return makeToken(TOKEN_TILDE);
        case '+': return makeToken(TOKEN_PLUS);
        case '-': return makeToken(TOKEN_MINUS);
        case '*': return makeToken(TOKEN_STAR);
        case '/': return makeToken(TOKEN_SLASH);
        case '!':
            return makeToken(matchChar('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=':
            return makeToken(matchChar('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<':
            if (matchChar('<')) return makeToken(TOKEN_LESS_LESS);
            return makeToken(matchChar('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            if (matchChar('>')) return makeToken(TOKEN_GREATER_GREATER);
            return makeToken(matchChar('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '"':
            return string();
    }

    return errorToken("Unexpected character.");
}

const char* tokenTypeName(TokenType type) {
    switch (type) {
        case TOKEN_LEFT_PAREN: return "LEFT_PAREN";
        case TOKEN_RIGHT_PAREN: return "RIGHT_PAREN";
        case TOKEN_LEFT_BRACE: return "LEFT_BRACE";
        case TOKEN_RIGHT_BRACE: return "RIGHT_BRACE";
        case TOKEN_LEFT_BRACKET: return "LEFT_BRACKET";
        case TOKEN_RIGHT_BRACKET: return "RIGHT_BRACKET";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_COLON: return "COLON";
        case TOKEN_DOT: return "DOT";
        case TOKEN_QUESTION: return "QUESTION";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_PERCENT: return "PERCENT";
        case TOKEN_AMPERSAND: return "AMPERSAND";
        case TOKEN_PIPE: return "PIPE";
        case TOKEN_CARET: return "CARET";
        case TOKEN_TILDE: return "TILDE";
        case TOKEN_LESS_LESS: return "LESS_LESS";
        case TOKEN_GREATER_GREATER: return "GREATER_GREATER";
        case TOKEN_BANG: return "BANG";
        case TOKEN_EQUAL: return "EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_LESS: return "LESS";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_BANG_EQUAL: return "BANG_EQUAL";
        case TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_STRING: return "STRING";
        case TOKEN_FSTRING_START: return "FSTRING_START";
        case TOKEN_FSTRING_MID: return "FSTRING_MID";
        case TOKEN_FSTRING_END: return "FSTRING_END";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_AND: return "AND";
        case TOKEN_ASYNC: return "ASYNC";
        case TOKEN_AWAIT: return "AWAIT";
        case TOKEN_BREAK: return "BREAK";
        case TOKEN_CASE: return "CASE";
        case TOKEN_CATCH: return "CATCH";
        case TOKEN_CLASS: return "CLASS";
        case TOKEN_FN: return "FN";
        case TOKEN_LET: return "LET";
        case TOKEN_CONST: return "CONST";
        case TOKEN_CONTINUE: return "CONTINUE";
        case TOKEN_DEBUGGER: return "DEBUGGER";
        case TOKEN_DEFAULT: return "DEFAULT";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_FALSE: return "FALSE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_ELIF: return "ELIF";
        case TOKEN_ENUM: return "ENUM";
        case TOKEN_FINALLY: return "FINALLY";
        case TOKEN_IMPLEMENTS: return "IMPLEMENTS";
        case TOKEN_IF: return "IF";
        case TOKEN_INTERFACE: return "INTERFACE";
        case TOKEN_MATCH: return "MATCH";
        case TOKEN_NIL: return "NIL";
        case TOKEN_OPTIONAL: return "OPTIONAL";
        case TOKEN_OR: return "OR";
        case TOKEN_PRINT: return "PRINT";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_PUBLIC: return "PUBLIC";
        case TOKEN_PRIVATE: return "PRIVATE";
        case TOKEN_STATIC: return "STATIC";
        case TOKEN_NAMESPACE: return "NAMESPACE";
        case TOKEN_STRUCT: return "STRUCT";
        case TOKEN_SUPER: return "SUPER";
        case TOKEN_SWITCH: return "SWITCH";
        case TOKEN_THIS: return "THIS";
        case TOKEN_TRAIT: return "TRAIT";
        case TOKEN_THROW: return "THROW";
        case TOKEN_TRUE: return "TRUE";
        case TOKEN_TRY: return "TRY";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_ERROR: return "ERROR";
        case TOKEN_EOF: return "EOF";
        default: return "UNKNOWN";
    }
}

std::string getSourceLine(int line) {
    if (lexer.originalSource == nullptr) return "";
    const char* p = lexer.originalSource;
    int currentLine = 1;
    while (*p != '\0' && currentLine < line) {
        if (*p == '\n') {
            currentLine++;
        }
        p++;
    }
    if (*p == '\0') return "";
    const char* lineStart = p;
    while (*p != '\0' && *p != '\n' && *p != '\r') {
        p++;
    }
    return std::string(lineStart, p);
}
