#include "tooling.h"
#include "chunk.h"
#include "compiler.h"
#include "lexer.h"
#include "package_manager.h"
#include "source_loader.h"
#include "system_native.h"
#include "urc.h"
#include "vm.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct TokenView {
    std::string text;
};

struct LspDocumentRecord {
    std::string uri;
    std::string text;
    std::filesystem::path path;
};

struct IndexedSymbol {
    std::string name;
    std::string detail;
    std::string hoverText;
    std::filesystem::path filePath;
    std::filesystem::path targetPath;
    std::string targetName;
    int line = 1;
    int column = 1;
    int endLine = 1;
    int endColumn = 1;
    int symbolKind = 13;
    int completionKind = 6;
    bool isImport = false;
    bool isModuleAlias = false;
};

struct SymbolIndex {
    std::filesystem::path filePath;
    std::string uri;
    std::vector<IndexedSymbol> symbols;
    std::unordered_map<std::string, std::size_t> firstByName;
};

struct SymbolIndexCache {
    std::unordered_map<std::string, SymbolIndex> indices;
    std::unordered_set<std::string> building;
};

bool pathExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode);
}

bool fileExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_regular_file(path, errorCode);
}

bool directoryExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_directory(path, errorCode);
}

static bool isAbsolutePath(const std::filesystem::path& path) {
    if (path.empty()) return false;
    if (path.is_absolute()) return true;
    std::string s = path.generic_string();
    if (s[0] == '/') return true;
    return false;
}

std::filesystem::path canonicalize(const std::filesystem::path& path) {
    std::error_code errorCode;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        return std::filesystem::absolute(path);
    }
    return result;
}

std::string displayPath(const std::filesystem::path& path) {
    return path.generic_string();
}

bool readTextFile(const std::filesystem::path& path,
                  std::string* content,
                  std::string* errorMessage = nullptr) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not open '" + displayPath(path) + "'.";
        }
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    *content = buffer.str();
    return true;
}

bool writeTextFile(const std::filesystem::path& path,
                   const std::string& content,
                   std::string* errorMessage = nullptr) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not create directory '" +
                            displayPath(path.parent_path()) + "'.";
        }
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not write '" + displayPath(path) + "'.";
        }
        return false;
    }

    file << content;
    return file.good();
}

std::string lowerCase(std::string value) {
    for (char& current : value) {
        current = static_cast<char>(std::tolower(static_cast<unsigned char>(current)));
    }
    return value;
}

bool shouldSkipToolingDirectory(const std::filesystem::path& path) {
    std::string name = lowerCase(path.filename().string());
    return name == "compiled" ||
           name == ".uranium" ||
           name == ".git" ||
           name == ".vs" ||
           name == "build" ||
           name.rfind("build", 0) == 0;
}

void discoverUraniumSourceFiles(const std::filesystem::path& target,
                                std::vector<std::filesystem::path>* files) {
    if (fileExists(target)) {
        if (lowerCase(target.extension().string()) == ".ur") {
            files->push_back(canonicalize(target));
        }
        return;
    }

    std::error_code errorCode;
    std::filesystem::recursive_directory_iterator iterator(target, errorCode);
    std::filesystem::recursive_directory_iterator end;
    while (!errorCode && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_directory(errorCode)) {
            if (shouldSkipToolingDirectory(entry.path())) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(errorCode);
            continue;
        }

        if (!errorCode &&
            entry.is_regular_file(errorCode) &&
            lowerCase(entry.path().extension().string()) == ".ur") {
            files->push_back(canonicalize(entry.path()));
        }

        iterator.increment(errorCode);
    }
}

std::vector<std::string> splitLines(const std::string& source, bool* hadTrailingNewline) {
    std::vector<std::string> lines;
    std::string current;
    *hadTrailingNewline = !source.empty() && source.back() == '\n';

    for (char c : source) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }

    if (!current.empty() || source.empty() || !*hadTrailingNewline) {
        if (!current.empty() && current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }

    return lines;
}

bool isIdentifierStart(char c) {
    unsigned char ch = static_cast<unsigned char>(c);
    return std::isalpha(ch) || c == '_';
}

bool isIdentifierPart(char c) {
    unsigned char ch = static_cast<unsigned char>(c);
    return std::isalnum(ch) || c == '_';
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) {
        start++;
    }

    std::size_t end = value.size();
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        end--;
    }

    return value.substr(start, end - start);
}

std::size_t findLineCommentStart(const std::string& line);
std::size_t firstNonWhitespace(const std::string& line);

std::string stripLineComment(const std::string& line) {
    std::size_t commentStart = findLineCommentStart(line);
    if (commentStart == std::string::npos) {
        return line;
    }
    return line.substr(0, commentStart);
}

bool startsWithKeywordAt(const std::string& text,
                         std::size_t index,
                         const std::string& keyword) {
    if (text.compare(index, keyword.size(), keyword) != 0) {
        return false;
    }

    std::size_t end = index + keyword.size();
    return end >= text.size() || !isIdentifierPart(text[end]);
}

void skipInlineWhitespace(const std::string& text, std::size_t* index) {
    while (*index < text.size() &&
           (text[*index] == ' ' || text[*index] == '\t' || text[*index] == '\r')) {
        (*index)++;
    }
}

bool readIdentifierAt(const std::string& text,
                      std::size_t* index,
                      std::string* value) {
    skipInlineWhitespace(text, index);
    if (*index >= text.size() || !isIdentifierStart(text[*index])) {
        return false;
    }

    std::size_t start = *index;
    (*index)++;
    while (*index < text.size() && isIdentifierPart(text[*index])) {
        (*index)++;
    }

    *value = text.substr(start, *index - start);
    return true;
}

bool readImportTokenAt(const std::string& text,
                       std::size_t* index,
                       std::string* value) {
    skipInlineWhitespace(text, index);
    if (*index >= text.size()) {
        return false;
    }

    if (text[*index] == '"') {
        std::size_t start = *index;
        (*index)++;
        while (*index < text.size()) {
            if (text[*index] == '"' && text[*index - 1] != '\\') {
                (*index)++;
                *value = text.substr(start, *index - start);
                return true;
            }
            (*index)++;
        }
        return false;
    }

    std::size_t start = *index;
    while (*index < text.size() &&
           text[*index] != ' ' &&
           text[*index] != '\t' &&
           text[*index] != ',' &&
           text[*index] != ';') {
        (*index)++;
    }

    if (start == *index) {
        return false;
    }

    *value = text.substr(start, *index - start);
    return true;
}

bool readAliasTokenAt(const std::string& text,
                      std::size_t* index,
                      std::string* value) {
    std::string token;
    if (!readImportTokenAt(text, index, &token)) {
        return false;
    }

    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        token = token.substr(1, token.size() - 2);
    }

    if (token.empty() || !isIdentifierStart(token[0])) {
        return false;
    }

    for (char current : token) {
        if (!isIdentifierPart(current)) {
            return false;
        }
    }

    *value = token;
    return true;
}

int updateBraceDepthForLine(const std::string& line, int currentDepth) {
    bool inString = false;
    bool inComment = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        char current = line[index];
        char previous = index > 0 ? line[index - 1] : '\0';

        if (inComment) {
            break;
        }

        if (current == '"' && previous != '\\') {
            inString = !inString;
            continue;
        }

        if (inString) {
            continue;
        }

        if (current == '/' && index + 1 < line.size() && line[index + 1] == '/') {
            inComment = true;
            continue;
        }

        if (current == '{') {
            currentDepth++;
        } else if (current == '}') {
            currentDepth--;
        }
    }

    return currentDepth;
}

std::string moduleDefaultAlias(const std::filesystem::path& resolvedPath) {
    std::string stem = resolvedPath.stem().string();
    if (stem == "index") {
        stem = resolvedPath.parent_path().filename().string();
    }
    return stem;
}

int findNameColumnInLine(const std::string& line,
                         const std::string& name,
                         std::size_t searchFrom = 0) {
    std::size_t position = line.find(name, searchFrom);
    if (position == std::string::npos) {
        position = line.find(name);
        if (position == std::string::npos) {
            return 1;
        }
    }
    return static_cast<int>(position) + 1;
}

void addIndexedSymbol(SymbolIndex* index, const IndexedSymbol& symbol) {
    if (index == nullptr) {
        return;
    }

    std::size_t offset = index->symbols.size();
    index->symbols.push_back(symbol);
    if (index->firstByName.find(symbol.name) == index->firstByName.end()) {
        index->firstByName[symbol.name] = offset;
    }
}

std::size_t firstNonWhitespace(const std::string& line) {
    std::size_t index = 0;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
        index++;
    }
    return index;
}

