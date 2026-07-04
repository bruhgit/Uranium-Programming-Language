#include "compiler.h"
#include "heap.h"
#include "lexer.h"
#include "optimizer.h"
#include "type_system.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

enum FunctionType {
    TYPE_SCRIPT,
    TYPE_FUNCTION,
    TYPE_METHOD,
};

struct Local {
    Token name;
    int depth;
    bool isConst;
    bool isCaptured;
    std::string typeAnnotation;
};

struct GlobalSymbol {
    Token name;
    bool isConst;
    std::string typeAnnotation;
};

struct Upvalue {
    uint8_t index;
    bool isLocal;
    bool isConst;
    std::string typeAnnotation;
};

enum PatternKind {
    PATTERN_WILDCARD,
    PATTERN_BINDING,
    PATTERN_LITERAL,
    PATTERN_PATH,
    PATTERN_ARRAY,
    PATTERN_OBJECT,
};

struct Pattern;

struct PatternField {
    std::string key;
    std::shared_ptr<Pattern> pattern;
};

struct Pattern {
    PatternKind kind;
    bool literalBool;
    Token literalToken;
    std::string name;
    std::vector<std::shared_ptr<Pattern>> elements;
    std::vector<PatternField> fields;

    Pattern()
        : kind(PATTERN_WILDCARD),
          literalBool(false),
          literalToken{} {
    }
};

struct ContractMethod {
    std::string name;
    int arity;
    bool isAsync;
};

struct ContractDeclaration {
    std::string name;
    bool isTrait;
    std::vector<std::string> genericParameters;
    std::vector<ContractMethod> methods;
};

struct Compiler {
    Compiler* enclosing;
    Compiler* root;
    FunctionType type;
    bool isAsync;
    FunctionPtr function;
    std::vector<Local> locals;
    std::vector<Upvalue> upvalues;
    std::vector<GlobalSymbol> globalSymbols;
    int scopeDepth;
};

struct Parser {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
};

struct ClassCompiler {
    ClassCompiler* enclosing;
    Token name;
    Token superclassName;
    bool hasSuperclass;
};

struct LoopCompiler {
    LoopCompiler* enclosing;
    int breakScopeDepth;
    int continueScopeDepth;
    int continueTarget;
    std::vector<int> breakJumps;
};

enum TransferKind {
    TRANSFER_RETURN_VALUE,
    TRANSFER_RETURN_NIL,
    TRANSFER_BREAK,
    TRANSFER_CONTINUE,
    TRANSFER_THROW,
};

struct PendingTransfer {
    int id;
    TransferKind kind;
    int continueTarget;
    int cleanupDepth;
    std::vector<int> jumpsToTarget;
};

struct TryCompiler {
    TryCompiler* enclosing;
    int scopeDepth;
    int actionLocal;
    int valueLocal;
    int nextTransferId;
    bool active;
    bool handlerActive;
    bool compiledCatch;
    bool compiledFinally;
    std::vector<int> afterTryJumps;
    std::vector<PendingTransfer> transfers;
};

static Parser parser;
static Compiler* current = nullptr;
static ClassCompiler* currentClass = nullptr;
static LoopCompiler* currentLoop = nullptr;
static TryCompiler* currentTry = nullptr;
static std::vector<ContractDeclaration> declaredContracts;
static std::vector<std::unique_ptr<std::string>> ownedSyntheticLexemes;
static std::unordered_map<std::string, FunctionPtr> declaredFunctions;
static FunctionPtr lastResolvedCallable = nullptr;
static void error(const char* message);

static std::string currentCompileFilename = "script.ur";

static std::string lastEmittedType = "Any";

static void setType(const std::string& type) {
    lastEmittedType = normalizeTypeAnnotation(type);
}

static void clearResolvedCallable() {
    lastResolvedCallable = nullptr;
}

static std::string describeTypeForError(const std::string& type) {
    std::string normalized = normalizeTypeAnnotation(type);
    return normalized.empty() ? "Any" : normalized;
}

static bool reportTypeMismatch(const std::string& expected,
                               const std::string& actual,
                               const std::string& context) {
    if (areTypesCompatible(expected, actual)) {
        return true;
    }

    std::string message =
        "Static type mismatch";
    if (!context.empty()) {
        message += " in " + context;
    }
    message += ": expected '" + describeTypeForError(expected) +
               "' but got '" + describeTypeForError(actual) + "'.";
    error(message.c_str());
    return false;
}

static Chunk* currentChunk() {
    return &current->function->chunk;
}

void (*g_compileErrorCallback)(const std::string& message, int line, int column, int length) = nullptr;

static void errorAt(const Token& token, const char* message) {
    if (parser.panicMode) {
        return;
    }

    parser.panicMode = true;
    
    if (g_compileErrorCallback != nullptr) {
        g_compileErrorCallback(message, token.line, token.column, token.length);
    }

    std::cerr << "\033[1;31merror:\033[0m " << message << "\n";
    std::cerr << "  --> " << currentCompileFilename << ":" << token.line << ":" << token.column << "\n";

    std::string sourceLine = getSourceLine(token.line);
    if (!sourceLine.empty()) {
        std::cerr << "   |\n";
        std::cerr << " " << token.line << " | " << sourceLine << "\n";
        std::cerr << "   | ";
        for (int i = 1; i < token.column; ++i) {
            std::cerr << " ";
        }
        int len = token.length;
        if (len <= 0) len = 1;
        std::cerr << "\033[1;32m";
        for (int i = 0; i < len; ++i) {
            std::cerr << "^";
        }
        std::cerr << "\033[0m\n";
    } else {
        std::cerr << "   |\n";
    }
    std::cerr << std::endl;
    parser.hadError = true;
}

static void error(const char* message) {
    errorAt(parser.previous, message);
}

static void errorAtCurrent(const char* message) {
    errorAt(parser.current, message);
}

static void advanceParser() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) {
            break;
        }

        errorAtCurrent(parser.current.start);
    }
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) {
        return false;
    }

    advanceParser();
    return true;
}

static Token consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advanceParser();
        return parser.previous;
    }

    errorAtCurrent(message);
    return parser.current;
}

static bool identifiersEqual(const Token& a, const Token& b) {
    return a.length == b.length && std::memcmp(a.start, b.start, a.length) == 0;
}

static void initCompiler(Compiler* compiler,
                         FunctionType type,
                         const std::string& name,
                         bool isAsync = false) {
    compiler->enclosing = current;
    compiler->root = current == nullptr ? compiler : current->root;
    compiler->type = type;
    compiler->isAsync = isAsync;
    compiler->function = uraniumHeap().allocateFunction(name);
    compiler->function->isAsync = isAsync;
    compiler->function->minArity = 0;
    compiler->function->hasReceiverSlot = (type == TYPE_METHOD);
    compiler->function->parameterNames.clear();
    compiler->function->parameterTypes.clear();
    compiler->function->genericParameters.clear();
    compiler->function->returnType.clear();
    compiler->locals.clear();
    compiler->upvalues.clear();
    compiler->globalSymbols.clear();
    compiler->scopeDepth = (type == TYPE_FUNCTION || type == TYPE_METHOD) ? 1 : 0;
    current = compiler;
}