void addDiagnostic(std::vector<ToolDiagnostic>* diagnostics,
                   int line,
                   int column,
                   int endColumn,
                   int severity,
                   const std::string& code,
                   const std::string& message) {
    ToolDiagnostic diagnostic;
    diagnostic.line = std::max(1, line);
    diagnostic.column = std::max(1, column);
    diagnostic.endLine = diagnostic.line;
    diagnostic.endColumn = std::max(diagnostic.column + 1, endColumn);
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostics->push_back(std::move(diagnostic));
}

std::size_t findLineCommentStart(const std::string& line) {
    bool inString = false;

    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        if (line[index] == '"' && (index == 0 || line[index - 1] != '\\')) {
            inString = !inString;
        }

        if (!inString && line[index] == '/' && line[index + 1] == '/') {
            return index;
        }
    }

    return std::string::npos;
}

std::vector<TokenView> tokenizeCodeLine(const std::string& code) {
    std::vector<TokenView> tokens;
    std::size_t index = 0;

    while (index < code.size()) {
        char current = code[index];
        if (current == ' ' || current == '\t' || current == '\r') {
            index++;
            continue;
        }

        if (current == '"') {
            std::size_t start = index++;
            while (index < code.size()) {
                if (code[index] == '"' && code[index - 1] != '\\') {
                    index++;
                    break;
                }
                index++;
            }
            tokens.push_back({code.substr(start, index - start)});
            continue;
        }

        if (isIdentifierStart(current)) {
            std::size_t start = index++;
            while (index < code.size() && isIdentifierPart(code[index])) {
                index++;
            }
            tokens.push_back({code.substr(start, index - start)});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(current))) {
            std::size_t start = index++;
            while (index < code.size() &&
                   (std::isdigit(static_cast<unsigned char>(code[index])) ||
                    code[index] == '.')) {
                index++;
            }
            tokens.push_back({code.substr(start, index - start)});
            continue;
        }

        if (index + 1 < code.size()) {
            std::string two = code.substr(index, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
                tokens.push_back({two});
                index += 2;
                continue;
            }
        }

        tokens.push_back({std::string(1, current)});
        index++;
    }

    return tokens;
}

bool isWordLikeToken(const std::string& token) {
    if (token.empty()) {
        return false;
    }

    char first = token[0];
    return isIdentifierStart(first) ||
           std::isdigit(static_cast<unsigned char>(first)) ||
           first == '"';
}

bool isBinaryOperatorToken(const std::string& token) {
    return token == "+" ||
           token == "-" ||
           token == "*" ||
           token == "/" ||
           token == "=" ||
           token == "==" ||
           token == "!=" ||
           token == "<" ||
           token == "<=" ||
           token == ">" ||
           token == ">=" ||
           token == "and" ||
           token == "or" ||
           token == "?" ||
           token == ":";
}

bool wantsSpaceBeforeParen(const std::string& previous) {
    return previous == "if" ||
           previous == "elif" ||
           previous == "while" ||
           previous == "for" ||
           previous == "switch" ||
           previous == "catch";
}

bool shouldInsertSpace(const std::string& previous, const std::string& current) {
    if (previous.empty()) {
        return false;
    }

    if (current == "," || current == ";" || current == ")" || current == "]" ||
        current == "}" || current == ".") {
        return false;
    }

    if (previous == "(" || previous == "[" || previous == "{" || previous == ".") {
        return false;
    }

    if (current == "(") {
        return wantsSpaceBeforeParen(previous);
    }

    if (current == "[") {
        return false;
    }

    if (current == "{") {
        return previous != "{";
    }

    if (previous == ",") {
        return true;
    }

    if (previous == "!") {
        return false;
    }

    if (isBinaryOperatorToken(previous) || isBinaryOperatorToken(current)) {
        return true;
    }

    if (isWordLikeToken(previous) && isWordLikeToken(current)) {
        return true;
    }

    return false;
}

std::string formatCodeLine(const std::string& line) {
    std::size_t commentStart = findLineCommentStart(line);
    std::string code = commentStart == std::string::npos
                           ? line
                           : line.substr(0, commentStart);
    std::string comment = commentStart == std::string::npos
                              ? ""
                              : line.substr(commentStart);
    std::string trimmedCode = trim(code);

    if (trimmedCode.empty()) {
        return comment.empty() ? std::string() : comment;
    }

    std::vector<TokenView> tokens = tokenizeCodeLine(trimmedCode);
    std::string formatted;
    std::string previous;
    for (const TokenView& token : tokens) {
        if (shouldInsertSpace(previous, token.text)) {
            formatted.push_back(' ');
        }
        formatted.append(token.text);
        previous = token.text;
    }

    if (!comment.empty()) {
        if (!formatted.empty()) {
            formatted.append(" ");
        }
        formatted.append(comment);
    }

    return formatted;
}

int countLeadingClosingBraces(const std::string& trimmedLine) {
    int count = 0;
    while (count < static_cast<int>(trimmedLine.size()) && trimmedLine[count] == '}') {
        count++;
    }
    return count;
}

int updateBraceDepth(const std::string& line, int depth) {
    bool inString = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        char current = line[index];
        char previous = index > 0 ? line[index - 1] : '\0';

        if (current == '"' && previous != '\\') {
            inString = !inString;
            continue;
        }

        if (inString) {
            continue;
        }

        if (current == '/' &&
            index + 1 < line.size() &&
            line[index + 1] == '/') {
            break;
        }

        if (current == '{') {
            depth++;
        } else if (current == '}') {
            depth = std::max(0, depth - 1);
        }
    }

    return depth;
}

bool formatUraniumSourceImpl(const std::string& source,
                             int indentSize,
                             std::string* formattedSource) {
    bool hadTrailingNewline = false;
    std::vector<std::string> lines = splitLines(source, &hadTrailingNewline);
    int braceDepth = 0;

    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::string trimmedLine = trim(lines[index]);
        if (trimmedLine.empty()) {
            if (index + 1 < lines.size() || hadTrailingNewline) {
                output << "\n";
            }
            continue;
        }

        int indentLevel = braceDepth;
        indentLevel = std::max(0, indentLevel - countLeadingClosingBraces(trimmedLine));
        output << std::string(static_cast<std::size_t>(indentLevel * indentSize), ' ');
        output << formatCodeLine(trimmedLine);

        if (index + 1 < lines.size() || hadTrailingNewline) {
            output << "\n";
        }

        braceDepth = updateBraceDepth(trimmedLine, braceDepth);
    }

    *formattedSource = output.str();
    return true;
}

void lintBraceBalance(const std::string& source,
                      std::vector<ToolDiagnostic>* diagnostics) {
    struct OpenBracket {
        char bracket;
        int line;
        int column;
    };

    std::vector<OpenBracket> stack;
    int line = 1;
    int column = 1;
    bool inString = false;

    for (std::size_t index = 0; index < source.size(); ++index) {
        char current = source[index];
        char previous = index > 0 ? source[index - 1] : '\0';

        if (!inString && current == '/' &&
            index + 1 < source.size() && source[index + 1] == '/') {
            while (index < source.size() && source[index] != '\n') {
                index++;
                column++;
            }
            if (index >= source.size()) {
                break;
            }
            line++;
            column = 1;
            continue;
        }

        if (current == '"' && previous != '\\') {
            inString = !inString;
        } else if (!inString) {
            if (current == '(' || current == '[' || current == '{') {
                stack.push_back({current, line, column});
            } else if (current == ')' || current == ']' || current == '}') {
                if (stack.empty()) {
                    addDiagnostic(diagnostics, line, column, column + 1,
                                  TOOL_SEVERITY_ERROR,
                                  "unmatched-close",
                                  "Closing delimiter has no matching opener.");
                } else {
                    OpenBracket open = stack.back();
                    bool matches =
                        (open.bracket == '(' && current == ')') ||
                        (open.bracket == '[' && current == ']') ||
                        (open.bracket == '{' && current == '}');
                    if (!matches) {
                        addDiagnostic(diagnostics, line, column, column + 1,
                                      TOOL_SEVERITY_ERROR,
                                      "mismatched-delimiter",
                                      "Closing delimiter does not match the opener.");
                    } else {
                        stack.pop_back();
                    }
                }
            }
        }

        if (current == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    for (const OpenBracket& open : stack) {
        addDiagnostic(diagnostics, open.line, open.column, open.column + 1,
                      TOOL_SEVERITY_ERROR,
                      "unclosed-delimiter",
                      "Opening delimiter is never closed.");
    }
}

void lintStyleIssues(const std::string& source,
                     std::vector<ToolDiagnostic>* diagnostics) {
    bool hadTrailingNewline = false;
    std::vector<std::string> lines = splitLines(source, &hadTrailingNewline);
    int blankRun = 0;
    std::unordered_set<std::string> importLines;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string& line = lines[index];
        int lineNumber = static_cast<int>(index + 1);

        std::size_t firstTab = line.find('\t');
        if (firstTab != std::string::npos) {
            addDiagnostic(diagnostics, lineNumber, static_cast<int>(firstTab + 1),
                          static_cast<int>(firstTab + 2),
                          TOOL_SEVERITY_WARNING,
                          "tabs",
                          "Use spaces instead of tab characters.");
        }

        if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            std::size_t start = line.find_last_not_of(" \t");
            int column = start == std::string::npos ? 1 : static_cast<int>(start + 2);
            addDiagnostic(diagnostics, lineNumber, column,
                          static_cast<int>(line.size() + 1),
                          TOOL_SEVERITY_WARNING,
                          "trailing-whitespace",
                          "Trailing whitespace should be removed.");
        }

        std::size_t firstContent = firstNonWhitespace(line);
        std::string trimmedLine = trim(line);
        if (!trimmedLine.empty()) {
            blankRun = 0;

            if (firstContent > 0 && firstContent % 4 != 0) {
                addDiagnostic(diagnostics, lineNumber, 1,
                              static_cast<int>(firstContent + 1),
                              TOOL_SEVERITY_INFORMATION,
                              "indentation",
                              "Indentation should use multiples of 4 spaces.");
            }

            if (trimmedLine.rfind("import ", 0) == 0 &&
                trimmedLine.find(" as ") == std::string::npos &&
                trimmedLine.find("\"") == std::string::npos) {
                addDiagnostic(diagnostics, lineNumber, 1,
                              static_cast<int>(trimmedLine.size() + 1),
                              TOOL_SEVERITY_INFORMATION,
                              "plain-import",
                              "Plain imports inject globals; prefer `import x as x` for clarity.");
            }

            if (trimmedLine.rfind("import ", 0) == 0 || trimmedLine.rfind("from ", 0) == 0) {
                if (!importLines.insert(trimmedLine).second) {
                    addDiagnostic(diagnostics, lineNumber, 1,
                                  static_cast<int>(trimmedLine.size() + 1),
                                  TOOL_SEVERITY_INFORMATION,
                                  "duplicate-import",
                                  "Duplicate import statement.");
                }
            }
        } else {
            blankRun++;
            if (blankRun > 1) {
                addDiagnostic(diagnostics, lineNumber, 1, 2,
                              TOOL_SEVERITY_INFORMATION,
                              "blank-lines",
                              "Avoid multiple consecutive blank lines.");
            }
        }
    }
}