static void emitByte(uint8_t byte) {
    currentChunk()->writeChunk(byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static void emitShort(uint16_t value) {
    emitByte(static_cast<uint8_t>((value >> 8) & 0xff));
    emitByte(static_cast<uint8_t>(value & 0xff));
}

static void emitNil() {
    emitByte(OP_NIL);
}

static void emitTrue() {
    emitByte(OP_TRUE);
}

static void emitFalse() {
    emitByte(OP_FALSE);
}

static bool isInitializer() {
    return current != nullptr &&
           current->type == TYPE_METHOD &&
           current->function != nullptr &&
           current->function->name == "init";
}

static void emitReturn() {
    if (isInitializer()) {
        emitBytes(OP_GET_LOCAL, 0);
        emitByte(OP_RETURN);
        return;
    }

    emitNil();
    emitByte(OP_RETURN);
}

static int makeConstant(const Value& value) {
    int constant = currentChunk()->addConstant(value);
    if (constant > UINT16_MAX) {
        std::string message =
            "Too many constants in one chunk (" + std::to_string(constant) + ").";
        error(message.c_str());
        return 0;
    }

    return constant;
}

static void emitConstantIndex(uint8_t shortOp, uint8_t longOp, int constant) {
    if (constant <= UINT8_MAX) {
        emitBytes(shortOp, static_cast<uint8_t>(constant));
        return;
    }

    emitByte(longOp);
    emitShort(static_cast<uint16_t>(constant));
}

static void emitConstant(const Value& value) {
    emitConstantIndex(OP_CONSTANT, OP_CONSTANT_LONG, makeConstant(value));
}

static int identifierConstant(const Token& name) {
    return makeConstant(Value::stringValue(std::string(name.start, name.length)));
}

static bool tokenMatchesLiteral(const Token& token, const char* literal) {
    int literalLength = static_cast<int>(std::strlen(literal));
    return token.length == literalLength &&
           std::memcmp(token.start, literal, literalLength) == 0;
}

static Token syntheticToken(TokenType type, const char* literal, int line) {
    Token token;
    token.type = type;
    token.start = literal;
    token.length = static_cast<int>(std::strlen(literal));
    token.line = line;
    token.column = 1;
    return token;
}

static Token ownedIdentifierToken(const std::string& literal, int line) {
    ownedSyntheticLexemes.push_back(std::make_unique<std::string>(literal));
    return syntheticToken(TOKEN_IDENTIFIER, ownedSyntheticLexemes.back()->c_str(), line);
}

static std::string tokenLexeme(const Token& token) {
    if (token.start == nullptr || token.length <= 0) {
        return std::string();
    }

    return std::string(token.start, token.length);
}

static FunctionPtr lookupDeclaredFunction(const std::string& name) {
    auto it = declaredFunctions.find(name);
    if (it == declaredFunctions.end()) {
        return nullptr;
    }
    return it->second;
}

static bool typeContainsGenericParameter(const std::string& type,
                                         const std::vector<std::string>& generics) {
    std::string normalized = normalizeTypeAnnotation(type);
    for (const std::string& generic : generics) {
        std::size_t position = normalized.find(generic);
        while (position != std::string::npos) {
            bool leftOk = position == 0 ||
                          !std::isalnum(static_cast<unsigned char>(normalized[position - 1]));
            std::size_t rightIndex = position + generic.size();
            bool rightOk = rightIndex >= normalized.size() ||
                           !std::isalnum(static_cast<unsigned char>(normalized[rightIndex]));
            if (leftOk && rightOk) {
                return true;
            }
            position = normalized.find(generic, position + 1);
        }
    }
    return false;
}

static std::string inferStaticCallReturnType(const FunctionPtr& function,
                                             const std::vector<std::string>& finalArgTypes) {
    if (function == nullptr) {
        return "Any";
    }

    if (function->genericParameters.empty()) {
        if (function->returnType.empty()) {
            return "Any";
        }
        return normalizeTypeAnnotation(function->returnType);
    }

    std::unordered_map<std::string, std::string> bindings;
    if (!inferTypeBindings(function->genericParameters, function->parameterTypes,
                           finalArgTypes, &bindings)) {
        return "Any";
    }

    std::string returnType = applyTypeBindings(function->returnType, bindings);
    if (typeContainsGenericParameter(returnType, function->genericParameters)) {
        return "Any";
    }

    return normalizeTypeAnnotation(returnType);
}

static void validateStaticCall(FunctionPtr function,
                               const std::vector<std::string>& providedArgTypes,
                               const std::vector<std::string>& providedArgNames,
                               const std::string& calleeName,
                               std::string* inferredReturnType) {
    if (inferredReturnType != nullptr) {
        *inferredReturnType = "Any";
    }

    if (function == nullptr) {
        return;
    }

    int expectedArgCount = static_cast<int>(function->parameterNames.size());
    int minArgCount = function->minArity - (function->hasReceiverSlot ? 1 : 0);
    if (minArgCount < 0) {
        minArgCount = 0;
    }

    std::vector<std::string> finalArgTypes(static_cast<std::size_t>(expectedArgCount), "Any");
    std::vector<bool> filled(static_cast<std::size_t>(expectedArgCount), false);
    int nextPositional = 0;
    for (std::size_t index = 0; index < providedArgTypes.size(); ++index) {
        const std::string& providedName =
            index < providedArgNames.size() ? providedArgNames[index] : std::string();
        if (providedName.empty()) {
            while (nextPositional < expectedArgCount &&
                   filled[static_cast<std::size_t>(nextPositional)]) {
                nextPositional++;
            }

            if (nextPositional >= expectedArgCount) {
                error(("Function '" + calleeName + "' received too many positional arguments.")
                          .c_str());
                return;
            }

            finalArgTypes[static_cast<std::size_t>(nextPositional)] = providedArgTypes[index];
            filled[static_cast<std::size_t>(nextPositional)] = true;
            nextPositional++;
            continue;
        }

        bool found = false;
        for (int paramIndex = 0; paramIndex < expectedArgCount; ++paramIndex) {
            if (function->parameterNames[static_cast<std::size_t>(paramIndex)] != providedName) {
                continue;
            }

            if (filled[static_cast<std::size_t>(paramIndex)]) {
                error(("Function '" + calleeName + "' received duplicate argument '" +
                       providedName + "'.")
                          .c_str());
                return;
            }

            finalArgTypes[static_cast<std::size_t>(paramIndex)] = providedArgTypes[index];
            filled[static_cast<std::size_t>(paramIndex)] = true;
            found = true;
            break;
        }

        if (!found) {
            error(("Function '" + calleeName + "' has no parameter named '" +
                   providedName + "'.")
                      .c_str());
            return;
        }
    }

    for (int index = 0; index < minArgCount; ++index) {
        if (!filled[static_cast<std::size_t>(index)]) {
            error(("Function '" + calleeName + "' is missing required arguments.").c_str());
            return;
        }
    }

    std::unordered_map<std::string, std::string> bindings;
    if (!function->genericParameters.empty()) {
        inferTypeBindings(function->genericParameters, function->parameterTypes,
                          finalArgTypes, &bindings);
    }

    for (std::size_t index = 0; index < function->parameterTypes.size() &&
                                index < finalArgTypes.size();
         ++index) {
        std::string expected = function->parameterTypes[index];
        if (!bindings.empty()) {
            expected = applyTypeBindings(expected, bindings);
        }

        if (!isConcreteTypeAnnotation(expected) ||
            !isConcreteTypeAnnotation(finalArgTypes[index])) {
            continue;
        }

        reportTypeMismatch(expected, finalArgTypes[index], "call to " + calleeName);
    }

    if (inferredReturnType != nullptr) {
        *inferredReturnType = inferStaticCallReturnType(function, finalArgTypes);
    }
}

static int findDeclaredLocalSlot(Compiler* compiler, const Token& name) {
    for (int i = static_cast<int>(compiler->locals.size()) - 1; i >= 0; --i) {
        if (identifiersEqual(compiler->locals[static_cast<std::size_t>(i)].name, name)) {
            return i;
        }
    }

    return -1;
}

static void emitUnset() {
    emitByte(OP_UNSET);
}

static bool tokenImmediatelyFollowedByColon(const Token& token) {
    if (token.start == nullptr) {
        return false;
    }

    const char* cursor = token.start + token.length;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        cursor++;
    }

    return *cursor == ':';
}

static std::vector<std::string> parseOptionalGenericParameterList() {
    std::vector<std::string> parameters;
    if (!match(TOKEN_LESS)) {
        return parameters;
    }

    do {
        Token parameter = consume(TOKEN_IDENTIFIER, "Expect generic parameter name.");
        if (parameter.type == TOKEN_IDENTIFIER) {
            parameters.push_back(tokenLexeme(parameter));
        }
    } while (match(TOKEN_COMMA));

    consume(TOKEN_GREATER, "Expect '>' after generic parameter list.");
    return parameters;
}

static std::string captureTypeAnnotation(const std::vector<TokenType>& stopTokens) {
    std::string annotation;
    int genericDepth = 0;
    int bracketDepth = 0;

    while (!check(TOKEN_EOF)) {
        bool shouldStop = genericDepth == 0 && bracketDepth == 0;
        if (shouldStop) {
            for (TokenType stopType : stopTokens) {
                if (check(stopType)) {
                    return annotation;
                }
            }
        }

        if (check(TOKEN_LESS)) {
            genericDepth++;
        } else if (check(TOKEN_GREATER) && genericDepth > 0) {
            genericDepth--;
        } else if (check(TOKEN_LEFT_BRACKET)) {
            bracketDepth++;
        } else if (check(TOKEN_RIGHT_BRACKET) && bracketDepth > 0) {
            bracketDepth--;
        }

        if (!annotation.empty()) {
            annotation.push_back(' ');
        }
        annotation.append(parser.current.start, parser.current.length);
        advanceParser();
    }

    return annotation;
}

static void skipTokensUntil(const std::vector<TokenType>& stopTokens) {
    int genericDepth = 0;
    int bracketDepth = 0;
    int parenDepth = 0;

    while (!check(TOKEN_EOF)) {
        bool shouldStop = genericDepth == 0 && bracketDepth == 0 && parenDepth == 0;
        if (shouldStop) {
            for (TokenType stopType : stopTokens) {
                if (check(stopType)) {
                    return;
                }
            }
        }

        if (match(TOKEN_LESS)) {
            genericDepth++;
            continue;
        }

        if (match(TOKEN_GREATER)) {
            if (genericDepth > 0) {
                genericDepth--;
            }
            continue;
        }

        if (match(TOKEN_LEFT_BRACKET)) {
            bracketDepth++;
            continue;
        }

        if (match(TOKEN_RIGHT_BRACKET)) {
            if (bracketDepth > 0) {
                bracketDepth--;
            }
            continue;
        }

        if (match(TOKEN_LEFT_PAREN)) {
            parenDepth++;
            continue;
        }

        if (match(TOKEN_RIGHT_PAREN)) {
            if (parenDepth > 0) {
                parenDepth--;
            }
            continue;
        }

        advanceParser();
    }
}

static void skipBlockTokens() {
    int depth = 1;
    while (depth > 0 && !check(TOKEN_EOF)) {
        if (match(TOKEN_LEFT_BRACE)) {
            depth++;
            continue;
        }

        if (match(TOKEN_RIGHT_BRACE)) {
            depth--;
            continue;
        }

        advanceParser();
    }
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitShort(UINT16_MAX);
    return static_cast<int>(currentChunk()->code.size()) - 2;
}

static void patchJump(int offset) {
    int jump = static_cast<int>(currentChunk()->code.size()) - offset - 2;
    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
        return;
    }

    currentChunk()->code[offset] = static_cast<uint8_t>((jump >> 8) & 0xff);
    currentChunk()->code[offset + 1] = static_cast<uint8_t>(jump & 0xff);
}

static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    int offset = static_cast<int>(currentChunk()->code.size()) - loopStart + 2;
    if (offset > UINT16_MAX) {
        error("Loop body is too large.");
        return;
    }

    emitShort(static_cast<uint16_t>(offset));
}

static int emitExceptionHandler() {
    emitByte(OP_PUSH_EXCEPTION_HANDLER);
    emitShort(UINT16_MAX);
    return static_cast<int>(currentChunk()->code.size()) - 2;
}

static void patchExceptionHandler(int offset) {
    int target = static_cast<int>(currentChunk()->code.size());
    if (target > UINT16_MAX) {
        error("Too much code for exception handler target.");
        return;
    }

    currentChunk()->code[offset] = static_cast<uint8_t>((target >> 8) & 0xff);
    currentChunk()->code[offset + 1] = static_cast<uint8_t>(target & 0xff);
}

static FunctionPtr endCompiler() {
    emitReturn();
    FunctionPtr function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) {
        const std::string chunkName = function->name.empty() ? "script" : function->name;
        disassembleChunk(&function->chunk, chunkName.c_str());
    }
#endif

    current = current->enclosing;
    return function;
}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;

    while (!current->locals.empty() && current->locals.back().depth > current->scopeDepth) {
        if (current->locals.back().isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        current->locals.pop_back();
    }
}

static void addLocal(const Token& name, bool isConst, const std::string& typeAnnotation = "Any") {
    if (current->locals.size() >= UINT8_MAX) {
        error("Too many local variables in one scope.");
        return;
    }

    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; --i) {
        const Local& local = current->locals[i];
        if (local.depth != -1 && local.depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, local.name)) {
            std::string message =
                "A variable named '" + tokenLexeme(name) + "' already exists in this scope.";
            error(message.c_str());
            return;
        }
    }

    current->locals.push_back({name, -1, isConst, false, typeAnnotation});
}

static void registerGlobalSymbol(const Token& name, bool isConst, const std::string& typeAnnotation = "Any") {
    Compiler* root = current->root;

    for (const GlobalSymbol& symbol : root->globalSymbols) {
        if (identifiersEqual(symbol.name, name)) {
            error("A global with this name already exists.");
            return;
        }
    }

    root->globalSymbols.push_back({name, isConst, typeAnnotation});
}

static void declareVariable(const Token& name, bool isConst, const std::string& typeAnnotation = "Any") {
    if (current->scopeDepth == 0) {
        registerGlobalSymbol(name, isConst, typeAnnotation);
        return;
    }

    addLocal(name, isConst, typeAnnotation);
}

static void markInitialized() {
    if (current->scopeDepth == 0 || current->locals.empty()) {
        return;
    }

    current->locals.back().depth = current->scopeDepth;
}

static void defineVariable(int global, bool isConst) {
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }

    if (isConst) {
        emitConstantIndex(OP_DEFINE_CONST_GLOBAL, OP_DEFINE_CONST_GLOBAL_LONG, global);
    } else {
        emitConstantIndex(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_LONG, global);
    }
}

static int resolveLocal(Compiler* compiler, const Token& name, bool* isConst) {
    for (int i = static_cast<int>(compiler->locals.size()) - 1; i >= 0; --i) {
        const Local& local = compiler->locals[i];
        if (!identifiersEqual(name, local.name)) {
            continue;
        }

        if (local.depth == -1) {
            error("Cannot read a local variable in its own initializer.");
        }

        if (isConst != nullptr) {
            *isConst = local.isConst;
        }
        return i;
    }

    return -1;
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal, bool isConst) {
    for (std::size_t i = 0; i < compiler->upvalues.size(); ++i) {
        const Upvalue& upvalue = compiler->upvalues[i];
        if (upvalue.index == index && upvalue.isLocal == isLocal) {
            return static_cast<int>(i);
        }
    }

    if (compiler->upvalues.size() >= UINT8_MAX) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues.push_back({index, isLocal, isConst, {}});
    compiler->function->upvalueCount = static_cast<int>(compiler->upvalues.size());
    return static_cast<int>(compiler->upvalues.size() - 1);
}

static int resolveUpvalue(Compiler* compiler, const Token& name, bool* isConst) {
    if (compiler->enclosing == nullptr) {
        return -1;
    }

    bool localIsConst = false;
    int local = resolveLocal(compiler->enclosing, name, &localIsConst);
    if (local != -1) {
        compiler->enclosing->locals[static_cast<std::size_t>(local)].isCaptured = true;
        if (isConst != nullptr) {
            *isConst = localIsConst;
        }
        return addUpvalue(compiler, static_cast<uint8_t>(local), true, localIsConst);
    }

    bool upvalueIsConst = false;
    int upvalue = resolveUpvalue(compiler->enclosing, name, &upvalueIsConst);
    if (upvalue != -1) {
        if (isConst != nullptr) {
            *isConst = upvalueIsConst;
        }
        return addUpvalue(compiler, static_cast<uint8_t>(upvalue), false, upvalueIsConst);
    }

    return -1;
}

static bool resolveGlobalSymbol(const Token& name, bool* isConst) {
    Compiler* root = current->root;
    for (int i = static_cast<int>(root->globalSymbols.size()) - 1; i >= 0; --i) {
        const GlobalSymbol& symbol = root->globalSymbols[i];
        if (!identifiersEqual(name, symbol.name)) {
            continue;
        }

        if (isConst != nullptr) {
            *isConst = symbol.isConst;
        }
        return true;
    }

    return false;
}

static std::string getVariableType(const Token& name) {
    for (int i = current->locals.size() - 1; i >= 0; --i) {
        if (identifiersEqual(name, current->locals[i].name)) {
            return normalizeTypeAnnotation(current->locals[i].typeAnnotation);
        }
    }
    for (const GlobalSymbol& sym : current->root->globalSymbols) {
        if (identifiersEqual(name, sym.name)) {
            return normalizeTypeAnnotation(sym.typeAnnotation);
        }
    }
    return "Any";
}

static void expression();
static void declaration();
static void statement();
static void block();
static void assignment();
static void ternary(bool canAssign);
static void orExpression(bool canAssign);
static void collectionLiteral();
static void primary(bool canAssign);
static void classDeclaration();
static void enumDeclaration();
static void contractDeclaration(bool isTrait);
static void tryStatement();
static void ifClauseStatement();
static void switchStatement();
static void matchStatement();
static std::shared_ptr<Pattern> parsePattern();

static void synchronize() {
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) {
            return;
        }

        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_ENUM:
            case TOKEN_FN:
            case TOKEN_ASYNC:
            case TOKEN_INTERFACE:
            case TOKEN_LET:
            case TOKEN_MATCH:
            case TOKEN_CONST:
            case TOKEN_TRAIT:
            case TOKEN_CATCH:
            case TOKEN_IF:
            case TOKEN_FINALLY:
            case TOKEN_FOR:
            case TOKEN_WHILE:
            case TOKEN_TRY:
            case TOKEN_THROW:
            case TOKEN_BREAK:
            case TOKEN_CASE:
            case TOKEN_CONTINUE:
            case TOKEN_DEFAULT:
            case TOKEN_ELIF:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
            case TOKEN_SWITCH:
            case TOKEN_RIGHT_BRACE:
                return;
            default:
                break;
        }

        advanceParser();
    }
}

static void optionalSemicolon() {
    match(TOKEN_SEMICOLON);
}

static bool matchForClauseSeparator() {
    return match(TOKEN_COMMA);
}

static void consumeForClauseSeparator() {
    if (matchForClauseSeparator()) {
        return;
    }

    if (match(TOKEN_SEMICOLON)) {
        error("Use ',' instead of ';' between for clauses.");
        return;
    }

    errorAtCurrent("Expect ',' between for clauses.");
}

static void emitScopeCleanup(int targetDepth) {
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; --i) {
        const Local& local = current->locals[static_cast<std::size_t>(i)];
        if (local.depth <= targetDepth) {
            break;
        }

        if (local.isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
    }
}

static void emitTryCleanup(int targetDepth) {
    for (TryCompiler* tryCompiler = currentTry;
         tryCompiler != nullptr && tryCompiler->scopeDepth > targetDepth;
         tryCompiler = tryCompiler->enclosing) {
        emitByte(OP_POP_EXCEPTION_HANDLER);
    }
}

static int createHiddenLocal(const char* name) {
    emitNil();
    Token token = syntheticToken(TOKEN_IDENTIFIER, name, parser.previous.line);
    addLocal(token, false);
    markInitialized();
    return static_cast<int>(current->locals.size()) - 1;
}

static void emitStoreLocalAndPop(int slot) {
    emitBytes(OP_SET_LOCAL, static_cast<uint8_t>(slot));
    emitByte(OP_POP);
}

static void emitSetLocalToNumber(int slot, int value) {
    emitConstant(Value::numberValue(static_cast<double>(value)));
    emitStoreLocalAndPop(slot);
}

static void emitSetLocalToNil(int slot) {
    emitNil();
    emitStoreLocalAndPop(slot);
}

static PendingTransfer* ensurePendingTransfer(TryCompiler* tryCompiler,
                                              TransferKind kind,
                                              int continueTarget,
                                              int cleanupDepth) {
    for (PendingTransfer& transfer : tryCompiler->transfers) {
        if (transfer.kind == kind &&
            transfer.continueTarget == continueTarget &&
            transfer.cleanupDepth == cleanupDepth) {
            return &transfer;
        }
    }

    PendingTransfer transfer;
    transfer.id = tryCompiler->nextTransferId++;
    transfer.kind = kind;
    transfer.continueTarget = continueTarget;
    transfer.cleanupDepth = cleanupDepth;
    tryCompiler->transfers.push_back(transfer);
    return &tryCompiler->transfers.back();
}

static void patchJumpListToCurrent(const std::vector<int>& offsets) {
    for (int offset : offsets) {
        patchJump(offset);
    }
}

static void patchTryTransfersToCurrent(TryCompiler* tryCompiler) {
    patchJumpListToCurrent(tryCompiler->afterTryJumps);
    for (const PendingTransfer& transfer : tryCompiler->transfers) {
        patchJumpListToCurrent(transfer.jumpsToTarget);
    }
}

static void emitTryTransfer(TransferKind kind,
                            bool storeTopValue,
                            int continueTarget,
                            int cleanupDepth) {
    if (currentTry == nullptr || !currentTry->active) {
        return;
    }

    if (storeTopValue) {
        emitStoreLocalAndPop(currentTry->valueLocal);
    }

    PendingTransfer* transfer =
        ensurePendingTransfer(currentTry, kind, continueTarget, cleanupDepth);
    emitSetLocalToNumber(currentTry->actionLocal, transfer->id);

    if (currentTry->handlerActive) {
        emitByte(OP_POP_EXCEPTION_HANDLER);
    }

    emitScopeCleanup(currentTry->scopeDepth);
    transfer->jumpsToTarget.push_back(emitJump(OP_JUMP));
}

static void emitTransferContinuation(const TryCompiler& tryCompiler,
                                     const PendingTransfer& transfer) {
    emitSetLocalToNil(tryCompiler.actionLocal);

    if (currentTry != nullptr) {
        switch (transfer.kind) {
            case TRANSFER_RETURN_VALUE:
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(tryCompiler.valueLocal));
                emitTryTransfer(TRANSFER_RETURN_VALUE, true, 0, -1);
                return;
            case TRANSFER_RETURN_NIL:
                emitTryTransfer(TRANSFER_RETURN_NIL, false, 0, -1);
                return;
            case TRANSFER_BREAK:
                emitTryTransfer(TRANSFER_BREAK, false, 0, transfer.cleanupDepth);
                return;
            case TRANSFER_CONTINUE:
                emitTryTransfer(TRANSFER_CONTINUE, false, transfer.continueTarget,
                                transfer.cleanupDepth);
                return;
            case TRANSFER_THROW:
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(tryCompiler.valueLocal));
                emitTryTransfer(TRANSFER_THROW, true, 0, -1);
                return;
        }
    }

    switch (transfer.kind) {
        case TRANSFER_RETURN_VALUE:
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(tryCompiler.valueLocal));
            emitByte(OP_RETURN);
            return;
        case TRANSFER_RETURN_NIL:
            emitReturn();
            return;
        case TRANSFER_BREAK:
            emitScopeCleanup(transfer.cleanupDepth);
            if (currentLoop != nullptr) {
                currentLoop->breakJumps.push_back(emitJump(OP_JUMP));
            }
            return;
        case TRANSFER_CONTINUE:
            emitScopeCleanup(transfer.cleanupDepth);
            emitLoop(transfer.continueTarget);
            return;
        case TRANSFER_THROW:
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(tryCompiler.valueLocal));
            emitByte(OP_THROW);
            return;
    }
}

static void emitGlobalLookupByName(const std::string& name) {
    emitConstantIndex(
        OP_GET_GLOBAL, OP_GET_GLOBAL_LONG,
        makeConstant(Value::stringValue(name)));
}

static void emitPropertyLookupByName(const std::string& name) {
    emitConstantIndex(
        OP_GET_PROPERTY, OP_GET_PROPERTY_LONG,
        makeConstant(Value::stringValue(name)));
}

static void emitCallGlobalOneArg(const std::string& name, int localSlot) {
    emitGlobalLookupByName(name);
    emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(localSlot));
    emitBytes(OP_CALL, 1);
}

static void emitPatternFailJump(std::vector<int>* failJumps) {
    failJumps->push_back(emitJump(OP_JUMP_IF_FALSE));
    emitByte(OP_POP);
}

static void patchPatternFailJumps(const std::vector<int>& failJumps) {
    if (failJumps.empty()) {
        return;
    }

    for (int offset : failJumps) {
        patchJump(offset);
    }
    emitByte(OP_POP);
}

static void emitLiteralTokenValue(const Token& token) {
    switch (token.type) {
        case TOKEN_FALSE:
            emitFalse();
            return;
        case TOKEN_TRUE:
            emitTrue();
            return;
        case TOKEN_NIL:
            emitNil();
            return;
        case TOKEN_NUMBER: {
            std::string lexeme(token.start, token.length);
            if (lexeme.find('.') == std::string::npos && lexeme.find('e') == std::string::npos && lexeme.find('E') == std::string::npos) {
                emitConstant(Value::intValue(static_cast<int64_t>(std::strtoll(lexeme.c_str(), nullptr, 10))));
            } else {
                emitConstant(Value::numberValue(std::strtod(lexeme.c_str(), nullptr)));
            }
            return;
        }
        case TOKEN_STRING: {
            std::string lexeme(token.start + 1, token.length - 2);
            emitConstant(Value::stringValue(lexeme));
            return;
        }
        default:
            error("Unsupported literal pattern.");
            emitNil();
            return;
    }
}