void lintLexicalIssues(const std::string& source,
                       std::vector<ToolDiagnostic>* diagnostics) {
    initLexer(source.c_str());
    for (;;) {
        Token token = scanToken();
        if (token.type == TOKEN_ERROR) {
            addDiagnostic(diagnostics, token.line, token.column,
                          token.column + std::max(1, token.length),
                          TOOL_SEVERITY_ERROR,
                          "lex-error",
                          std::string(token.start, token.length));
        } else if (token.type == TOKEN_SEMICOLON) {
            addDiagnostic(diagnostics, token.line, token.column,
                          token.column + 1,
                          TOOL_SEVERITY_INFORMATION,
                          "semicolon",
                          "Semicolons are optional; prefer newline-oriented style.");
        }

        if (token.type == TOKEN_EOF) {
            break;
        }
    }
}

bool parseLoaderError(const std::string& message,
                      std::filesystem::path* filePath,
                      int* line,
                      std::string* detail) {
    std::size_t messageSeparator = message.rfind(": ");
    if (messageSeparator == std::string::npos) {
        return false;
    }

    std::size_t lineSeparator = message.rfind(':', messageSeparator - 1);
    if (lineSeparator == std::string::npos) {
        return false;
    }

    std::string lineText =
        message.substr(lineSeparator + 1, messageSeparator - (lineSeparator + 1));
    if (lineText.empty() ||
        !std::all_of(lineText.begin(), lineText.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return false;
    }

    *filePath = message.substr(0, lineSeparator);
    *line = std::stoi(lineText);
    *detail = message.substr(messageSeparator + 2);
    return true;
}

void appendLoaderDiagnostics(const std::filesystem::path& filePath,
                             std::vector<ToolDiagnostic>* diagnostics) {
    std::filesystem::path packageRoot;
    std::filesystem::path workingDirectory =
        findPackageRootForPath(filePath, &packageRoot)
            ? packageRoot
            : canonicalize(filePath.parent_path());
    std::string expandedSource;
    std::string errorMessage;
    if (loadProgramWithImports(filePath, workingDirectory, &expandedSource, &errorMessage)) {
        return;
    }

    std::filesystem::path diagnosticFile = filePath;
    int line = 1;
    std::string detail = errorMessage;
    if (parseLoaderError(errorMessage, &diagnosticFile, &line, &detail) &&
        canonicalize(diagnosticFile) == canonicalize(filePath)) {
        addDiagnostic(diagnostics, line, 1, 2,
                      TOOL_SEVERITY_ERROR,
                      "import",
                      detail);
    }
}

std::string severityLabel(int severity) {
    switch (severity) {
        case TOOL_SEVERITY_ERROR:
            return "error";
        case TOOL_SEVERITY_WARNING:
            return "warning";
        default:
            return "info";
    }
}

void sortDiagnostics(std::vector<ToolDiagnostic>* diagnostics) {
    std::sort(diagnostics->begin(), diagnostics->end(),
              [](const ToolDiagnostic& left, const ToolDiagnostic& right) {
                  if (left.line != right.line) {
                      return left.line < right.line;
                  }
                  if (left.column != right.column) {
                      return left.column < right.column;
                  }
                  return left.code < right.code;
              });
}

std::filesystem::path resolveTargetPath(const std::filesystem::path& rawPath) {
    if (isAbsolutePath(rawPath)) {
        return canonicalize(rawPath);
    }
    return canonicalize(std::filesystem::current_path() / rawPath);
}

std::string tokenLexeme(const Token& token) {
    if (token.type == TOKEN_EOF) {
        return "<eof>";
    }

    return std::string(token.start, token.length);
}

void appendTokenDump(const std::string& source, std::ostream& output) {
    output << "== TOKENS ==" << std::endl;
    initLexer(source.c_str());
    for (;;) {
        Token token = scanToken();
        output << token.line << ":" << token.column << "  "
               << tokenTypeName(token.type) << "  "
               << tokenLexeme(token) << std::endl;
        if (token.type == TOKEN_EOF) {
            break;
        }
    }
}

void appendFunctionDisassembly(FunctionPtr function,
                               std::ostream& output,
                               std::unordered_set<FunctionPtr>* seen) {
    if (function == nullptr || seen->find(function) != seen->end()) {
        return;
    }

    seen->insert(function);
    std::string name = function->name.empty() ? "<script>" : function->name;
    disassembleChunkToStream(output, &function->chunk, name.c_str());

    for (const Value& constant : function->chunk.constants.values) {
        if (constant.isFunction()) {
            appendFunctionDisassembly(constant.asFunction(), output, seen);
        } else if (constant.isClosure() && constant.asClosure() != nullptr) {
            appendFunctionDisassembly(constant.asClosure()->function, output, seen);
        }
    }
}

bool loadFunctionForDebug(const std::filesystem::path& filePath,
                          FunctionPtr* function,
                          std::string* rawSource,
                          std::string* expandedSource,
                          std::string* manifestText,
                          std::string* entryPath,
                          std::string* errorMessage) {
    std::string extension = lowerCase(filePath.extension().string());
    if (extension == ".urc") {
        return readUrcFile(filePath, function, errorMessage);
    }

    if (extension == ".ura") {
        return readUraFile(filePath, function, manifestText, entryPath, errorMessage);
    }

    if (!readTextFile(filePath, rawSource, errorMessage)) {
        return false;
    }

    std::filesystem::path packageRoot;
    std::filesystem::path workingDirectory =
        findPackageRootForPath(filePath, &packageRoot)
            ? packageRoot
            : canonicalize(filePath.parent_path());
    if (!loadProgramWithImports(filePath, workingDirectory, expandedSource, errorMessage)) {
        return false;
    }

    return compile(expandedSource->c_str(), function);
}

int executeDebugRunFunction(VM& vm, const FunctionPtr& function) {
    InterpretResult result = vm.interpret(function);
    if (result == INTERPRET_COMPILE_ERROR) {
        return 65;
    }
    if (result == INTERPRET_RUNTIME_ERROR) {
        return 70;
    }
    return 0;
}

} // namespace

bool formatUraniumSource(const std::string& source,
                         int indentSize,
                         std::string* formattedSource) {
    return formatUraniumSourceImpl(source, std::max(2, indentSize), formattedSource);
}

static std::vector<ToolDiagnostic>* g_activeDiagnostics = nullptr;

static void compileErrorListener(const std::string& message, int line, int column, int length) {
    if (g_activeDiagnostics != nullptr) {
        ToolDiagnostic diag;
        diag.line = line;
        diag.column = column;
        diag.endLine = line;
        diag.endColumn = column + (length > 0 ? length : 1);
        diag.severity = TOOL_SEVERITY_ERROR;
        diag.code = "compiler-error";
        diag.message = message;
        g_activeDiagnostics->push_back(diag);
    }
}