static void emitPathPatternValue(const std::string& path) {
    std::size_t start = 0;
    std::size_t dot = path.find('.');
    if (dot == std::string::npos) {
        emitGlobalLookupByName(path);
        return;
    }

    emitGlobalLookupByName(path.substr(0, dot));
    start = dot + 1;
    while (start < path.size()) {
        dot = path.find('.', start);
        std::string member =
            dot == std::string::npos
                ? path.substr(start)
                : path.substr(start, dot - start);
        emitPropertyLookupByName(member);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
}

static std::shared_ptr<Pattern> parsePattern() {
    std::shared_ptr<Pattern> pattern = std::make_shared<Pattern>();

    if (match(TOKEN_LEFT_BRACKET)) {
        pattern->kind = PATTERN_ARRAY;
        if (!check(TOKEN_RIGHT_BRACKET)) {
            do {
                pattern->elements.push_back(parsePattern());
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_BRACKET, "Expect ']' after array pattern.");
        return pattern;
    }

    if (match(TOKEN_LEFT_BRACE)) {
        pattern->kind = PATTERN_OBJECT;
        if (!check(TOKEN_RIGHT_BRACE)) {
            do {
                PatternField field;

                if (match(TOKEN_STRING)) {
                    const Token keyToken = parser.previous;
                    field.key = std::string(keyToken.start + 1, keyToken.length - 2);
                    consume(TOKEN_COLON, "Expect ':' after object pattern string key.");
                    field.pattern = parsePattern();
                } else {
                    Token keyToken =
                        consume(TOKEN_IDENTIFIER, "Expect object pattern field name.");
                    field.key = tokenLexeme(keyToken);
                    if (match(TOKEN_COLON)) {
                        field.pattern = parsePattern();
                    } else {
                        field.pattern = std::make_shared<Pattern>();
                        if (field.key == "_") {
                            field.pattern->kind = PATTERN_WILDCARD;
                        } else {
                            field.pattern->kind = PATTERN_BINDING;
                            field.pattern->name = field.key;
                        }
                    }
                }

                pattern->fields.push_back(field);
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_BRACE, "Expect '}' after object pattern.");
        return pattern;
    }

    if (match(TOKEN_FALSE) || match(TOKEN_TRUE) || match(TOKEN_NIL) ||
        match(TOKEN_NUMBER) || match(TOKEN_STRING)) {
        pattern->kind = PATTERN_LITERAL;
        pattern->literalToken = parser.previous;
        return pattern;
    }

    if (match(TOKEN_IDENTIFIER)) {
        const Token identifier = parser.previous;
        const std::string name = tokenLexeme(identifier);
        if (name == "_") {
            pattern->kind = PATTERN_WILDCARD;
            return pattern;
        }

        if (match(TOKEN_DOT)) {
            pattern->kind = PATTERN_PATH;
            pattern->name = name;
            do {
                Token member =
                    consume(TOKEN_IDENTIFIER, "Expect member name in pattern path.");
                pattern->name += ".";
                pattern->name += tokenLexeme(member);
            } while (match(TOKEN_DOT));
            return pattern;
        }

        pattern->kind = PATTERN_BINDING;
        pattern->name = name;
        return pattern;
    }

    errorAtCurrent("Expect a pattern.");
    return pattern;
}

static void collectPatternBindings(const std::shared_ptr<Pattern>& pattern,
                                   std::vector<std::string>* names) {
    if (pattern == nullptr) {
        return;
    }

    switch (pattern->kind) {
        case PATTERN_BINDING:
            names->push_back(pattern->name);
            return;
        case PATTERN_ARRAY:
            for (const auto& element : pattern->elements) {
                collectPatternBindings(element, names);
            }
            return;
        case PATTERN_OBJECT:
            for (const PatternField& field : pattern->fields) {
                collectPatternBindings(field.pattern, names);
            }
            return;
        default:
            return;
    }
}

static void declarePatternBindings(const std::shared_ptr<Pattern>& pattern, bool isConst) {
    std::vector<std::string> names;
    collectPatternBindings(pattern, &names);
    for (const std::string& name : names) {
        Token token = ownedIdentifierToken(name, parser.previous.line);
        declareVariable(token, isConst);
        if (current->scopeDepth > 0) {
            emitUnset();
        }
    }
}

static bool validateBindingPattern(const std::shared_ptr<Pattern>& pattern) {
    if (pattern == nullptr) {
        return true;
    }

    switch (pattern->kind) {
        case PATTERN_WILDCARD:
        case PATTERN_BINDING:
            return true;
        case PATTERN_ARRAY:
            for (const auto& element : pattern->elements) {
                if (!validateBindingPattern(element)) {
                    return false;
                }
            }
            return true;
        case PATTERN_OBJECT:
            for (const PatternField& field : pattern->fields) {
                if (!validateBindingPattern(field.pattern)) {
                    return false;
                }
            }
            return true;
        case PATTERN_LITERAL:
        case PATTERN_PATH:
            error("Literal and path patterns are only allowed inside match statements.");
            return false;
    }

    return false;
}

static void initializeBindingTarget(const std::string& name,
                                    bool isConst,
                                    int targetDepth) {
    Token token = ownedIdentifierToken(name, parser.previous.line);
    int local = findDeclaredLocalSlot(current, token);
    if (local != -1) {
        emitBytes(OP_SET_LOCAL, static_cast<uint8_t>(local));
        emitByte(OP_POP);
        current->locals[static_cast<std::size_t>(local)].depth = targetDepth;
        return;
    }

    int global = identifierConstant(token);
    if (isConst) {
        emitConstantIndex(OP_DEFINE_CONST_GLOBAL, OP_DEFINE_CONST_GLOBAL_LONG, global);
    } else {
        emitConstantIndex(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_LONG, global);
    }
}

static void compilePatternTestFromLocal(const std::shared_ptr<Pattern>& pattern,
                                        int valueSlot,
                                        std::vector<int>* failJumps) {
    if (pattern == nullptr) {
        return;
    }

    switch (pattern->kind) {
        case PATTERN_WILDCARD:
        case PATTERN_BINDING:
            return;
        case PATTERN_LITERAL:
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
            emitLiteralTokenValue(pattern->literalToken);
            emitByte(OP_EQUAL);
            emitPatternFailJump(failJumps);
            return;
        case PATTERN_PATH:
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
            emitPathPatternValue(pattern->name);
            emitByte(OP_EQUAL);
            emitPatternFailJump(failJumps);
            return;
        case PATTERN_ARRAY:
            emitCallGlobalOneArg("isArray", valueSlot);
            emitPatternFailJump(failJumps);
            emitCallGlobalOneArg("len", valueSlot);
            emitConstant(Value::numberValue(static_cast<double>(pattern->elements.size())));
            emitByte(OP_EQUAL);
            emitPatternFailJump(failJumps);

            for (std::size_t index = 0; index < pattern->elements.size(); ++index) {
                beginScope();
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
                emitConstant(Value::numberValue(static_cast<double>(index)));
                emitByte(OP_GET_INDEX);
                Token temp = syntheticToken(TOKEN_IDENTIFIER, "@match_item", parser.previous.line);
                addLocal(temp, false);
                markInitialized();
                int nestedSlot = static_cast<int>(current->locals.size()) - 1;
                compilePatternTestFromLocal(pattern->elements[index], nestedSlot, failJumps);
                endScope();
            }
            return;
        case PATTERN_OBJECT: {
            emitCallGlobalOneArg("isMap", valueSlot);
            int tryInstance = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);
            int typeOkJump = emitJump(OP_JUMP);

            patchJump(tryInstance);
            emitByte(OP_POP);
            emitCallGlobalOneArg("isInstance", valueSlot);
            int tryClass = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);
            int classOkJump = emitJump(OP_JUMP);

            patchJump(tryClass);
            emitByte(OP_POP);
            emitCallGlobalOneArg("isClass", valueSlot);
            emitPatternFailJump(failJumps);

            patchJump(typeOkJump);
            patchJump(classOkJump);

            for (const PatternField& field : pattern->fields) {
                beginScope();
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
                emitPropertyLookupByName(field.key);
                Token temp = syntheticToken(TOKEN_IDENTIFIER, "@match_field", parser.previous.line);
                addLocal(temp, false);
                markInitialized();
                int nestedSlot = static_cast<int>(current->locals.size()) - 1;
                compilePatternTestFromLocal(field.pattern, nestedSlot, failJumps);
                endScope();
            }
            return;
        }
    }
}

static void compilePatternBindingsFromLocal(const std::shared_ptr<Pattern>& pattern,
                                            int valueSlot,
                                            bool isConst,
                                            int targetDepth) {
    if (pattern == nullptr) {
        return;
    }

    switch (pattern->kind) {
        case PATTERN_WILDCARD:
        case PATTERN_LITERAL:
        case PATTERN_PATH:
            return;
        case PATTERN_BINDING:
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
            initializeBindingTarget(pattern->name, isConst, targetDepth);
            return;
        case PATTERN_ARRAY:
            for (std::size_t index = 0; index < pattern->elements.size(); ++index) {
                beginScope();
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
                emitConstant(Value::numberValue(static_cast<double>(index)));
                emitByte(OP_GET_INDEX);
                Token temp = syntheticToken(TOKEN_IDENTIFIER, "@bind_item", parser.previous.line);
                addLocal(temp, false);
                markInitialized();
                int nestedSlot = static_cast<int>(current->locals.size()) - 1;
                compilePatternBindingsFromLocal(pattern->elements[index], nestedSlot,
                                                isConst, targetDepth);
                endScope();
            }
            return;
        case PATTERN_OBJECT:
            for (const PatternField& field : pattern->fields) {
                beginScope();
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(valueSlot));
                emitPropertyLookupByName(field.key);
                Token temp = syntheticToken(TOKEN_IDENTIFIER, "@bind_field", parser.previous.line);
                addLocal(temp, false);
                markInitialized();
                int nestedSlot = static_cast<int>(current->locals.size()) - 1;
                compilePatternBindingsFromLocal(field.pattern, nestedSlot,
                                                isConst, targetDepth);
                endScope();
            }
            return;
    }
}

static void namedVariable(Token name, bool canAssign) {
    clearResolvedCallable();
    bool isConst = false;
    int local = resolveLocal(current, name, &isConst);
    if (local != -1) {
        if (canAssign && match(TOKEN_EQUAL)) {
            if (isConst) {
                error("Cannot assign to a const variable.");
            }

            assignment();
            std::string varType = getVariableType(name);
            if (isConcreteTypeAnnotation(varType) && isConcreteTypeAnnotation(lastEmittedType)) {
                reportTypeMismatch(varType, lastEmittedType,
                                   "assignment to " + tokenLexeme(name));
            }

            emitBytes(OP_SET_LOCAL, static_cast<uint8_t>(local));
        } else {
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(local));
            setType(getVariableType(name));
        }
        return;
    }

    int upvalue = resolveUpvalue(current, name, &isConst);
    if (upvalue != -1) {
        if (canAssign && match(TOKEN_EQUAL)) {
            if (isConst) {
                error("Cannot assign to a const variable.");
            }

            assignment();
            std::string varType = getVariableType(name);
            if (isConcreteTypeAnnotation(varType) && isConcreteTypeAnnotation(lastEmittedType)) {
                reportTypeMismatch(varType, lastEmittedType,
                                   "assignment to " + tokenLexeme(name));
            }

            emitBytes(OP_SET_UPVALUE, static_cast<uint8_t>(upvalue));
        } else {
            emitBytes(OP_GET_UPVALUE, static_cast<uint8_t>(upvalue));
            setType(getVariableType(name));
        }
        return;
    }

    bool isGlobalConst = false;
    resolveGlobalSymbol(name, &isGlobalConst);

    if (canAssign && match(TOKEN_EQUAL)) {
        if (isGlobalConst) {
            error("Cannot assign to a const variable.");
        }

        assignment();
        std::string varType = getVariableType(name);
        if (isConcreteTypeAnnotation(varType) && isConcreteTypeAnnotation(lastEmittedType)) {
            reportTypeMismatch(varType, lastEmittedType,
                               "assignment to " + tokenLexeme(name));
        }
        emitConstantIndex(OP_SET_GLOBAL, OP_SET_GLOBAL_LONG, identifierConstant(name));
        return;
    }

    emitConstantIndex(OP_GET_GLOBAL, OP_GET_GLOBAL_LONG, identifierConstant(name));
    FunctionPtr declared = lookupDeclaredFunction(tokenLexeme(name));
    if (declared != nullptr) {
        setType("Function");
        lastResolvedCallable = declared;
    } else {
        setType(getVariableType(name));
    }
}

static void finishCall() {
    FunctionPtr calleeFunction = lastResolvedCallable;
    clearResolvedCallable();
    uint8_t argCount = 0;
    bool hasNamedArgs = false;
    std::vector<uint16_t> namedOperands;
    std::vector<std::string> argTypes;
    std::vector<std::string> argNames;

    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (argCount == UINT8_MAX) {
                error("Cannot pass more than 255 arguments to a function.");
            }

            if (check(TOKEN_IDENTIFIER) && tokenImmediatelyFollowedByColon(parser.current)) {
                Token argumentName = consume(TOKEN_IDENTIFIER, "Expect argument name.");
                consume(TOKEN_COLON, "Expect ':' after named argument.");
                expression();
                int constant = identifierConstant(argumentName);
                namedOperands.push_back(static_cast<uint16_t>(constant + 1));
                hasNamedArgs = true;
                argTypes.push_back(lastEmittedType);
                argNames.push_back(tokenLexeme(argumentName));
            } else {
                expression();
                namedOperands.push_back(0);
                argTypes.push_back(lastEmittedType);
                argNames.push_back(std::string());
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    std::string inferredReturnType = "Any";
    if (calleeFunction != nullptr) {
        std::string calleeName =
            calleeFunction->name.empty() ? "<fn>" : calleeFunction->name;
        validateStaticCall(calleeFunction, argTypes, argNames, calleeName,
                           &inferredReturnType);
    }
    clearResolvedCallable();
    setType(inferredReturnType);

    if (!hasNamedArgs) {
        emitBytes(OP_CALL, argCount);
        return;
    }

    emitByte(OP_CALL_NAMED);
    emitByte(argCount);
    for (uint16_t operand : namedOperands) {
        emitShort(operand);
    }
}

static void call(bool canAssign) {
    primary(canAssign);

    for (;;) {
        if (match(TOKEN_LEFT_PAREN)) {
            finishCall();
            continue;
        }

        if (match(TOKEN_LEFT_BRACKET)) {
            std::string receiverType = lastEmittedType;
            expression();
            consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

            if (canAssign && match(TOKEN_EQUAL)) {
                assignment();
                emitByte(OP_SET_INDEX);
            } else {
                emitByte(OP_GET_INDEX);
                setType(indexedAccessResultType(receiverType));
                clearResolvedCallable();
            }
            continue;
        }

        if (match(TOKEN_DOT)) {
            std::string receiverType = lastEmittedType;
            Token propertyName =
                consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
            int property = identifierConstant(propertyName);

            if (canAssign && match(TOKEN_EQUAL)) {
                assignment();
                emitConstantIndex(OP_SET_PROPERTY, OP_SET_PROPERTY_LONG, property);
            } else {
                emitConstantIndex(OP_GET_PROPERTY, OP_GET_PROPERTY_LONG, property);
                setType(propertyAccessResultType(receiverType, tokenLexeme(propertyName)));
                clearResolvedCallable();
            }
            continue;
        }

        break;
    }
}

static void bitwiseOr(bool canAssign);
static void bitwiseXor(bool canAssign);
static void bitwiseAnd(bool canAssign);
static void shift(bool canAssign);

static void unary(bool canAssign) {
    if (match(TOKEN_AWAIT)) {
        unary(false);
        emitByte(OP_AWAIT);
        setType("Any");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_BANG)) {
        unary(false);
        emitByte(OP_NOT);
        setType("Bool");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_TILDE)) {
        unary(false);
        emitByte(OP_BITNOT);
        setType("Number");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_MINUS)) {
        unary(false);
        if (isConcreteTypeAnnotation(lastEmittedType) &&
            !areTypesCompatible("Number", lastEmittedType)) {
            reportTypeMismatch("Number", lastEmittedType, "unary '-'");
        }
        emitByte(OP_NEGATE);
        setType("Number");
        clearResolvedCallable();
        return;
    }

    call(canAssign);
}

static void factor(bool canAssign) {
    unary(canAssign);

    while (match(TOKEN_STAR) || match(TOKEN_SLASH) || match(TOKEN_PERCENT)) {
        TokenType op = parser.previous.type;
        std::string leftType = lastEmittedType;
        unary(false);
        std::string rightType = lastEmittedType;
        if (isConcreteTypeAnnotation(leftType) && isConcreteTypeAnnotation(rightType)) {
            reportTypeMismatch("Number", leftType,
                               op == TOKEN_STAR ? "operator '*'" : (op == TOKEN_SLASH ? "operator '/'" : "operator '%'"));
            reportTypeMismatch("Number", rightType,
                               op == TOKEN_STAR ? "operator '*'" : (op == TOKEN_SLASH ? "operator '/'" : "operator '%'"));
        }

        if (op == TOKEN_STAR) {
            emitByte(OP_MULTIPLY);
        } else if (op == TOKEN_SLASH) {
            emitByte(OP_DIVIDE);
        } else {
            emitByte(OP_MODULO);
        }
        setType("Number");
        clearResolvedCallable();
    }
}

static void term(bool canAssign) {
    factor(canAssign);

    while (match(TOKEN_PLUS) || match(TOKEN_MINUS)) {
        TokenType op = parser.previous.type;
        std::string leftType = lastEmittedType;
        factor(false);
        std::string rightType = lastEmittedType;

        if (op == TOKEN_PLUS) {
            emitByte(OP_ADD);
            if (areTypesCompatible("Number", leftType) &&
                areTypesCompatible("Number", rightType)) {
                setType("Number");
            } else if (areTypesCompatible("String", leftType) &&
                       areTypesCompatible("String", rightType)) {
                setType("String");
            } else {
                setType("Any");
            }
        } else {
            emitByte(OP_SUBTRACT);
            if (isConcreteTypeAnnotation(leftType) && isConcreteTypeAnnotation(rightType)) {
                reportTypeMismatch("Number", leftType, "operator '-'");
                reportTypeMismatch("Number", rightType, "operator '-'");
            }
            setType("Number");
        }
        clearResolvedCallable();
    }
}

static void shift(bool canAssign) {
    term(canAssign);

    while (match(TOKEN_LESS_LESS) || match(TOKEN_GREATER_GREATER)) {
        TokenType op = parser.previous.type;
        term(false);
        if (op == TOKEN_LESS_LESS) {
            emitByte(OP_SHL);
        } else {
            emitByte(OP_SHR);
        }
        setType("Number");
        clearResolvedCallable();
    }
}

static void bitwiseAnd(bool canAssign) {
    shift(canAssign);

    while (match(TOKEN_AMPERSAND)) {
        shift(false);
        emitByte(OP_BITAND);
        setType("Number");
        clearResolvedCallable();
    }
}

static void bitwiseXor(bool canAssign) {
    bitwiseAnd(canAssign);

    while (match(TOKEN_CARET)) {
        bitwiseAnd(false);
        emitByte(OP_BITXOR);
        setType("Number");
        clearResolvedCallable();
    }
}

static void bitwiseOr(bool canAssign) {
    bitwiseXor(canAssign);

    while (match(TOKEN_PIPE)) {
        bitwiseXor(false);
        emitByte(OP_BITOR);
        setType("Number");
        clearResolvedCallable();
    }
}

static void comparison(bool canAssign) {
    bitwiseOr(canAssign);

    while (match(TOKEN_GREATER) || match(TOKEN_GREATER_EQUAL) ||
           match(TOKEN_LESS) || match(TOKEN_LESS_EQUAL)) {
        TokenType op = parser.previous.type;
        std::string leftType = lastEmittedType;
        bitwiseOr(false);
        std::string rightType = lastEmittedType;
        if (isConcreteTypeAnnotation(leftType) && isConcreteTypeAnnotation(rightType)) {
            reportTypeMismatch("Number", leftType, "comparison");
            reportTypeMismatch("Number", rightType, "comparison");
        }

        switch (op) {
            case TOKEN_GREATER:
                emitByte(OP_GREATER);
                break;
            case TOKEN_GREATER_EQUAL:
                emitByte(OP_LESS);
                emitByte(OP_NOT);
                break;
            case TOKEN_LESS:
                emitByte(OP_LESS);
                break;
            case TOKEN_LESS_EQUAL:
                emitByte(OP_GREATER);
                emitByte(OP_NOT);
                break;
            default:
                break;
        }
        setType("Bool");
        clearResolvedCallable();
    }
}

static void equality(bool canAssign) {
    comparison(canAssign);

    while (match(TOKEN_BANG_EQUAL) || match(TOKEN_EQUAL_EQUAL)) {
        TokenType op = parser.previous.type;
        comparison(false);
        emitByte(OP_EQUAL);

        if (op == TOKEN_BANG_EQUAL) {
            emitByte(OP_NOT);
        }
        setType("Bool");
        clearResolvedCallable();
    }
}

static void andExpression(bool canAssign) {
    equality(canAssign);

    while (match(TOKEN_AND)) {
        std::string leftType = lastEmittedType;
        int endJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
        equality(false);
        std::string rightType = lastEmittedType;
        patchJump(endJump);
        if (areTypesCompatible("Bool", leftType) && areTypesCompatible("Bool", rightType)) {
            setType("Bool");
        } else {
            setType("Any");
        }
        clearResolvedCallable();
    }
}

static void orExpression(bool canAssign) {
    andExpression(canAssign);

    while (match(TOKEN_OR)) {
        std::string leftType = lastEmittedType;
        int elseJump = emitJump(OP_JUMP_IF_FALSE);
        int endJump = emitJump(OP_JUMP);

        patchJump(elseJump);
        emitByte(OP_POP);
        andExpression(false);
        std::string rightType = lastEmittedType;
        patchJump(endJump);
        if (areTypesCompatible("Bool", leftType) && areTypesCompatible("Bool", rightType)) {
            setType("Bool");
        } else {
            setType("Any");
        }
        clearResolvedCallable();
    }
}

static void ternary(bool canAssign) {
    orExpression(canAssign);

    if (!match(TOKEN_QUESTION)) {
        return;
    }

    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    assignment();
    std::string trueType = lastEmittedType;
    consume(TOKEN_COLON, "Expect ':' in ternary expression.");
    int endJump = emitJump(OP_JUMP);

    patchJump(elseJump);
    emitByte(OP_POP);
    ternary(false);
    std::string falseType = lastEmittedType;
    patchJump(endJump);
    if (areTypesCompatible(trueType, falseType)) {
        setType(trueType);
    } else if (areTypesCompatible(falseType, trueType)) {
        setType(falseType);
    } else {
        setType("Any");
    }
    clearResolvedCallable();
}