void lintUraniumSource(const std::string& source,
                       std::vector<ToolDiagnostic>* diagnostics) {
    diagnostics->clear();
    
    // 1. Run style, lexical, and brace linter
    lintStyleIssues(source, diagnostics);
    lintLexicalIssues(source, diagnostics);
    lintBraceBalance(source, diagnostics);

    // 2. Run compiler parser & type checker to capture compiler errors
    g_activeDiagnostics = diagnostics;
    g_compileErrorCallback = compileErrorListener;
    
    FunctionPtr ignored = nullptr;
    compile(source.c_str(), &ignored);
    
    g_compileErrorCallback = nullptr;
    g_activeDiagnostics = nullptr;

    sortDiagnostics(diagnostics);
}

int formatPath(const std::filesystem::path& targetPath,
               bool checkOnly,
               int indentSize) {
    std::filesystem::path resolved = resolveTargetPath(targetPath);
    if (!pathExists(resolved)) {
        std::cerr << "Format target does not exist: " << displayPath(resolved) << std::endl;
        return 66;
    }

    std::vector<std::filesystem::path> files;
    discoverUraniumSourceFiles(resolved, &files);
    if (files.empty()) {
        std::cerr << "No Uranium source files found under " << displayPath(resolved) << std::endl;
        return 66;
    }

    int changedCount = 0;
    int errorCount = 0;
    for (const std::filesystem::path& file : files) {
        std::string source;
        std::string errorMessage;
        if (!readTextFile(file, &source, &errorMessage)) {
            std::cerr << errorMessage << std::endl;
            errorCount++;
            continue;
        }

        std::string formatted;
        formatUraniumSourceImpl(source, std::max(2, indentSize), &formatted);
        if (formatted == source) {
            continue;
        }

        changedCount++;
        if (checkOnly) {
            std::cout << "Needs formatting: " << displayPath(file) << std::endl;
            continue;
        }

        if (!writeTextFile(file, formatted, &errorMessage)) {
            std::cerr << errorMessage << std::endl;
            errorCount++;
            continue;
        }

        std::cout << "Formatted " << displayPath(file) << std::endl;
    }

    if (errorCount > 0) {
        return 74;
    }

    if (checkOnly) {
        std::cout << "Format check: " << changedCount << " file(s) need changes." << std::endl;
        return changedCount == 0 ? 0 : 1;
    }

    std::cout << "Formatted " << changedCount << " file(s)." << std::endl;
    return 0;
}

int lintPath(const std::filesystem::path& targetPath,
             const std::filesystem::path& executablePath) {
    (void) executablePath;
    std::filesystem::path resolved = resolveTargetPath(targetPath);
    if (!pathExists(resolved)) {
        std::cerr << "Lint target does not exist: " << displayPath(resolved) << std::endl;
        return 66;
    }

    std::vector<std::filesystem::path> files;
    discoverUraniumSourceFiles(resolved, &files);
    if (files.empty()) {
        std::cerr << "No Uranium source files found under " << displayPath(resolved) << std::endl;
        return 66;
    }

    int issueCount = 0;
    int errorCount = 0;

    for (const std::filesystem::path& file : files) {
        std::string source;
        std::string errorMessage;
        if (!readTextFile(file, &source, &errorMessage)) {
            std::cerr << errorMessage << std::endl;
            errorCount++;
            continue;
        }

        std::vector<ToolDiagnostic> diagnostics;
        lintUraniumSource(source, &diagnostics);
        appendLoaderDiagnostics(file, &diagnostics);
        sortDiagnostics(&diagnostics);

        for (const ToolDiagnostic& diagnostic : diagnostics) {
            std::cout << displayPath(file) << ":" << diagnostic.line << ":" << diagnostic.column
                      << ": " << severityLabel(diagnostic.severity)
                      << " [" << diagnostic.code << "] "
                      << diagnostic.message << std::endl;
        }

        issueCount += static_cast<int>(diagnostics.size());
    }

    std::cout << "Lint summary: " << issueCount << " issue(s)." << std::endl;
    if (errorCount > 0) {
        return 74;
    }

    return issueCount == 0 ? 0 : 1;
}

int debugPath(const std::filesystem::path& targetPath,
              const std::filesystem::path& executablePath) {
    (void) executablePath;
    std::filesystem::path resolved = resolveTargetPath(targetPath);
    if (!fileExists(resolved)) {
        std::cerr << "Debug target does not exist: " << displayPath(resolved) << std::endl;
        return 66;
    }

    FunctionPtr function = nullptr;
    std::string rawSource;
    std::string expandedSource;
    std::string manifestText;
    std::string entryPath;
    std::string errorMessage;
    if (!loadFunctionForDebug(resolved, &function, &rawSource, &expandedSource,
                              &manifestText, &entryPath, &errorMessage)) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        } else {
            std::cerr << "Failed to compile debug target." << std::endl;
        }
        return 74;
    }

    std::cout << "== DEBUG ==" << std::endl;
    std::cout << "path: " << displayPath(resolved) << std::endl;

    std::filesystem::path packageRoot;
    if (findPackageRootForPath(resolved, &packageRoot)) {
        std::cout << "package: " << displayPath(packageRoot) << std::endl;
        std::filesystem::path lockPath = packageLockFilePath(packageRoot);
        if (fileExists(lockPath)) {
            std::cout << "lockfile: " << displayPath(lockPath) << std::endl;
        }
    }

    if (!manifestText.empty()) {
        std::cout << "archive-entry: " << entryPath << std::endl;
    }

    if (!rawSource.empty()) {
        appendTokenDump(rawSource, std::cout);
        if (!expandedSource.empty() && expandedSource != rawSource) {
            std::cout << std::endl
                      << "== EXPANDED SOURCE ==" << std::endl
                      << expandedSource << std::endl;
        }
    }

    std::cout << std::endl << "== BYTECODE ==" << std::endl;
    std::unordered_set<FunctionPtr> seen;
    appendFunctionDisassembly(function, std::cout, &seen);
    return 0;
}

int debugRunPath(const std::filesystem::path& targetPath,
                 const std::filesystem::path& executablePath,
                 const std::vector<std::string>& scriptArgs) {
    std::filesystem::path resolved = resolveTargetPath(targetPath);
    if (!fileExists(resolved)) {
        std::cerr << "Debug target does not exist: " << displayPath(resolved) << std::endl;
        return 66;
    }

    FunctionPtr function = nullptr;
    std::string rawSource;
    std::string expandedSource;
    std::string manifestText;
    std::string entryPath;
    std::string errorMessage;
    if (!loadFunctionForDebug(resolved, &function, &rawSource, &expandedSource,
                              &manifestText, &entryPath, &errorMessage)) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        }
        return 74;
    }

    configureRuntimeProcessContext(canonicalize(executablePath).generic_string(),
                                   resolved.generic_string(),
                                   scriptArgs);
    VM vm;
    vm.setDebugTraceEnabled(true);
    return executeDebugRunFunction(vm, function);
}

static bool loadTextForSymbolIndex(const std::filesystem::path& path,
                                   const std::unordered_map<std::string, LspDocumentRecord>& documents,
                                   std::string* text) {
    std::filesystem::path canonical = canonicalize(path);
    for (const auto& entry : documents) {
        if (canonicalize(entry.second.path) == canonical) {
            *text = entry.second.text;
            return true;
        }
    }

    std::string errorMessage;
    return readTextFile(canonical, text, &errorMessage);
}

static std::filesystem::path resolveImportModulePath(const std::string& spec,
                                                     const std::filesystem::path& importerPath) {
    if (spec.empty()) {
        return {};
    }

    auto assignIfExists = [](std::filesystem::path candidate) -> std::filesystem::path {
        if (fileExists(candidate)) {
            return canonicalize(candidate);
        }
        return {};
    };

    if (spec.size() >= 2 && spec.front() == '"' && spec.back() == '"') {
        std::string rawPath = spec.substr(1, spec.size() - 2);
        std::filesystem::path rawPathPath(rawPath);
        std::filesystem::path candidate = isAbsolutePath(rawPathPath)
                                              ? rawPathPath
                                              : (importerPath.parent_path() / rawPathPath);
        if (candidate.extension().empty()) {
            candidate += ".ur";
        }
        return assignIfExists(candidate);
    }

    bool resolvedPackageImport = false;
    std::filesystem::path resolvedPackagePath;
    std::string ignoreError;
    if (tryResolveInstalledPackageImport(spec, importerPath, importerPath.parent_path(),
                                         &resolvedPackageImport, &resolvedPackagePath,
                                         &ignoreError) &&
        resolvedPackageImport) {
        return canonicalize(resolvedPackagePath);
    }

    std::filesystem::path current = canonicalize(importerPath.parent_path());
    for (;;) {
        if (directoryExists(current / "urlib")) {
            std::filesystem::path candidate = current / "urlib" / spec;
            if (candidate.extension().empty()) {
                std::filesystem::path direct = assignIfExists(candidate.string() + ".ur");
                if (!direct.empty()) {
                    return direct;
                }
            } else {
                std::filesystem::path direct = assignIfExists(candidate);
                if (!direct.empty()) {
                    return direct;
                }
            }

            std::filesystem::path indexCandidate = current / "urlib" / spec / "index.ur";
            std::filesystem::path indexed = assignIfExists(indexCandidate);
            if (!indexed.empty()) {
                return indexed;
            }
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static bool tryParseDeclarationSymbol(const std::string& originalLine,
                                      int lineNumber,
                                      const std::filesystem::path& filePath,
                                      IndexedSymbol* symbol) {
    std::string line = stripLineComment(originalLine);
    std::size_t contentStart = firstNonWhitespace(line);
    if (contentStart >= line.size()) {
        return false;
    }

    std::size_t index = contentStart;
    if (startsWithKeywordAt(line, index, "export")) {
        index += 6;
        skipInlineWhitespace(line, &index);
    } else if (startsWithKeywordAt(line, index, "private")) {
        index += 7;
        skipInlineWhitespace(line, &index);
    }

    auto fill = [&](const std::string& name,
                    const std::string& detail,
                    int symbolKind,
                    int completionKind,
                    std::size_t searchFrom) {
        symbol->name = name;
        symbol->detail = detail;
        symbol->hoverText = detail;
        symbol->filePath = canonicalize(filePath);
        symbol->line = lineNumber;
        symbol->column = findNameColumnInLine(originalLine, name, searchFrom);
        symbol->endLine = lineNumber;
        symbol->endColumn = symbol->column + static_cast<int>(name.size());
        symbol->symbolKind = symbolKind;
        symbol->completionKind = completionKind;
    };

    std::size_t keywordStart = index;
    if (startsWithKeywordAt(line, index, "async")) {
        index += 5;
        skipInlineWhitespace(line, &index);
        if (startsWithKeywordAt(line, index, "fn")) {
            index += 2;
            std::string name;
            if (readIdentifierAt(line, &index, &name)) {
                fill(name, trim(stripLineComment(originalLine)), 12, 3, keywordStart);
                return true;
            }
        }
        return false;
    }

    auto parseSimple = [&](const std::string& keyword,
                           int symbolKind,
                           int completionKind) -> bool {
        if (!startsWithKeywordAt(line, keywordStart, keyword)) {
            return false;
        }
        std::size_t cursor = keywordStart + keyword.size();
        std::string name;
        if (!readIdentifierAt(line, &cursor, &name)) {
            return false;
        }
        fill(name, trim(stripLineComment(originalLine)), symbolKind, completionKind, keywordStart);
        return true;
    };

    if (parseSimple("fn", 12, 3)) {
        return true;
    }
    if (parseSimple("class", 5, 7)) {
        return true;
    }
    if (parseSimple("enum", 10, 13)) {
        return true;
    }
    if (parseSimple("interface", 11, 8)) {
        return true;
    }
    if (parseSimple("trait", 11, 8)) {
        return true;
    }

    if (startsWithKeywordAt(line, keywordStart, "const")) {
        std::size_t cursor = keywordStart + 5;
        skipInlineWhitespace(line, &cursor);
        if (cursor < line.size() && (line[cursor] == '[' || line[cursor] == '{')) {
            return false;
        }
        std::string name;
        if (!readIdentifierAt(line, &cursor, &name)) {
            return false;
        }
        fill(name, trim(stripLineComment(originalLine)), 14, 21, keywordStart);
        return true;
    }

    if (startsWithKeywordAt(line, keywordStart, "let")) {
        std::size_t cursor = keywordStart + 3;
        skipInlineWhitespace(line, &cursor);
        if (cursor < line.size() && (line[cursor] == '[' || line[cursor] == '{')) {
            return false;
        }
        std::string name;
        if (!readIdentifierAt(line, &cursor, &name)) {
            return false;
        }
        fill(name, trim(stripLineComment(originalLine)), 13, 6, keywordStart);
        return true;
    }

    return false;
}

static void parseImportSymbols(const std::string& originalLine,
                               int lineNumber,
                               const std::filesystem::path& filePath,
                               SymbolIndex* index) {
    std::string line = trim(stripLineComment(originalLine));
    if (line.empty()) {
        return;
    }

    auto addModuleAlias = [&](const std::string& alias,
                              const std::string& spec,
                              const std::filesystem::path& targetPath) {
        IndexedSymbol symbol;
        symbol.name = alias;
        symbol.detail = "module " + spec;
        symbol.hoverText = "module " + spec;
        symbol.filePath = canonicalize(filePath);
        symbol.targetPath = targetPath;
        symbol.line = lineNumber;
        symbol.column = findNameColumnInLine(originalLine, alias);
        symbol.endLine = lineNumber;
        symbol.endColumn = symbol.column + static_cast<int>(alias.size());
        symbol.symbolKind = 2;
        symbol.completionKind = 9;
        symbol.isImport = true;
        symbol.isModuleAlias = true;
        addIndexedSymbol(index, symbol);
    };

    auto addImportedName = [&](const std::string& alias,
                               const std::string& importName,
                               const std::string& spec,
                               const std::filesystem::path& targetPath) {
        IndexedSymbol symbol;
        symbol.name = alias;
        symbol.detail = "import " + importName + " from " + spec;
        symbol.hoverText = "import " + importName + " from " + spec;
        symbol.filePath = canonicalize(filePath);
        symbol.targetPath = targetPath;
        symbol.targetName = importName;
        symbol.line = lineNumber;
        symbol.column = findNameColumnInLine(originalLine, alias);
        symbol.endLine = lineNumber;
        symbol.endColumn = symbol.column + static_cast<int>(alias.size());
        symbol.symbolKind = 13;
        symbol.completionKind = 18;
        symbol.isImport = true;
        addIndexedSymbol(index, symbol);
    };

    if (startsWithKeywordAt(line, 0, "import")) {
        std::size_t cursor = 6;
        std::string spec;
        if (!readImportTokenAt(line, &cursor, &spec)) {
            return;
        }

        std::string alias;
        skipInlineWhitespace(line, &cursor);
        if (startsWithKeywordAt(line, cursor, "as")) {
            cursor += 2;
            if (!readAliasTokenAt(line, &cursor, &alias)) {
                return;
            }
        }

        std::filesystem::path resolvedPath = resolveImportModulePath(spec, filePath);
        if (alias.empty()) {
            if (!resolvedPath.empty()) {
                alias = moduleDefaultAlias(resolvedPath);
            } else {
                alias = spec;
                if (alias.size() >= 2 && alias.front() == '"' && alias.back() == '"') {
                    alias = moduleDefaultAlias(alias.substr(1, alias.size() - 2));
                }
            }
        }

        if (!alias.empty()) {
            addModuleAlias(alias, spec, resolvedPath);
        }
        return;
    }

    if (!startsWithKeywordAt(line, 0, "from")) {
        return;
    }

    std::size_t cursor = 4;
    std::string spec;
    if (!readImportTokenAt(line, &cursor, &spec)) {
        return;
    }

    skipInlineWhitespace(line, &cursor);
    if (!startsWithKeywordAt(line, cursor, "import")) {
        return;
    }
    cursor += 6;

    std::filesystem::path resolvedPath = resolveImportModulePath(spec, filePath);

    while (cursor < line.size()) {
        skipInlineWhitespace(line, &cursor);
        if (cursor >= line.size()) {
            break;
        }

        if (line[cursor] == '*') {
            return;
        }

        std::string importName;
        if (!readIdentifierAt(line, &cursor, &importName)) {
            return;
        }

        std::string alias = importName;
        skipInlineWhitespace(line, &cursor);
        if (startsWithKeywordAt(line, cursor, "as")) {
            cursor += 2;
            if (!readAliasTokenAt(line, &cursor, &alias)) {
                return;
            }
        }

        addImportedName(alias, importName, spec, resolvedPath);

        skipInlineWhitespace(line, &cursor);
        if (cursor < line.size() && line[cursor] == ',') {
            cursor++;
            continue;
        }
        return;
    }
}

static const SymbolIndex* buildSymbolIndexForPath(
    const std::filesystem::path& filePath,
    const std::unordered_map<std::string, LspDocumentRecord>& documents,
    SymbolIndexCache* cache);

static const SymbolIndex* buildSymbolIndexForPath(
    const std::filesystem::path& filePath,
    const std::unordered_map<std::string, LspDocumentRecord>& documents,
    SymbolIndexCache* cache) {
    std::filesystem::path canonical = canonicalize(filePath);
    std::string key = canonical.generic_string();

    auto existing = cache->indices.find(key);
    if (existing != cache->indices.end()) {
        return &existing->second;
    }

    if (cache->building.find(key) != cache->building.end()) {
        return nullptr;
    }

    cache->building.insert(key);

    std::string source;
    if (!loadTextForSymbolIndex(canonical, documents, &source)) {
        cache->building.erase(key);
        return nullptr;
    }

    SymbolIndex index;
    index.filePath = canonical;
    std::string canonicalStr = canonical.generic_string();
    index.uri = (!canonicalStr.empty() && canonicalStr[0] == '/')
                ? "file://" + canonicalStr
                : "file:///" + canonicalStr;

    bool hadTrailingNewline = false;
    std::vector<std::string> lines = splitLines(source, &hadTrailingNewline);
    int braceDepth = 0;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const std::string& line = lines[lineIndex];
        if (braceDepth == 0) {
            parseImportSymbols(line, static_cast<int>(lineIndex) + 1, canonical, &index);

            IndexedSymbol symbol;
            if (tryParseDeclarationSymbol(line, static_cast<int>(lineIndex) + 1,
                                          canonical, &symbol)) {
                addIndexedSymbol(&index, symbol);
            }
        }

        braceDepth = updateBraceDepthForLine(line, braceDepth);
    }

    cache->building.erase(key);
    auto inserted = cache->indices.emplace(key, std::move(index));
    return &inserted.first->second;
}

static const IndexedSymbol* lookupSymbolByName(const SymbolIndex* index,
                                               const std::string& name) {
    if (index == nullptr) {
        return nullptr;
    }

    auto it = index->firstByName.find(name);
    if (it == index->firstByName.end() || it->second >= index->symbols.size()) {
        return nullptr;
    }
    return &index->symbols[it->second];
}

static const IndexedSymbol* resolveDefinitionTarget(
    const IndexedSymbol& symbol,
    const std::unordered_map<std::string, LspDocumentRecord>& documents,
    SymbolIndexCache* cache) {
    if (!symbol.isImport) {
        return &symbol;
    }

    if (symbol.targetPath.empty()) {
        return &symbol;
    }

    if (symbol.isModuleAlias || symbol.targetName.empty()) {
        return nullptr;
    }

    const SymbolIndex* targetIndex = buildSymbolIndexForPath(symbol.targetPath, documents, cache);
    if (targetIndex == nullptr) {
        return nullptr;
    }
    return lookupSymbolByName(targetIndex, symbol.targetName);
}

static bool lineTextAt(const std::string& source,
                       int zeroBasedLine,
                       std::string* lineText) {
    bool hadTrailingNewline = false;
    std::vector<std::string> lines = splitLines(source, &hadTrailingNewline);
    if (zeroBasedLine < 0 || static_cast<std::size_t>(zeroBasedLine) >= lines.size()) {
        return false;
    }
    *lineText = lines[static_cast<std::size_t>(zeroBasedLine)];
    return true;
}

static bool identifierAtPosition(const std::string& source,
                                 int zeroBasedLine,
                                 int zeroBasedCharacter,
                                 std::string* identifier,
                                 int* startCharacter,
                                 int* endCharacter) {
    std::string line;
    if (!lineTextAt(source, zeroBasedLine, &line)) {
        return false;
    }

    if (line.empty()) {
        return false;
    }

    int cursor = std::max(0, std::min(zeroBasedCharacter, static_cast<int>(line.size())));
    if (cursor >= static_cast<int>(line.size()) || !isIdentifierPart(line[cursor])) {
        if (cursor > 0 && isIdentifierPart(line[cursor - 1])) {
            cursor--;
        } else {
            return false;
        }
    }

    int start = cursor;
    while (start > 0 && isIdentifierPart(line[start - 1])) {
        start--;
    }

    int end = cursor + 1;
    while (end < static_cast<int>(line.size()) && isIdentifierPart(line[end])) {
        end++;
    }

    *identifier = line.substr(static_cast<std::size_t>(start),
                              static_cast<std::size_t>(end - start));
    if (startCharacter != nullptr) {
        *startCharacter = start;
    }
    if (endCharacter != nullptr) {
        *endCharacter = end;
    }
    return true;
}

static bool qualifiedAccessAtPosition(const std::string& source,
                                      int zeroBasedLine,
                                      int zeroBasedCharacter,
                                      std::string* qualifier,
                                      std::string* member,
                                      int* memberStart,
                                      int* memberEnd) {
    int start = 0;
    int end = 0;
    if (!identifierAtPosition(source, zeroBasedLine, zeroBasedCharacter, member, &start, &end)) {
        return false;
    }

    std::string line;
    if (!lineTextAt(source, zeroBasedLine, &line)) {
        return false;
    }

    int probe = start;
    while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
        probe--;
    }
    if (probe == 0 || line[probe - 1] != '.') {
        return false;
    }
    probe--;
    while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
        probe--;
    }
    if (probe == 0 || !isIdentifierPart(line[probe - 1])) {
        return false;
    }

    int qualifierEnd = probe;
    int qualifierStart = qualifierEnd - 1;
    while (qualifierStart > 0 && isIdentifierPart(line[qualifierStart - 1])) {
        qualifierStart--;
    }

    *qualifier = line.substr(static_cast<std::size_t>(qualifierStart),
                             static_cast<std::size_t>(qualifierEnd - qualifierStart));
    if (memberStart != nullptr) {
        *memberStart = start;
    }
    if (memberEnd != nullptr) {
        *memberEnd = end;
    }
    return true;
}