static void assignment() {
    ternary(true);

    if (match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
        assignment();
    }
}

static void expression() {
    assignment();
}

static void collectionLiteral() {
    if (match(TOKEN_RIGHT_BRACKET)) {
        emitBytes(OP_ARRAY, 0);
        setType("Array<Any>");
        clearResolvedCallable();
        return;
    }

    int count = 1;
    expression();
    std::string firstType = lastEmittedType;

    if (match(TOKEN_COLON)) {
        std::vector<std::string> mapValueTypes;
        expression();
        mapValueTypes.push_back(lastEmittedType);

        while (match(TOKEN_COMMA)) {
            if (count == UINT8_MAX) {
                error("Cannot have more than 255 entries in a map literal.");
            }

            count++;
            expression();
            consume(TOKEN_COLON, "Expect ':' after map key.");
            expression();
            mapValueTypes.push_back(lastEmittedType);
        }

        consume(TOKEN_RIGHT_BRACKET, "Expect ']' after map literal.");
        emitBytes(OP_MAP, static_cast<uint8_t>(count));
        setType(inferMapLiteralType(mapValueTypes));
        clearResolvedCallable();
        return;
    }

    std::vector<std::string> arrayElementTypes;
    arrayElementTypes.push_back(firstType);
    while (match(TOKEN_COMMA)) {
        if (count == UINT8_MAX) {
            error("Cannot have more than 255 elements in an array literal.");
        }

        count++;
        expression();
        arrayElementTypes.push_back(lastEmittedType);
    }

    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after array literal.");
    emitBytes(OP_ARRAY, static_cast<uint8_t>(count));
    setType(inferArrayLiteralType(arrayElementTypes));
    clearResolvedCallable();
}

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void primary(bool canAssign) {
    if (match(TOKEN_FALSE)) {
        emitFalse();
        setType("Bool");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_TRUE)) {
        emitTrue();
        setType("Bool");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_NIL)) {
        emitNil();
        setType("Nil");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_NUMBER)) {
        std::string lexeme(parser.previous.start, parser.previous.length);
        if (lexeme.find('.') == std::string::npos && lexeme.find('e') == std::string::npos && lexeme.find('E') == std::string::npos) {
            emitConstant(Value::intValue(static_cast<int64_t>(std::strtoll(lexeme.c_str(), nullptr, 10))));
            setType("int");
        } else {
            emitConstant(Value::numberValue(std::strtod(lexeme.c_str(), nullptr)));
            setType("float");
        }
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_STRING)) {
        std::string lexeme(parser.previous.start + 1, parser.previous.length - 2);
        emitConstant(Value::stringValue(lexeme));
        setType("String");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_LEFT_BRACKET)) {
        collectionLiteral();
        return;
    }

    if (match(TOKEN_THIS)) {
        if (currentClass == nullptr) {
            error("Cannot use 'this' outside of a class.");
            emitNil();
            setType("Nil");
            return;
        }

        namedVariable(parser.previous, canAssign);
        setType("Instance");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_SUPER)) {
        if (currentClass == nullptr) {
            error("Cannot use 'super' outside of a class.");
            emitNil();
            return;
        }

        if (!currentClass->hasSuperclass) {
            error("Cannot use 'super' in a class with no superclass.");
            emitNil();
            return;
        }

        consume(TOKEN_DOT, "Expect '.' after 'super'.");
        Token methodName = consume(TOKEN_IDENTIFIER, "Expect superclass method name.");

        emitConstantIndex(
            OP_GET_GLOBAL, OP_GET_GLOBAL_LONG,
            identifierConstant(currentClass->superclassName));
        namedVariable(syntheticToken(TOKEN_THIS, "this", methodName.line), false);
        emitConstantIndex(OP_GET_SUPER, OP_GET_SUPER_LONG, identifierConstant(methodName));
        setType("Function");
        clearResolvedCallable();
        return;
    }

    if (match(TOKEN_IDENTIFIER)) {
        namedVariable(parser.previous, canAssign);
        return;
    }

    if (match(TOKEN_LEFT_PAREN)) {
        expression();
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
        return;
    }

    errorAtCurrent("Expect expression.");
}

static FunctionPtr function(const Token& name, FunctionType type, bool isAsync) {
    Compiler compiler;
    initCompiler(&compiler, type, std::string(name.start, name.length), isAsync);
    current->function->genericParameters = parseOptionalGenericParameterList();
    declaredFunctions[tokenLexeme(name)] = current->function;

    if (type == TYPE_METHOD) {
        if (isAsync && tokenMatchesLiteral(name, "init")) {
            error("Async init() is not supported.");
        }

        Token thisToken;
        thisToken.type = TOKEN_THIS;
        thisToken.start = "this";
        thisToken.length = 4;
        thisToken.line = name.line;

        addLocal(thisToken, true);
        markInitialized();
        current->function->arity = 1;
        current->function->minArity = 1;
    }

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    bool sawDefaultParameter = false;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                errorAtCurrent("Functions cannot have more than 255 parameters.");
            }

            Token parameter = consume(TOKEN_IDENTIFIER, "Expect parameter name.");
            std::string parameterType;
            if (match(TOKEN_COLON)) {
                parameterType = captureTypeAnnotation(
                    {TOKEN_COMMA, TOKEN_RIGHT_PAREN, TOKEN_EQUAL});
            }

            if (parameterType.empty()) {
                parameterType = "Any";
            }

            declareVariable(parameter, false, parameterType);
            markInitialized();
            current->function->parameterNames.push_back(tokenLexeme(parameter));
            current->function->parameterTypes.push_back(normalizeTypeAnnotation(parameterType));

            if (match(TOKEN_EQUAL)) {
                sawDefaultParameter = true;
                int parameterSlot = findDeclaredLocalSlot(current, parameter);
                emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(parameterSlot));
                emitUnset();
                emitByte(OP_EQUAL);
                int skipDefaultJump = emitJump(OP_JUMP_IF_FALSE);
                emitByte(OP_POP);
                expression();
                if (isConcreteTypeAnnotation(parameterType) &&
                    isConcreteTypeAnnotation(lastEmittedType)) {
                    reportTypeMismatch(parameterType, lastEmittedType,
                                       "default value for parameter '" +
                                           tokenLexeme(parameter) + "'");
                }
                emitBytes(OP_SET_LOCAL, static_cast<uint8_t>(parameterSlot));
                emitByte(OP_POP);
                int endDefaultJump = emitJump(OP_JUMP);
                patchJump(skipDefaultJump);
                emitByte(OP_POP);
                patchJump(endDefaultJump);
            } else {
                if (sawDefaultParameter) {
                    error("Required parameters cannot follow default parameters.");
                }
                current->function->minArity++;
            }
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    if (match(TOKEN_COLON)) {
        current->function->returnType = captureTypeAnnotation({TOKEN_LEFT_BRACE});
        current->function->returnType =
            normalizeTypeAnnotation(current->function->returnType);
    }
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    FunctionPtr functionValue = endCompiler();
    int constant = makeConstant(Value::functionValue(functionValue));
    emitConstantIndex(OP_CLOSURE, OP_CLOSURE_LONG, constant);
    for (const Upvalue& upvalue : compiler.upvalues) {
        emitByte(upvalue.isLocal ? 1 : 0);
        emitByte(upvalue.index);
    }
    return functionValue;
}