static bool completionContextAtPosition(const std::string& source,
                                        int zeroBasedLine,
                                        int zeroBasedCharacter,
                                        std::string* qualifier,
                                        std::string* prefix) {
    qualifier->clear();
    prefix->clear();

    int start = 0;
    int end = 0;
    std::string identifier;
    bool hasIdentifier =
        identifierAtPosition(source, zeroBasedLine, zeroBasedCharacter, &identifier, &start, &end);

    std::string line;
    if (!lineTextAt(source, zeroBasedLine, &line)) {
        return false;
    }

    if (hasIdentifier) {
        *prefix = identifier;
        int probe = start;
        while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
            probe--;
        }
        if (probe > 0 && line[probe - 1] == '.') {
            probe--;
            while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
                probe--;
            }
            if (probe > 0 && isIdentifierPart(line[probe - 1])) {
                int qualifierEnd = probe;
                int qualifierStart = qualifierEnd - 1;
                while (qualifierStart > 0 && isIdentifierPart(line[qualifierStart - 1])) {
                    qualifierStart--;
                }
                *qualifier = line.substr(static_cast<std::size_t>(qualifierStart),
                                         static_cast<std::size_t>(qualifierEnd - qualifierStart));
                return true;
            }
        }
        return true;
    }

    int probe = std::max(0, std::min(zeroBasedCharacter, static_cast<int>(line.size())));
    while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
        probe--;
    }
    if (probe == 0 || line[probe - 1] != '.') {
        return true;
    }
    probe--;
    while (probe > 0 && (line[probe - 1] == ' ' || line[probe - 1] == '\t')) {
        probe--;
    }
    if (probe == 0 || !isIdentifierPart(line[probe - 1])) {
        return true;
    }

    int qualifierEnd = probe;
    int qualifierStart = qualifierEnd - 1;
    while (qualifierStart > 0 && isIdentifierPart(line[qualifierStart - 1])) {
        qualifierStart--;
    }
    *qualifier = line.substr(static_cast<std::size_t>(qualifierStart),
                             static_cast<std::size_t>(qualifierEnd - qualifierStart));
    return true;
}

static std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char current : value) {
        switch (current) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(current);
                break;
        }
    }
    return escaped;
}

static void skipJsonWhitespace(const std::string& text, std::size_t* index) {
    while (*index < text.size() &&
           (text[*index] == ' ' || text[*index] == '\t' ||
            text[*index] == '\r' || text[*index] == '\n')) {
        (*index)++;
    }
}

static bool parseJsonStringAt(const std::string& text,
                              std::size_t* index,
                              std::string* value) {
    skipJsonWhitespace(text, index);
    if (*index >= text.size() || text[*index] != '"') {
        return false;
    }

    (*index)++;
    value->clear();
    while (*index < text.size()) {
        char current = text[*index];
        (*index)++;
        if (current == '"') {
            return true;
        }
        if (current != '\\') {
            value->push_back(current);
            continue;
        }
        if (*index >= text.size()) {
            return false;
        }
        char escaped = text[*index];
        (*index)++;
        switch (escaped) {
            case '"': value->push_back('"'); break;
            case '\\': value->push_back('\\'); break;
            case '/': value->push_back('/'); break;
            case 'n': value->push_back('\n'); break;
            case 'r': value->push_back('\r'); break;
            case 't': value->push_back('\t'); break;
            default: value->push_back(escaped); break;
        }
    }
    return false;
}

static std::size_t skipJsonString(const std::string& text, std::size_t index) {
    std::string ignored;
    return parseJsonStringAt(text, &index, &ignored) ? index : std::string::npos;
}

static std::size_t skipJsonValue(const std::string& text, std::size_t index) {
    skipJsonWhitespace(text, &index);
    if (index >= text.size()) {
        return std::string::npos;
    }

    if (text[index] == '"') {
        return skipJsonString(text, index);
    }

    if (text[index] == '{' || text[index] == '[') {
        char open = text[index];
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        while (index < text.size()) {
            if (text[index] == '"') {
                index = skipJsonString(text, index);
                if (index == std::string::npos) {
                    return index;
                }
                continue;
            }
            if (text[index] == open) {
                depth++;
            } else if (text[index] == close) {
                depth--;
                if (depth == 0) {
                    return index + 1;
                }
            }
            index++;
        }
        return std::string::npos;
    }

    while (index < text.size() &&
           text[index] != ',' &&
           text[index] != '}' &&
           text[index] != ']' &&
           text[index] != '\r' &&
           text[index] != '\n') {
        index++;
    }
    return index;
}

static bool extractJsonStringField(const std::string& text,
                                   const std::string& fieldName,
                                   std::string* value,
                                   std::size_t searchFrom = 0) {
    std::string pattern = "\"" + fieldName + "\"";
    std::size_t position = text.find(pattern, searchFrom);
    if (position == std::string::npos) {
        return false;
    }

    std::size_t cursor = position + pattern.size();
    skipJsonWhitespace(text, &cursor);
    if (cursor >= text.size() || text[cursor] != ':') {
        return false;
    }
    cursor++;
    return parseJsonStringAt(text, &cursor, value);
}

static bool extractJsonIntegerField(const std::string& text,
                                    const std::string& fieldName,
                                    int* value) {
    std::string pattern = "\"" + fieldName + "\"";
    std::size_t position = text.find(pattern);
    if (position == std::string::npos) {
        return false;
    }

    std::size_t cursor = position + pattern.size();
    skipJsonWhitespace(text, &cursor);
    if (cursor >= text.size() || text[cursor] != ':') {
        return false;
    }
    cursor++;
    skipJsonWhitespace(text, &cursor);

    std::size_t start = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        cursor++;
    }
    if (start == cursor) {
        return false;
    }

    *value = std::stoi(text.substr(start, cursor - start));
    return true;
}

static bool extractJsonRawField(const std::string& text,
                                const std::string& fieldName,
                                std::string* rawValue) {
    std::string pattern = "\"" + fieldName + "\"";
    std::size_t position = text.find(pattern);
    if (position == std::string::npos) {
        return false;
    }

    std::size_t cursor = position + pattern.size();
    skipJsonWhitespace(text, &cursor);
    if (cursor >= text.size() || text[cursor] != ':') {
        return false;
    }
    cursor++;
    std::size_t start = cursor;
    std::size_t end = skipJsonValue(text, cursor);
    if (end == std::string::npos) {
        return false;
    }

    *rawValue = text.substr(start, end - start);
    return true;
}

static std::string percentDecode(const std::string& text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            auto hexValue = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };

            int high = hexValue(text[index + 1]);
            int low = hexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }

        decoded.push_back(text[index]);
    }
    return decoded;
}

static std::filesystem::path fileUriToPath(const std::string& uri) {
    std::string prefix = "file://";
    if (uri.rfind(prefix, 0) == 0) {
        std::string body = percentDecode(uri.substr(prefix.size()));
#ifdef _WIN32
        // On Windows, if path is /C:/..., strip the leading slash
        if (body.size() >= 3 && body[0] == '/' && body[2] == ':') {
            body = body.substr(1);
        }
        std::replace(body.begin(), body.end(), '/', '\\');
#endif
        return body;
    }

    return percentDecode(uri);
}

static std::string pathToFileUri(const std::filesystem::path& path) {
    std::string normalized = canonicalize(path).generic_string();
    if (!normalized.empty() && normalized[0] == '/') {
        return "file://" + normalized;
    }
    return "file:///" + normalized;
}

static bool readLspMessage(std::istream& input, std::string* message) {
    message->clear();
    std::string line;
    std::size_t contentLength = 0;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        std::string lower = lowerCase(line);
        if (lower.rfind("content-length:", 0) == 0) {
            std::string lengthText = trim(line.substr(15));
            contentLength = static_cast<std::size_t>(std::stoul(lengthText));
        }
    }

    if (contentLength == 0) {
        return false;
    }

    message->resize(contentLength);
    input.read(&(*message)[0], static_cast<std::streamsize>(contentLength));
    return input.good() || static_cast<std::size_t>(input.gcount()) == contentLength;
}

static void writeLspMessage(const std::string& json) {
    std::cout << "Content-Length: " << json.size() << "\r\n\r\n" << json;
    std::cout.flush();
}

static std::string buildDiagnosticsJson(const std::string& uri,
                                        const std::vector<ToolDiagnostic>& diagnostics) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{";
    out << "\"uri\":\"" << jsonEscape(uri) << "\",\"diagnostics\":[";
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        const ToolDiagnostic& diagnostic = diagnostics[index];
        if (index > 0) {
            out << ",";
        }
        out << "{";
        out << "\"range\":{\"start\":{\"line\":" << std::max(0, diagnostic.line - 1)
            << ",\"character\":" << std::max(0, diagnostic.column - 1)
            << "},\"end\":{\"line\":" << std::max(0, diagnostic.endLine - 1)
            << ",\"character\":" << std::max(0, diagnostic.endColumn - 1) << "}},";
        out << "\"severity\":" << diagnostic.severity << ",";
        out << "\"code\":\"" << jsonEscape(diagnostic.code) << "\",";
        out << "\"source\":\"uranium\",";
        out << "\"message\":\"" << jsonEscape(diagnostic.message) << "\"";
        out << "}";
    }
    out << "]}}";
    return out.str();
}

static std::string buildFormatResponse(const std::string& id,
                                       const std::string& text) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":[{";
    out << "\"range\":{\"start\":{\"line\":0,\"character\":0},";
    out << "\"end\":{\"line\":2147483647,\"character\":0}},";
    out << "\"newText\":\"" << jsonEscape(text) << "\"";
    out << "}]}";
    return out.str();
}

static std::string buildEmptyResponse(const std::string& id) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":null}";
}

static std::string buildLocationJson(const std::filesystem::path& path,
                                     int line,
                                     int column,
                                     int endLine,
                                     int endColumn) {
    std::ostringstream out;
    out << "{\"uri\":\"" << jsonEscape(pathToFileUri(path)) << "\",";
    out << "\"range\":{\"start\":{\"line\":" << std::max(0, line - 1)
        << ",\"character\":" << std::max(0, column - 1)
        << "},\"end\":{\"line\":" << std::max(0, endLine - 1)
        << ",\"character\":" << std::max(0, endColumn - 1) << "}}}";
    return out.str();
}

static std::string buildDefinitionResponse(const std::string& id,
                                           const IndexedSymbol* symbol) {
    if (symbol == nullptr) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":[]}";
    }

    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":[" +
           buildLocationJson(symbol->filePath, symbol->line, symbol->column,
                             symbol->endLine, symbol->endColumn) +
           "]}";
}

static std::string buildHoverResponse(const std::string& id,
                                      const IndexedSymbol* symbol) {
    if (symbol == nullptr) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":null}";
    }

    std::string contents = symbol->hoverText.empty() ? symbol->detail : symbol->hoverText;
    if (contents.empty()) {
        contents = symbol->name;
    }

    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":\"" +
           jsonEscape(contents) + "\"}}}";
}

static std::string buildCompletionResponse(const std::string& id,
                                           const std::vector<IndexedSymbol>& items,
                                           const std::string& prefix) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"id\":" << id
        << ",\"result\":{\"isIncomplete\":false,\"items\":[";

    bool first = true;
    std::unordered_set<std::string> seen;
    for (const IndexedSymbol& item : items) {
        if (!prefix.empty() && item.name.rfind(prefix, 0) != 0) {
            continue;
        }
        if (!seen.insert(item.name).second) {
            continue;
        }
        if (!first) {
            out << ",";
        }
        first = false;
        out << "{";
        out << "\"label\":\"" << jsonEscape(item.name) << "\",";
        out << "\"kind\":" << item.completionKind << ",";
        out << "\"detail\":\"" << jsonEscape(item.detail) << "\"";
        out << "}";
    }

    out << "]}}";
    return out.str();
}

static std::string buildDocumentSymbolResponse(const std::string& id,
                                               const SymbolIndex* index) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":[";
    bool first = true;
    std::unordered_set<std::string> seen;
    if (index != nullptr) {
        for (const IndexedSymbol& symbol : index->symbols) {
            std::string key = symbol.name + "@" + std::to_string(symbol.line) + ":" +
                              std::to_string(symbol.column);
            if (!seen.insert(key).second) {
                continue;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            out << "{";
            out << "\"name\":\"" << jsonEscape(symbol.name) << "\",";
            out << "\"kind\":" << symbol.symbolKind << ",";
            out << "\"location\":" << buildLocationJson(symbol.filePath, symbol.line,
                                                       symbol.column, symbol.endLine,
                                                       symbol.endColumn);
            out << "}";
        }
    }
    out << "]}";
    return out.str();
}