static void enumDeclaration() {
    Token enumName = consume(TOKEN_IDENTIFIER, "Expect enum name after 'enum'.");
    if (enumName.type != TOKEN_IDENTIFIER) {
        return;
    }

    parseOptionalGenericParameterList();
    consume(TOKEN_LEFT_BRACE, "Expect '{' before enum body.");

    declareVariable(enumName, true, "Map");
    int enumConstant = identifierConstant(enumName);

    int entryCount = 0;
    int nextOrdinal = 0;
    bool canAutoAssign = true;
    if (!check(TOKEN_RIGHT_BRACE)) {
        do {
            if (entryCount == UINT8_MAX) {
                error("Enums cannot contain more than 255 members.");
            }

            Token memberName = consume(TOKEN_IDENTIFIER, "Expect enum member name.");
            emitConstant(Value::stringValue(tokenLexeme(memberName)));

            if (match(TOKEN_EQUAL)) {
                if (check(TOKEN_NUMBER)) {
                    Token numericValue = consume(TOKEN_NUMBER, "Expect enum value.");
                    std::string lexeme(numericValue.start, numericValue.length);
                    double parsed = std::strtod(lexeme.c_str(), nullptr);
                    emitConstant(Value::numberValue(parsed));
                    if (parsed == std::trunc(parsed)) {
                        nextOrdinal = static_cast<int>(parsed) + 1;
                        canAutoAssign = true;
                    } else {
                        canAutoAssign = false;
                    }
                } else {
                    expression();
                    canAutoAssign = false;
                }
            } else {
                if (!canAutoAssign) {
                    error("Implicit enum values cannot follow a non-numeric explicit enum value.");
                    emitNil();
                } else {
                    emitConstant(Value::numberValue(static_cast<double>(nextOrdinal++)));
                }
            }

            entryCount++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after enum body.");
    emitBytes(OP_MAP, static_cast<uint8_t>(entryCount));
    defineVariable(enumConstant, true);
}

static void contractDeclaration(bool isTrait) {
    Token name = consume(
        TOKEN_IDENTIFIER,
        isTrait ? "Expect trait name after 'trait'." : "Expect interface name after 'interface'.");
    if (name.type != TOKEN_IDENTIFIER) {
        return;
    }

    ContractDeclaration contract;
    contract.name = tokenLexeme(name);
    contract.isTrait = isTrait;
    contract.genericParameters = parseOptionalGenericParameterList();

    consume(TOKEN_LEFT_BRACE, "Expect '{' before contract body.");
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        bool isAsyncMethod = match(TOKEN_ASYNC);
        consume(TOKEN_FN, "Contracts can only contain fn signatures.");
        Token methodName = consume(TOKEN_IDENTIFIER, "Expect contract method name.");
        parseOptionalGenericParameterList();
        consume(TOKEN_LEFT_PAREN, "Expect '(' after contract method name.");

        int arity = 0;
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                consume(TOKEN_IDENTIFIER, "Expect contract parameter name.");
                if (match(TOKEN_COLON)) {
                    captureTypeAnnotation({TOKEN_COMMA, TOKEN_RIGHT_PAREN, TOKEN_EQUAL});
                }
                if (match(TOKEN_EQUAL)) {
                    error("Contract methods cannot declare default parameter values.");
                    skipTokensUntil({TOKEN_COMMA, TOKEN_RIGHT_PAREN});
                }
                arity++;
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after contract parameters.");
        if (match(TOKEN_COLON)) {
            captureTypeAnnotation({TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE, TOKEN_SEMICOLON});
        }

        if (match(TOKEN_LEFT_BRACE)) {
            error("Contract methods cannot have bodies.");
            skipBlockTokens();
        } else {
            optionalSemicolon();
        }

        contract.methods.push_back({tokenLexeme(methodName), arity, isAsyncMethod});
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after contract body.");
    declaredContracts.push_back(contract);
}

static void classDeclaration() {
    Token className = consume(TOKEN_IDENTIFIER, "Expect class name after 'class'.");
    if (className.type != TOKEN_IDENTIFIER) {
        return;
    }

    parseOptionalGenericParameterList();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after class name.");
    Token superclassName = parser.current;
    bool hasSuperclass = false;
    if (!check(TOKEN_RIGHT_PAREN)) {
        superclassName = consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        hasSuperclass = superclassName.type == TOKEN_IDENTIFIER;
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after class parameters.");

    std::vector<std::string> implementedContracts;
    if (match(TOKEN_IMPLEMENTS)) {
        do {
            Token contractName =
                consume(TOKEN_IDENTIFIER, "Expect trait or interface name after 'implements'.");
            implementedContracts.push_back(tokenLexeme(contractName));
            parseOptionalGenericParameterList();
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    if (current->type == TYPE_SCRIPT &&
        current->scopeDepth == 0 &&
        !hasSuperclass &&
        tokenMatchesLiteral(className, "main")) {
        block();
        return;
    }

    if (hasSuperclass && identifiersEqual(className, superclassName)) {
        error("A class cannot inherit from itself.");
    }

    declareVariable(className, true, "Class");
    int classNameConstant = identifierConstant(className);
    emitConstantIndex(OP_CLASS, OP_CLASS_LONG, classNameConstant);
    defineVariable(classNameConstant, true);
    namedVariable(className, false);

    if (hasSuperclass) {
        namedVariable(superclassName, false);
        emitByte(OP_INHERIT);
    }

    ClassCompiler classCompiler;
    classCompiler.enclosing = currentClass;
    classCompiler.name = className;
    classCompiler.superclassName = superclassName;
    classCompiler.hasSuperclass = hasSuperclass;
    currentClass = &classCompiler;
    std::vector<ContractMethod> definedMethods;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        bool isAsyncMethod = false;
        if (match(TOKEN_ASYNC)) {
            isAsyncMethod = true;
        }

        if (!match(TOKEN_FN)) {
            errorAtCurrent("Only fn or async fn methods are allowed inside a class body.");
            synchronize();
            continue;
        }

        Token methodName = consume(TOKEN_IDENTIFIER, "Expect method name after 'fn'.");
        if (methodName.type != TOKEN_IDENTIFIER) {
            continue;
        }

        FunctionPtr methodFunction = function(methodName, TYPE_METHOD, isAsyncMethod);
        emitConstantIndex(OP_METHOD, OP_METHOD_LONG, identifierConstant(methodName));
        definedMethods.push_back({
            tokenLexeme(methodName),
            methodFunction == nullptr ? 0 : methodFunction->arity - 1,
            isAsyncMethod,
        });
    }
    currentClass = currentClass->enclosing;

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");
    emitByte(OP_POP);

    if (!implementedContracts.empty()) {
        for (const std::string& contractName : implementedContracts) {
            const ContractDeclaration* contract = nullptr;
            for (const ContractDeclaration& candidate : declaredContracts) {
                if (candidate.name == contractName) {
                    contract = &candidate;
                    break;
                }
            }

            if (contract == nullptr) {
                std::string message =
                    "Unknown contract '" + contractName + "' in implements clause.";
                error(message.c_str());
                continue;
            }

            for (const ContractMethod& requiredMethod : contract->methods) {
                bool foundName = false;
                bool foundExact = false;
                int definedArity = 0;
                bool definedAsync = false;

                for (const ContractMethod& definedMethod : definedMethods) {
                    if (definedMethod.name == requiredMethod.name) {
                        foundName = true;
                        definedArity = definedMethod.arity;
                        definedAsync = definedMethod.isAsync;
                        if (definedMethod.arity == requiredMethod.arity &&
                            definedMethod.isAsync == requiredMethod.isAsync) {
                            foundExact = true;
                            break;
                        }
                    }
                }

                if (!foundExact) {
                    std::string contractTypeStr = (contract->isTrait ? "trait" : "interface");
                    if (foundName) {
                        if (definedArity != requiredMethod.arity) {
                            std::string message =
                                "Class '" + tokenLexeme(className) + "' implements " + contractTypeStr + " '" +
                                contractName + "' but method '" + requiredMethod.name + "' has arity mismatch: expected " +
                                std::to_string(requiredMethod.arity) + " but got " + std::to_string(definedArity) + ".";
                            error(message.c_str());
                        } else if (definedAsync != requiredMethod.isAsync) {
                            std::string message =
                                "Class '" + tokenLexeme(className) + "' implements " + contractTypeStr + " '" +
                                contractName + "' but method '" + requiredMethod.name + "' async mismatch: expected " +
                                (requiredMethod.isAsync ? "async fn" : "fn") + " but got " + (definedAsync ? "async fn" : "fn") + ".";
                            error(message.c_str());
                        }
                    } else {
                        std::string message =
                            "Class '" + tokenLexeme(className) + "' does not satisfy " + contractTypeStr + " '" +
                            contractName + "': missing method '" + requiredMethod.name + "'.";
                        error(message.c_str());
                    }
                }
            }
        }
    }
}

static void functionDeclaration(bool isAsync) {
    Token name = consume(TOKEN_IDENTIFIER, "Expect function name after 'fn'.");
    if (name.type != TOKEN_IDENTIFIER) {
        return;
    }

    declareVariable(name, true, "Function");
    function(name, TYPE_FUNCTION, isAsync);
    defineVariable(identifierConstant(name), true);
}

static void destructuringVariableDeclaration(bool isConst) {
    int targetDepth = current->scopeDepth;
    std::shared_ptr<Pattern> pattern = parsePattern();
    if (!validateBindingPattern(pattern)) {
        return;
    }

    declarePatternBindings(pattern, isConst);
    consume(TOKEN_EQUAL, "Expect '=' after destructuring pattern.");
    expression();

    beginScope();
    Token temp = syntheticToken(TOKEN_IDENTIFIER, "@destructure_value", parser.previous.line);
    addLocal(temp, false);
    markInitialized();
    int tempSlot = static_cast<int>(current->locals.size()) - 1;
    compilePatternBindingsFromLocal(pattern, tempSlot, isConst, targetDepth);
    endScope();
    optionalSemicolon();
}

static void variableDeclaration(bool isConst) {
    if (check(TOKEN_LEFT_BRACKET) || check(TOKEN_LEFT_BRACE)) {
        destructuringVariableDeclaration(isConst);
        return;
    }

    Token name = consume(TOKEN_IDENTIFIER, "Expect variable name.");
    if (name.type != TOKEN_IDENTIFIER) {
        return;
    }

    std::string typeAnn = "Any";
    if (match(TOKEN_COLON)) {
        typeAnn = captureTypeAnnotation({TOKEN_EQUAL});
    }

    declareVariable(name, isConst, typeAnn);
    consume(TOKEN_EQUAL, "Expect '=' after variable name.");
    expression();
    
    if (isConcreteTypeAnnotation(typeAnn) && isConcreteTypeAnnotation(lastEmittedType)) {
        reportTypeMismatch(typeAnn, lastEmittedType, "variable '" + tokenLexeme(name) + "'");
    }

    defineVariable(identifierConstant(name), isConst);
    optionalSemicolon();
}

static void forVariableDeclaration(bool isConst) {
    Token name = consume(TOKEN_IDENTIFIER, "Expect variable name.");
    if (name.type != TOKEN_IDENTIFIER) {
        return;
    }

    std::string typeAnn = "Any";
    if (match(TOKEN_COLON)) {
        typeAnn = captureTypeAnnotation({TOKEN_EQUAL});
    }

    declareVariable(name, isConst, typeAnn);
    consume(TOKEN_EQUAL, "Expect '=' after variable name.");
    expression();

    if (isConcreteTypeAnnotation(typeAnn) && isConcreteTypeAnnotation(lastEmittedType)) {
        reportTypeMismatch(typeAnn, lastEmittedType, "loop variable '" + tokenLexeme(name) + "'");
    }

    defineVariable(identifierConstant(name), isConst);
    consumeForClauseSeparator();
}

static void printStatement() {
    bool hasParentheses = match(TOKEN_LEFT_PAREN);
    expression();

    if (hasParentheses) {
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after value.");
    }

    emitByte(OP_PRINT);
    optionalSemicolon();
}

static void returnStatement() {
    if (current->type == TYPE_SCRIPT) {
        error("Cannot return from top-level code.");
        if (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF) && !check(TOKEN_SEMICOLON)) {
            expression();
        }
        optionalSemicolon();
        return;
    }

    if (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF) && !check(TOKEN_SEMICOLON)) {
        if (isInitializer()) {
            error("Cannot return a value from init().");
            expression();
            emitByte(OP_POP);
            if (currentTry != nullptr) {
                emitTryTransfer(TRANSFER_RETURN_NIL, false, 0, -1);
                optionalSemicolon();
                return;
            }
            emitTryCleanup(-1);
            emitReturn();
            optionalSemicolon();
            return;
        }

        expression();
        if (isConcreteTypeAnnotation(current->function->returnType) &&
            isConcreteTypeAnnotation(lastEmittedType)) {
            reportTypeMismatch(current->function->returnType, lastEmittedType,
                               "return from '" + current->function->name + "'");
        }
        if (currentTry != nullptr) {
            emitTryTransfer(TRANSFER_RETURN_VALUE, true, 0, -1);
            optionalSemicolon();
            return;
        }
        emitTryCleanup(-1);
        emitByte(OP_RETURN);
        optionalSemicolon();
        return;
    }

    if (isConcreteTypeAnnotation(current->function->returnType) &&
        !areTypesCompatible(current->function->returnType, "Nil")) {
        error(("Return from '" + current->function->name +
               "' is missing a value required by type '" +
               current->function->returnType + "'.")
                  .c_str());
    }

    if (currentTry != nullptr) {
        emitTryTransfer(TRANSFER_RETURN_NIL, false, 0, -1);
        optionalSemicolon();
        return;
    }

    emitTryCleanup(-1);
    emitReturn();
    optionalSemicolon();
}

static void throwStatement() {
    expression();
    emitByte(OP_THROW);
    optionalSemicolon();
}

static void breakStatement() {
    if (currentLoop == nullptr) {
        error("Cannot use 'break' outside of a loop.");
        optionalSemicolon();
        return;
    }

    if (currentTry != nullptr) {
        emitTryTransfer(TRANSFER_BREAK, false, 0, currentLoop->breakScopeDepth);
        optionalSemicolon();
        return;
    }

    emitTryCleanup(currentLoop->breakScopeDepth);
    emitScopeCleanup(currentLoop->breakScopeDepth);
    currentLoop->breakJumps.push_back(emitJump(OP_JUMP));
    optionalSemicolon();
}

static void continueStatement() {
    if (currentLoop == nullptr) {
        error("Cannot use 'continue' outside of a loop.");
        optionalSemicolon();
        return;
    }

    if (currentTry != nullptr) {
        emitTryTransfer(TRANSFER_CONTINUE, false,
                        currentLoop->continueTarget, currentLoop->continueScopeDepth);
        optionalSemicolon();
        return;
    }

    emitTryCleanup(currentLoop->continueScopeDepth);
    emitScopeCleanup(currentLoop->continueScopeDepth);
    emitLoop(currentLoop->continueTarget);
    optionalSemicolon();
}

static void tryStatement() {
    beginScope();

    TryCompiler tryCompiler;
    tryCompiler.enclosing = currentTry;
    tryCompiler.scopeDepth = current->scopeDepth;
    tryCompiler.actionLocal = createHiddenLocal("@try_action");
    tryCompiler.valueLocal = createHiddenLocal("@try_value");
    tryCompiler.nextTransferId = 1;
    tryCompiler.active = true;
    tryCompiler.handlerActive = true;
    tryCompiler.compiledCatch = false;
    tryCompiler.compiledFinally = false;
    tryCompiler.afterTryJumps.clear();
    tryCompiler.transfers.clear();
    currentTry = &tryCompiler;

    consume(TOKEN_LEFT_BRACE, "Expect '{' after 'try'.");
    int handlerOffset = emitExceptionHandler();

    beginScope();
    block();
    endScope();

    emitByte(OP_POP_EXCEPTION_HANDLER);
    tryCompiler.afterTryJumps.push_back(emitJump(OP_JUMP));
    tryCompiler.handlerActive = false;

    patchExceptionHandler(handlerOffset);

    bool hasCatch = false;
    if (match(TOKEN_CATCH)) {
        hasCatch = true;
        tryCompiler.compiledCatch = true;

        consume(TOKEN_LEFT_PAREN, "Expect '(' after 'catch'.");
        Token catchName = consume(TOKEN_IDENTIFIER, "Expect catch variable name.");
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after catch variable.");
        consume(TOKEN_LEFT_BRACE, "Expect '{' before catch block.");

        beginScope();
        addLocal(catchName, false);
        markInitialized();
        block();
        endScope();
    }

    bool hasFinally = false;
    if (match(TOKEN_FINALLY)) {
        hasFinally = true;
        tryCompiler.compiledFinally = true;
    }

    if (!hasCatch && !hasFinally) {
        error("Expect 'catch' or 'finally' after try block.");
        currentTry = tryCompiler.enclosing;
        patchJumpListToCurrent(tryCompiler.afterTryJumps);
        endScope();
        return;
    }

    if (!hasCatch) {
        PendingTransfer* throwTransfer =
            ensurePendingTransfer(&tryCompiler, TRANSFER_THROW, 0, -1);
        emitStoreLocalAndPop(tryCompiler.valueLocal);
        emitSetLocalToNumber(tryCompiler.actionLocal, throwTransfer->id);
        throwTransfer->jumpsToTarget.push_back(emitJump(OP_JUMP));
    }

    currentTry = tryCompiler.enclosing;

    if (hasFinally) {
        patchTryTransfersToCurrent(&tryCompiler);
        consume(TOKEN_LEFT_BRACE, "Expect '{' after 'finally'.");
        beginScope();
        block();
        endScope();
    }

    if (!hasFinally) {
        patchTryTransfersToCurrent(&tryCompiler);
    }

    if (!tryCompiler.transfers.empty()) {
        for (const PendingTransfer& transfer : tryCompiler.transfers) {
            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(tryCompiler.actionLocal));
            emitConstant(Value::numberValue(static_cast<double>(transfer.id)));
            emitByte(OP_EQUAL);
            int nextTransferCheck = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);
            emitTransferContinuation(tryCompiler, transfer);
            patchJump(nextTransferCheck);
            emitByte(OP_POP);
        }
    }

    endScope();
}

static void ifClauseStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after condition keyword.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    int elseJump = emitJump(OP_JUMP);

    patchJump(thenJump);
    emitByte(OP_POP);

    if (match(TOKEN_ELIF)) {
        ifClauseStatement();
    } else if (match(TOKEN_ELSE)) {
        statement();
    }

    patchJump(elseJump);
}

static void ifStatement() {
    ifClauseStatement();
}

static void switchStatement() {
    beginScope();
    int switchSlot = createHiddenLocal("@switch_value");

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
    emitStoreLocalAndPop(switchSlot);
    consume(TOKEN_LEFT_BRACE, "Expect '{' after switch header.");

    std::vector<int> endJumps;
    bool sawDefault = false;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_CASE)) {
            if (sawDefault) {
                error("Cannot place a case after default.");
            }

            emitBytes(OP_GET_LOCAL, static_cast<uint8_t>(switchSlot));
            expression();
            emitByte(OP_EQUAL);
            int nextCaseJump = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);

            consume(TOKEN_LEFT_BRACE, "Expect '{' before case body.");
            beginScope();
            block();
            endScope();
            endJumps.push_back(emitJump(OP_JUMP));

            patchJump(nextCaseJump);
            emitByte(OP_POP);
            continue;
        }

        if (match(TOKEN_DEFAULT)) {
            if (sawDefault) {
                error("Switch can only have one default block.");
            }

            sawDefault = true;
            consume(TOKEN_LEFT_BRACE, "Expect '{' before default body.");
            beginScope();
            block();
            endScope();
            continue;
        }

        errorAtCurrent("Expect 'case', 'default', or '}' inside switch.");
        synchronize();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after switch body.");
    patchJumpListToCurrent(endJumps);
    endScope();
}

static void matchStatement() {
    beginScope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'match'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after match value.");

    Token temp = syntheticToken(TOKEN_IDENTIFIER, "@match_value", parser.previous.line);
    addLocal(temp, false);
    markInitialized();
    int matchSlot = static_cast<int>(current->locals.size()) - 1;

    consume(TOKEN_LEFT_BRACE, "Expect '{' after match header.");
    std::vector<int> endJumps;
    bool sawDefault = false;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_CASE)) {
            if (sawDefault) {
                error("Cannot place a case after default in match.");
            }

            std::shared_ptr<Pattern> pattern = parsePattern();
            beginScope();
            int caseDepth = current->scopeDepth;
            declarePatternBindings(pattern, false);

            std::vector<int> failJumps;
            compilePatternTestFromLocal(pattern, matchSlot, &failJumps);
            compilePatternBindingsFromLocal(pattern, matchSlot, false, caseDepth);

            consume(TOKEN_LEFT_BRACE, "Expect '{' before match case body.");
            block();
            endScope();
            endJumps.push_back(emitJump(OP_JUMP));
            patchPatternFailJumps(failJumps);
            continue;
        }

        if (match(TOKEN_DEFAULT)) {
            if (sawDefault) {
                error("Match can only have one default block.");
            }

            sawDefault = true;
            consume(TOKEN_LEFT_BRACE, "Expect '{' before match default body.");
            beginScope();
            block();
            endScope();
            continue;
        }

        errorAtCurrent("Expect 'case', 'default', or '}' inside match.");
        synchronize();
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after match body.");
    patchJumpListToCurrent(endJumps);
    endScope();
}

static void whileStatement() {
    int loopStart = static_cast<int>(currentChunk()->code.size());
    LoopCompiler loop;
    loop.enclosing = currentLoop;
    loop.breakScopeDepth = current->scopeDepth;
    loop.continueScopeDepth = current->scopeDepth;
    loop.continueTarget = loopStart;
    loop.breakJumps.clear();
    currentLoop = &loop;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);
    for (int jump : loop.breakJumps) {
        patchJump(jump);
    }
    currentLoop = loop.enclosing;
}

static void expressionStatement() {
    expression();
    emitByte(OP_POP);
    optionalSemicolon();
}

static void forStatement() {
    beginScope();
    int forScopeDepth = current->scopeDepth;
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    if (matchForClauseSeparator()) {
    } else if (match(TOKEN_LET)) {
        forVariableDeclaration(false);
    } else if (match(TOKEN_CONST)) {
        forVariableDeclaration(true);
    } else {
        expression();
        emitByte(OP_POP);
        consumeForClauseSeparator();
    }

    int loopStart = static_cast<int>(currentChunk()->code.size());
    int exitJump = -1;
    int continueTarget = loopStart;

    if (!matchForClauseSeparator()) {
        expression();
        consumeForClauseSeparator();
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }

    if (!match(TOKEN_RIGHT_PAREN)) {
        int bodyJump = emitJump(OP_JUMP);
        int incrementStart = static_cast<int>(currentChunk()->code.size());

        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emitLoop(loopStart);
        loopStart = incrementStart;
        continueTarget = incrementStart;
        patchJump(bodyJump);
    }

    LoopCompiler loop;
    loop.enclosing = currentLoop;
    loop.breakScopeDepth = forScopeDepth - 1;
    loop.continueScopeDepth = forScopeDepth;
    loop.continueTarget = continueTarget;
    loop.breakJumps.clear();
    currentLoop = &loop;

    statement();
    emitLoop(loopStart);
    currentLoop = loop.enclosing;

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);
    }

    endScope();
    for (int jump : loop.breakJumps) {
        patchJump(jump);
    }
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
        return;
    }

    if (match(TOKEN_RETURN)) {
        returnStatement();
        return;
    }

    if (match(TOKEN_IF)) {
        ifStatement();
        return;
    }

    if (match(TOKEN_SWITCH)) {
        switchStatement();
        return;
    }

    if (match(TOKEN_MATCH)) {
        matchStatement();
        return;
    }

    if (match(TOKEN_WHILE)) {
        whileStatement();
        return;
    }

    if (match(TOKEN_TRY)) {
        tryStatement();
        return;
    }

    if (match(TOKEN_THROW)) {
        throwStatement();
        return;
    }

    if (match(TOKEN_FOR)) {
        forStatement();
        return;
    }

    if (match(TOKEN_BREAK)) {
        breakStatement();
        return;
    }

    if (match(TOKEN_DEBUGGER)) {
        emitByte(OP_BREAKPOINT);
        optionalSemicolon();
        return;
    }

    if (match(TOKEN_CONTINUE)) {
        continueStatement();
        return;
    }

    if (match(TOKEN_ELIF)) {
        error("Cannot use 'elif' without a matching 'if'.");
        return;
    }

    if (match(TOKEN_CASE) || match(TOKEN_DEFAULT)) {
        error("Cannot use switch branches outside of a switch.");
        return;
    }

    if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
        return;
    }

    expressionStatement();
}

static void declaration() {
    if (check(TOKEN_RIGHT_BRACE)) {
        errorAtCurrent("Unexpected '}' without a matching block.");
        advanceParser();
        parser.panicMode = false;
        return;
    }

    if (match(TOKEN_CLASS)) {
        classDeclaration();
    } else if (match(TOKEN_ENUM)) {
        enumDeclaration();
    } else if (match(TOKEN_INTERFACE)) {
        contractDeclaration(false);
    } else if (match(TOKEN_TRAIT)) {
        contractDeclaration(true);
    } else if (match(TOKEN_ASYNC)) {
        consume(TOKEN_FN, "Expect 'fn' after 'async'.");
        functionDeclaration(true);
    } else if (match(TOKEN_FN)) {
        functionDeclaration(false);
    } else if (match(TOKEN_LET)) {
        variableDeclaration(false);
    } else if (match(TOKEN_CONST)) {
        variableDeclaration(true);
    } else {
        statement();
    }

    if (parser.panicMode) {
        synchronize();
    }
}

bool compile(const char* source, FunctionPtr* function, const std::string& filename) {
    currentCompileFilename = filename;
    initLexer(source);
    parser.hadError = false;
    parser.panicMode = false;
    currentClass = nullptr;
    currentLoop = nullptr;
    currentTry = nullptr;
    declaredContracts.clear();
    declaredFunctions.clear();
    ownedSyntheticLexemes.clear();
    lastResolvedCallable = nullptr;
    lastEmittedType = "Any";

    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT, "");

    advanceParser();

    while (!check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_EOF, "Expect end of file.");
    FunctionPtr compiled = endCompiler();

    if (function != nullptr) {
        *function = compiled;
    }

    if (g_optimizerLevel > 0) {
        optimizeFunctionTree(compiled, g_optimizerLevel);
    }

    return !parser.hadError;
}