static std::string buildInitializeResponse(const std::string& id) {
    return
        "{\"jsonrpc\":\"2.0\",\"id\":" + id +
        ",\"result\":{\"capabilities\":{\"textDocumentSync\":"
        "{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
        "\"documentFormattingProvider\":true,"
        "\"definitionProvider\":true,"
        "\"hoverProvider\":true,"
        "\"documentSymbolProvider\":true,"
        "\"completionProvider\":{\"resolveProvider\":false,\"triggerCharacters\":[\".\"]}}}}";
}

int runLspServer(const std::filesystem::path& executablePath) {
    (void) executablePath;
    std::unordered_map<std::string, LspDocumentRecord> documents;
    bool shutdownRequested = false;

    for (;;) {
        std::string message;
        if (!readLspMessage(std::cin, &message)) {
            break;
        }

        std::string method;
        extractJsonStringField(message, "method", &method);

        std::string id;
        bool hasId = extractJsonRawField(message, "id", &id);

        if (method == "initialize") {
            writeLspMessage(buildInitializeResponse(hasId ? id : "0"));
            continue;
        }

        if (method == "shutdown") {
            shutdownRequested = true;
            if (hasId) {
                writeLspMessage(buildEmptyResponse(id));
            }
            continue;
        }

        if (method == "exit") {
            return shutdownRequested ? 0 : 1;
        }

        if (method == "textDocument/didOpen" ||
            method == "textDocument/didChange" ||
            method == "textDocument/didSave") {
            std::string uri;
            if (!extractJsonStringField(message, "uri", &uri)) {
                continue;
            }

            std::string text;
            bool hasText = false;
            if (method == "textDocument/didOpen") {
                hasText = extractJsonStringField(message, "text", &text);
            } else if (method == "textDocument/didChange") {
                std::size_t changePos = message.find("\"contentChanges\"");
                hasText = changePos != std::string::npos &&
                          extractJsonStringField(message, "text", &text, changePos);
            } else if (method == "textDocument/didSave") {
                hasText = extractJsonStringField(message, "text", &text);
            }

            std::filesystem::path filePath = fileUriToPath(uri);
            if (!hasText && fileExists(filePath)) {
                std::string errorMessage;
                if (!readTextFile(filePath, &text, &errorMessage)) {
                    text.clear();
                } else {
                    hasText = true;
                }
            }

            if (!hasText) {
                continue;
            }

            LspDocumentRecord document;
            document.uri = uri;
            document.text = text;
            document.path = filePath;
            documents[uri] = document;

            std::vector<ToolDiagnostic> diagnostics;
            lintUraniumSource(text, &diagnostics);
            writeLspMessage(buildDiagnosticsJson(uri, diagnostics));
            continue;
        }

        if (method == "textDocument/formatting") {
            std::string uri;
            if (!extractJsonStringField(message, "uri", &uri)) {
                if (hasId) {
                    writeLspMessage(buildEmptyResponse(id));
                }
                continue;
            }

            int tabSize = 4;
            extractJsonIntegerField(message, "tabSize", &tabSize);

            std::string text;
            auto existing = documents.find(uri);
            if (existing != documents.end()) {
                text = existing->second.text;
            } else {
                std::filesystem::path filePath = fileUriToPath(uri);
                std::string errorMessage;
                if (!readTextFile(filePath, &text, &errorMessage)) {
                    text.clear();
                }
            }

            std::string formatted;
            formatUraniumSource(text, std::max(2, tabSize), &formatted);
            if (formatted == text) {
                if (hasId) {
                    writeLspMessage("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":[]}");
                }
            } else if (hasId) {
                writeLspMessage(buildFormatResponse(id, formatted));
            }
            continue;
        }

        if (method == "textDocument/definition" ||
            method == "textDocument/hover" ||
            method == "textDocument/completion" ||
            method == "textDocument/documentSymbol") {
            std::string uri;
            if (!extractJsonStringField(message, "uri", &uri)) {
                if (hasId) {
                    writeLspMessage(buildEmptyResponse(id));
                }
                continue;
            }

            std::filesystem::path filePath = fileUriToPath(uri);
            std::string text;
            auto existing = documents.find(uri);
            if (existing != documents.end()) {
                text = existing->second.text;
            } else {
                std::string errorMessage;
                if (!readTextFile(filePath, &text, &errorMessage)) {
                    text.clear();
                }
            }

            SymbolIndexCache cache;
            LspDocumentRecord inlineDocument;
            inlineDocument.uri = uri;
            inlineDocument.text = text;
            inlineDocument.path = filePath;
            std::unordered_map<std::string, LspDocumentRecord> lookupDocuments = documents;
            lookupDocuments[uri] = inlineDocument;
            const SymbolIndex* currentIndex = buildSymbolIndexForPath(filePath, lookupDocuments, &cache);

            if (method == "textDocument/documentSymbol") {
                if (hasId) {
                    writeLspMessage(buildDocumentSymbolResponse(id, currentIndex));
                }
                continue;
            }

            int line = 0;
            int character = 0;
            extractJsonIntegerField(message, "line", &line);
            extractJsonIntegerField(message, "character", &character);

            std::string qualifier;
            std::string identifier;
            const IndexedSymbol* resolved = nullptr;
            if (qualifiedAccessAtPosition(text, line, character, &qualifier, &identifier,
                                          nullptr, nullptr)) {
                const IndexedSymbol* qualifierSymbol =
                    lookupSymbolByName(currentIndex, qualifier);
                if (qualifierSymbol != nullptr &&
                    qualifierSymbol->isImport &&
                    qualifierSymbol->isModuleAlias &&
                    !qualifierSymbol->targetPath.empty()) {
                    const SymbolIndex* moduleIndex =
                        buildSymbolIndexForPath(qualifierSymbol->targetPath, lookupDocuments, &cache);
                    resolved = lookupSymbolByName(moduleIndex, identifier);
                }
            } else if (identifierAtPosition(text, line, character, &identifier, nullptr, nullptr)) {
                const IndexedSymbol* direct = lookupSymbolByName(currentIndex, identifier);
                if (direct != nullptr) {
                    resolved = resolveDefinitionTarget(*direct, lookupDocuments, &cache);
                    if (resolved == nullptr) {
                        resolved = direct;
                    }
                }
            }

            if (method == "textDocument/definition") {
                if (hasId) {
                    writeLspMessage(buildDefinitionResponse(id, resolved));
                }
                continue;
            }

            if (method == "textDocument/hover") {
                if (hasId) {
                    writeLspMessage(buildHoverResponse(id, resolved));
                }
                continue;
            }

            if (method == "textDocument/completion") {
                std::string completionQualifier;
                std::string prefix;
                completionContextAtPosition(text, line, character, &completionQualifier, &prefix);

                std::vector<IndexedSymbol> items;
                if (!completionQualifier.empty()) {
                    const IndexedSymbol* moduleAlias =
                        lookupSymbolByName(currentIndex, completionQualifier);
                    if (moduleAlias != nullptr &&
                        moduleAlias->isImport &&
                        moduleAlias->isModuleAlias &&
                        !moduleAlias->targetPath.empty()) {
                        const SymbolIndex* moduleIndex =
                            buildSymbolIndexForPath(moduleAlias->targetPath, lookupDocuments, &cache);
                        if (moduleIndex != nullptr) {
                            items = moduleIndex->symbols;
                        }
                    }
                } else if (currentIndex != nullptr) {
                    items = currentIndex->symbols;
                    const std::vector<std::pair<std::string, int>> keywords = {
                        {"async", 14}, {"await", 14}, {"break", 14}, {"case", 14},
                        {"catch", 14}, {"class", 14}, {"const", 14}, {"continue", 14},
                        {"debugger", 14}, {"default", 14}, {"elif", 14}, {"else", 14}, {"enum", 14},
                        {"false", 14}, {"finally", 14}, {"fn", 14}, {"for", 14},
                        {"if", 14}, {"implements", 14}, {"interface", 14}, {"let", 14},
                        {"match", 14}, {"nil", 14}, {"or", 14}, {"print", 14},
                        {"return", 14}, {"super", 14}, {"switch", 14}, {"this", 14},
                        {"throw", 14}, {"trait", 14}, {"true", 14}, {"try", 14},
                        {"while", 14}
                    };
                    for (const auto& keyword : keywords) {
                        IndexedSymbol item;
                        item.name = keyword.first;
                        item.detail = "keyword";
                        item.hoverText = "keyword " + keyword.first;
                        item.completionKind = keyword.second;
                        item.symbolKind = 13;
                        items.push_back(item);
                    }
                }

                if (hasId) {
                    writeLspMessage(buildCompletionResponse(id, items, prefix));
                }
                continue;
            }
        }

        if (hasId) {
            writeLspMessage(buildEmptyResponse(id));
        }
    }

    return 0;
}
