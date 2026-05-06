#include "source_loader.h"
#include "package_manager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum ImportKind {
    IMPORT_MODULE,
    IMPORT_FROM,
};

enum DeclarationVisibility {
    VISIBILITY_IMPLICIT,
    VISIBILITY_EXPORT,
    VISIBILITY_PRIVATE,
};

struct ImportItem {
    std::string name;
    std::string alias;
    bool wildcard = false;
};

struct ImportRequest {
    ImportKind kind;
    std::string spec;
    std::string namespaceAlias;
    std::vector<ImportItem> items;
    std::filesystem::path resolvedPath;
    int lineNumber = 0;
};

struct ModuleRecord {
    std::filesystem::path canonicalPath;
    std::vector<std::string> dependencies;
    std::unordered_map<std::string, std::string> exports;
    std::string transformedSource;
    bool isEntry = false;
};

struct LoaderState {
    std::unordered_map<std::string, ModuleRecord> modules;
    int nextModuleId = 1;
};

struct ParsedDeclaration {
    bool hasModifier = false;
    bool isDeclaration = false;
    DeclarationVisibility visibility = VISIBILITY_IMPLICIT;
    std::string name;
    std::string strippedLine;
};

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
        start++;
    }

    std::size_t end = value.size();
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string stripLineComment(const std::string& line) {
    bool inString = false;

    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
            inString = !inString;
        }

        if (!inString && line[i] == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }

    return line;
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

std::filesystem::path canonicalize(const std::filesystem::path& path) {
    std::error_code errorCode;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        return std::filesystem::absolute(path);
    }
    return result;
}

bool readFileText(const std::filesystem::path& path,
                  std::string* content,
                  std::string* errorMessage) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open import file '" + path.string() + "'.");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    *content = buffer.str();
    return true;
}

bool isIdentifierStart(char c) {
    unsigned char ch = static_cast<unsigned char>(c);
    return std::isalpha(ch) || c == '_';
}

bool isIdentifierPart(char c) {
    unsigned char ch = static_cast<unsigned char>(c);
    return std::isalnum(ch) || c == '_';
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

void skipWhitespace(const std::string& text, std::size_t* index) {
    while (*index < text.size() && (text[*index] == ' ' || text[*index] == '\t')) {
        (*index)++;
    }
}

bool readIdentifier(const std::string& text,
                    std::size_t* index,
                    std::string* out,
                    std::string* errorMessage,
                    const std::string& context) {
    skipWhitespace(text, index);
    if (*index >= text.size() || !isIdentifierStart(text[*index])) {
        return setError(errorMessage, "Expected identifier " + context + ".");
    }

    std::size_t start = *index;
    (*index)++;
    while (*index < text.size() && isIdentifierPart(text[*index])) {
        (*index)++;
    }

    *out = text.substr(start, *index - start);
    return true;
}

bool readSpecToken(const std::string& text,
                   std::size_t* index,
                   std::string* out,
                   std::string* errorMessage) {
    skipWhitespace(text, index);
    if (*index >= text.size()) {
        return setError(errorMessage, "Import statement is missing a module path.");
    }

    if (text[*index] == '"') {
        std::size_t start = *index;
        (*index)++;

        while (*index < text.size()) {
            if (text[*index] == '"' && text[*index - 1] != '\\') {
                (*index)++;
                *out = text.substr(start, *index - start);
                return true;
            }
            (*index)++;
        }

        return setError(errorMessage, "Unterminated quoted import path.");
    }

    std::size_t start = *index;
    while (*index < text.size() &&
           text[*index] != ' ' &&
           text[*index] != '\t' &&
           text[*index] != ';') {
        (*index)++;
    }

    if (start == *index) {
        return setError(errorMessage, "Import statement is missing a module path.");
    }

    *out = text.substr(start, *index - start);
    return true;
}

bool isValidAliasName(const std::string& value) {
    if (value.empty() || !isIdentifierStart(value[0])) {
        return false;
    }

    for (char character : value) {
        if (!isIdentifierPart(character)) {
            return false;
        }
    }

    return true;
}

bool readAliasToken(const std::string& text,
                    std::size_t* index,
                    std::string* out,
                    std::string* errorMessage,
                    const std::string& context) {
    skipWhitespace(text, index);
    if (*index >= text.size()) {
        return setError(errorMessage, "Expected alias " + context + ".");
    }

    if (text[*index] == '"') {
        std::string quoted;
        if (!readSpecToken(text, index, &quoted, errorMessage)) {
            return false;
        }

        std::string alias = quoted.substr(1, quoted.size() - 2);
        if (!isValidAliasName(alias)) {
            return setError(errorMessage,
                            "Quoted import alias '" + alias +
                            "' must still be a valid identifier.");
        }

        *out = alias;
        return true;
    }

    return readIdentifier(text, index, out, errorMessage, context);
}

bool consumeKeyword(const std::string& text,
                    std::size_t* index,
                    const std::string& keyword) {
    skipWhitespace(text, index);
    if (text.compare(*index, keyword.size(), keyword) != 0) {
        return false;
    }

    std::size_t end = *index + keyword.size();
    if (end < text.size() && isIdentifierPart(text[end])) {
        return false;
    }

    *index = end;
    return true;
}

bool ensureLineFinished(const std::string& text,
                        std::size_t* index,
                        std::string* errorMessage) {
    skipWhitespace(text, index);
    if (*index < text.size() && text[*index] == ';') {
        (*index)++;
        skipWhitespace(text, index);
    }

    if (*index != text.size()) {
        return setError(errorMessage, "Unexpected trailing tokens in import statement.");
    }

    return true;
}

bool parseImportDirective(const std::string& line,
                          ImportRequest* request,
                          std::string* errorMessage) {
    std::string trimmed = trim(stripLineComment(line));
    if (trimmed.empty()) {
        return false;
    }

    if (trimmed.rfind("import", 0) == 0 &&
        (trimmed.size() == 6 || !isIdentifierPart(trimmed[6]))) {
        std::size_t index = 6;
        request->kind = IMPORT_MODULE;
        request->items.clear();
        request->namespaceAlias.clear();

        if (!readSpecToken(trimmed, &index, &request->spec, errorMessage)) {
            return true;
        }

        if (consumeKeyword(trimmed, &index, "as")) {
            if (!readAliasToken(trimmed, &index, &request->namespaceAlias, errorMessage,
                                "after 'as'")) {
                return true;
            }
        }

        if (!ensureLineFinished(trimmed, &index, errorMessage)) {
            return true;
        }

        return true;
    }

    if (trimmed.rfind("from", 0) == 0 &&
        (trimmed.size() == 4 || !isIdentifierPart(trimmed[4]))) {
        std::size_t index = 4;
        request->kind = IMPORT_FROM;
        request->items.clear();
        request->namespaceAlias.clear();

        if (!readSpecToken(trimmed, &index, &request->spec, errorMessage)) {
            return true;
        }

        if (!consumeKeyword(trimmed, &index, "import")) {
            setError(errorMessage, "Expected 'import' after module path in from-import statement.");
            return true;
        }

        while (true) {
            skipWhitespace(trimmed, &index);
            if (index >= trimmed.size()) {
                setError(errorMessage, "from-import statement is missing imported names.");
                return true;
            }

            ImportItem item;
            if (trimmed[index] == '*') {
                item.wildcard = true;
                item.name = "*";
                index++;
            } else {
                if (!readIdentifier(trimmed, &index, &item.name, errorMessage, "in from-import")) {
                    return true;
                }

                if (consumeKeyword(trimmed, &index, "as")) {
                    if (!readAliasToken(trimmed, &index, &item.alias, errorMessage,
                                        "after 'as'")) {
                        return true;
                    }
                }
            }

            request->items.push_back(item);
            skipWhitespace(trimmed, &index);

            if (index < trimmed.size() && trimmed[index] == ',') {
                index++;
                continue;
            }

            if (!ensureLineFinished(trimmed, &index, errorMessage)) {
                return true;
            }

            if (request->items.size() > 1) {
                for (const ImportItem& current : request->items) {
                    if (current.wildcard) {
                        setError(errorMessage, "Wildcard import '*' must be used alone.");
                        return true;
                    }
                }
            }

            return true;
        }
    }

    return false;
}

bool resolveImportPath(const std::string& spec,
                       const std::filesystem::path& importerPath,
                       const std::filesystem::path& workingDirectory,
                       std::filesystem::path* resolvedPath,
                       std::string* errorMessage) {
    auto assignIfExists = [&](const std::filesystem::path& candidate) -> bool {
        if (!fileExists(candidate)) {
            return false;
        }

        *resolvedPath = canonicalize(candidate);
        return true;
    };

    if (spec.size() >= 2 && spec.front() == '"' && spec.back() == '"') {
        std::string rawPath = spec.substr(1, spec.size() - 2);
        std::filesystem::path candidate = importerPath.parent_path() / rawPath;

        if (candidate.extension().empty()) {
            candidate += ".ur";
        }

        if (assignIfExists(candidate)) {
            return true;
        }

        return setError(errorMessage,
                        "Could not resolve import " + spec + " from '" + importerPath.string() + "'.");
    }

    bool resolvedPackageImport = false;
    if (!tryResolveInstalledPackageImport(spec, importerPath, workingDirectory,
                                          &resolvedPackageImport, resolvedPath, errorMessage)) {
        return false;
    }
    if (resolvedPackageImport) {
        return true;
    }

    std::vector<std::filesystem::path> searchRoots;
    std::unordered_set<std::string> seenRoots;

    auto appendSearchRoot = [&](const std::filesystem::path& root) {
        if (root.empty()) {
            return;
        }

        std::filesystem::path normalized = canonicalize(root);
        std::string key = normalized.string();
        if (seenRoots.insert(key).second) {
            searchRoots.push_back(normalized);
        }
    };

    auto appendAncestorRoots = [&](std::filesystem::path root) {
        if (root.empty()) {
            return;
        }

        for (;;) {
            appendSearchRoot(root);
            std::filesystem::path parent = root.parent_path();
            if (parent == root) {
                break;
            }
            root = parent;
        }
    };

    appendSearchRoot(workingDirectory);
    appendAncestorRoots(importerPath.parent_path());

    for (const std::filesystem::path& root : searchRoots) {
        if (!directoryExists(root / "urlib")) {
            continue;
        }

        std::filesystem::path candidate = root / "urlib" / spec;
        if (candidate.extension().empty()) {
            candidate += ".ur";
        }

        if (assignIfExists(candidate)) {
            return true;
        }

        std::filesystem::path indexCandidate = root / "urlib" / spec / "index.ur";
        if (assignIfExists(indexCandidate)) {
            return true;
        }
    }

    return setError(errorMessage,
                    "Could not resolve library import '" + spec + "' under urlib/.");
}

int updateBraceDepth(const std::string& line, int currentDepth) {
    bool inString = false;
    bool inComment = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        char current = line[i];
        char previous = i > 0 ? line[i - 1] : '\0';

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

        if (current == '/' && i + 1 < line.size() && line[i + 1] == '/') {
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

bool tryParseDeclarationName(const std::string& trimmed,
                             const std::string& keyword,
                             std::string* name) {
    if (trimmed.rfind(keyword, 0) != 0) {
        return false;
    }

    if (trimmed.size() == keyword.size() || !std::isspace(static_cast<unsigned char>(trimmed[keyword.size()]))) {
        return false;
    }

    std::size_t index = keyword.size();
    while (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index]))) {
        index++;
    }

    if (index >= trimmed.size() || !isIdentifierStart(trimmed[index])) {
        return false;
    }

    std::size_t start = index;
    index++;
    while (index < trimmed.size() && isIdentifierPart(trimmed[index])) {
        index++;
    }

    *name = trimmed.substr(start, index - start);
    return true;
}

bool parseDeclarationLine(const std::string& line,
                          ParsedDeclaration* result,
                          std::string* errorMessage) {
    result->hasModifier = false;
    result->isDeclaration = false;
    result->visibility = VISIBILITY_IMPLICIT;
    result->name.clear();
    result->strippedLine = line;

    std::size_t contentStart = 0;
    while (contentStart < line.size() &&
           (line[contentStart] == ' ' || line[contentStart] == '\t')) {
        contentStart++;
    }

    if (contentStart >= line.size()) {
        return true;
    }

    if (line[contentStart] == '/' &&
        contentStart + 1 < line.size() &&
        line[contentStart + 1] == '/') {
        return true;
    }

    std::size_t declarationStart = contentStart;
    if (startsWithKeywordAt(line, declarationStart, "export")) {
        result->hasModifier = true;
        result->visibility = VISIBILITY_EXPORT;
        declarationStart += 6;
        skipWhitespace(line, &declarationStart);
    } else if (startsWithKeywordAt(line, declarationStart, "private")) {
        result->hasModifier = true;
        result->visibility = VISIBILITY_PRIVATE;
        declarationStart += 7;
        skipWhitespace(line, &declarationStart);
    }

    auto parseNameAfterKeyword = [&](std::size_t keywordStart,
                                     const std::string& keyword,
                                     const std::string& readableKeyword) -> bool {
        std::size_t index = keywordStart + keyword.size();
        if (!readIdentifier(line, &index, &result->name, errorMessage,
                            "after '" + readableKeyword + "' declaration")) {
            return false;
        }

        result->isDeclaration = true;
        if (result->hasModifier) {
            result->strippedLine =
                line.substr(0, contentStart) + line.substr(declarationStart);
        }
        return true;
    };

    if (startsWithKeywordAt(line, declarationStart, "async")) {
        std::size_t asyncStart = declarationStart;
        std::size_t fnStart = asyncStart + 5;
        skipWhitespace(line, &fnStart);
        if (startsWithKeywordAt(line, fnStart, "fn")) {
            return parseNameAfterKeyword(fnStart, "fn", "async fn");
        }
    }

    if (startsWithKeywordAt(line, declarationStart, "fn")) {
        return parseNameAfterKeyword(declarationStart, "fn", "fn");
    }

    if (startsWithKeywordAt(line, declarationStart, "enum")) {
        return parseNameAfterKeyword(declarationStart, "enum", "enum");
    }

    if (startsWithKeywordAt(line, declarationStart, "interface")) {
        return parseNameAfterKeyword(declarationStart, "interface", "interface");
    }

    if (startsWithKeywordAt(line, declarationStart, "trait")) {
        return parseNameAfterKeyword(declarationStart, "trait", "trait");
    }

    if (startsWithKeywordAt(line, declarationStart, "let")) {
        std::size_t index = declarationStart + 3;
        skipWhitespace(line, &index);
        if (index < line.size() && (line[index] == '[' || line[index] == '{')) {
            if (result->hasModifier) {
                return setError(
                    errorMessage,
                    "Visibility modifiers cannot be applied to destructuring let declarations.");
            }
            return true;
        }
        return parseNameAfterKeyword(declarationStart, "let", "let");
    }

    if (startsWithKeywordAt(line, declarationStart, "const")) {
        std::size_t index = declarationStart + 5;
        skipWhitespace(line, &index);
        if (index < line.size() && (line[index] == '[' || line[index] == '{')) {
            if (result->hasModifier) {
                return setError(
                    errorMessage,
                    "Visibility modifiers cannot be applied to destructuring const declarations.");
            }
            return true;
        }
        return parseNameAfterKeyword(declarationStart, "const", "const");
    }

    if (startsWithKeywordAt(line, declarationStart, "class")) {
        return parseNameAfterKeyword(declarationStart, "class", "class");
    }

    if (result->hasModifier) {
        return setError(errorMessage,
                        "Visibility modifiers must prefix top-level fn, async fn, let, const, class, enum, interface, or trait declarations.");
    }

    return true;
}

bool appendImportAlias(std::unordered_map<std::string, std::string>* aliasMap,
                       const std::string& aliasName,
                       const std::string& symbolName,
                       const std::filesystem::path& modulePath,
                       std::string* errorMessage) {
    auto existing = aliasMap->find(aliasName);
    if (existing == aliasMap->end()) {
        (*aliasMap)[aliasName] = symbolName;
        return true;
    }

    if (existing->second == symbolName) {
        return true;
    }

    return setError(errorMessage,
                    "Import name collision for '" + aliasName + "' in '" + modulePath.string() + "'.");
}

bool appendNamespaceAlias(
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>* namespaceAliases,
    const std::string& alias,
    const std::unordered_map<std::string, std::string>& exports,
    const std::filesystem::path& modulePath,
    std::string* errorMessage) {
    auto existing = namespaceAliases->find(alias);
    if (existing == namespaceAliases->end()) {
        (*namespaceAliases)[alias] = exports;
        return true;
    }

    if (existing->second == exports) {
        return true;
    }

    return setError(errorMessage,
                    "Namespace alias collision for '" + alias + "' in '" + modulePath.string() + "'.");
}

std::string rewriteModuleSource(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& ownRenameMap,
    const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& namespaceAliases,
    const std::filesystem::path& modulePath,
    std::string* errorMessage) {
    std::string output;
    output.reserve(source.size() + 64);

    std::size_t i = 0;
    while (i < source.size()) {
        char current = source[i];

        if (current == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') {
                output.push_back(source[i]);
                i++;
            }
            continue;
        }

        if (current == '"') {
            output.push_back(current);
            i++;

            while (i < source.size()) {
                output.push_back(source[i]);
                if (source[i] == '"' && source[i - 1] != '\\') {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }

        if (!isIdentifierStart(current)) {
            output.push_back(current);
            i++;
            continue;
        }

        std::size_t start = i;
        i++;
        while (i < source.size() && isIdentifierPart(source[i])) {
            i++;
        }

        std::string identifier = source.substr(start, i - start);

        std::size_t probe = i;
        while (probe < source.size() && (source[probe] == ' ' || source[probe] == '\t')) {
            probe++;
        }

        auto namespaceIt = namespaceAliases.find(identifier);
        if (namespaceIt != namespaceAliases.end() && probe < source.size() && source[probe] == '.') {
            probe++;
            while (probe < source.size() && (source[probe] == ' ' || source[probe] == '\t')) {
                probe++;
            }

            if (probe >= source.size() || !isIdentifierStart(source[probe])) {
                setError(errorMessage,
                         "Expected member name after namespace alias '" + identifier +
                         "' in '" + modulePath.string() + "'.");
                return std::string();
            }

            std::size_t memberStart = probe;
            probe++;
            while (probe < source.size() && isIdentifierPart(source[probe])) {
                probe++;
            }

            std::string member = source.substr(memberStart, probe - memberStart);
            auto memberIt = namespaceIt->second.find(member);
            if (memberIt == namespaceIt->second.end()) {
                setError(errorMessage,
                         "Module alias '" + identifier + "' has no export named '" + member +
                         "' in '" + modulePath.string() + "'.");
                return std::string();
            }

            output.append(memberIt->second);
            i = probe;
            continue;
        }

        auto renameIt = ownRenameMap.find(identifier);
        if (renameIt != ownRenameMap.end()) {
            output.append(renameIt->second);
        } else {
            output.append(identifier);
        }
    }

    return output;
}

std::string makeInternalSymbol(int moduleId, const std::string& name) {
    return "__ur_mod_" + std::to_string(moduleId) + "_" + name;
}

bool processModule(const std::filesystem::path& filePath,
                   const std::filesystem::path& workingDirectory,
                   bool isEntry,
                   LoaderState* state,
                   std::unordered_set<std::string>* activeModules,
                   ModuleRecord** moduleOut,
                   std::string* errorMessage) {
    std::filesystem::path canonicalPath = canonicalize(filePath);
    std::string canonicalKey = canonicalPath.string();

    auto cached = state->modules.find(canonicalKey);
    if (cached != state->modules.end()) {
        *moduleOut = &cached->second;
        return true;
    }

    if (activeModules->find(canonicalKey) != activeModules->end()) {
        return setError(errorMessage, "Cyclic import detected at '" + canonicalKey + "'.");
    }

    activeModules->insert(canonicalKey);

    std::string source;
    if (!readFileText(canonicalPath, &source, errorMessage)) {
        activeModules->erase(canonicalKey);
        return false;
    }

    std::vector<ImportRequest> imports;
    std::string bodyWithoutImports;
    std::vector<std::pair<std::string, DeclarationVisibility>> topLevelDeclarations;
    bool usesExplicitVisibility = false;
    std::stringstream stream(source);
    std::string line;
    int lineNumber = 0;
    int braceDepth = 0;

    while (std::getline(stream, line)) {
        lineNumber++;

        ImportRequest request;
        std::string parseError;
        bool isImport = parseImportDirective(line, &request, &parseError);
        if (!parseError.empty()) {
            activeModules->erase(canonicalKey);
            return setError(errorMessage,
                            canonicalKey + ":" + std::to_string(lineNumber) + ": " + parseError);
        }

        int depthBefore = braceDepth;
        braceDepth = updateBraceDepth(line, braceDepth);

        if (isImport) {
            if (depthBefore != 0) {
                activeModules->erase(canonicalKey);
                return setError(errorMessage,
                                canonicalKey + ":" + std::to_string(lineNumber) +
                                ": import statements must appear at top level.");
            }

            request.lineNumber = lineNumber;
            imports.push_back(std::move(request));
            continue;
        }

        ParsedDeclaration declaration;
        std::string declarationError;
        if (!parseDeclarationLine(line, &declaration, &declarationError)) {
            activeModules->erase(canonicalKey);
            return setError(errorMessage,
                            canonicalKey + ":" + std::to_string(lineNumber) + ": " +
                            declarationError);
        }

        if (declaration.hasModifier && depthBefore != 0) {
            activeModules->erase(canonicalKey);
            return setError(errorMessage,
                            canonicalKey + ":" + std::to_string(lineNumber) +
                            ": visibility modifiers must appear on top-level declarations.");
        }

        if (depthBefore == 0 && declaration.isDeclaration) {
            topLevelDeclarations.emplace_back(declaration.name, declaration.visibility);
            usesExplicitVisibility = usesExplicitVisibility || declaration.hasModifier;
        }

        bodyWithoutImports.append(declaration.strippedLine);
        bodyWithoutImports.push_back('\n');
    }

    ModuleRecord module;
    module.canonicalPath = canonicalPath;
    module.isEntry = isEntry;

    std::unordered_map<std::string, std::string> ownRenameMap;
    int moduleId = state->nextModuleId++;

    if (!isEntry) {
        std::unordered_set<std::string> seenDeclarations;
        for (const auto& declaration : topLevelDeclarations) {
            const std::string& name = declaration.first;
            if (!seenDeclarations.insert(name).second) {
                continue;
            }

            ownRenameMap[name] = makeInternalSymbol(moduleId, name);
        }

        for (const auto& declaration : topLevelDeclarations) {
            const std::string& name = declaration.first;
            bool shouldExport =
                !usesExplicitVisibility || declaration.second == VISIBILITY_EXPORT;
            if (!shouldExport) {
                continue;
            }

            module.exports[name] = ownRenameMap[name];
        }
    }

    std::unordered_map<std::string, std::string> importedAliases;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> namespaceAliases;
    std::unordered_set<std::string> dependencySet;

    for (const ImportRequest& request : imports) {
        std::filesystem::path resolvedPath;
        if (!resolveImportPath(request.spec, canonicalPath, workingDirectory, &resolvedPath, errorMessage)) {
            activeModules->erase(canonicalKey);
            return false;
        }

        ModuleRecord* importedModule = nullptr;
        if (!processModule(resolvedPath, workingDirectory, false, state, activeModules,
                           &importedModule, errorMessage)) {
            activeModules->erase(canonicalKey);
            return false;
        }

        if (dependencySet.insert(importedModule->canonicalPath.string()).second) {
            module.dependencies.push_back(importedModule->canonicalPath.string());
        }

        if (request.kind == IMPORT_MODULE) {
            if (request.namespaceAlias.empty()) {
                for (const auto& exportPair : importedModule->exports) {
                    if (!appendImportAlias(&importedAliases, exportPair.first, exportPair.second,
                                           canonicalPath, errorMessage)) {
                        activeModules->erase(canonicalKey);
                        return false;
                    }
                }

                std::string defaultAlias = moduleDefaultAlias(importedModule->canonicalPath);
                if (!defaultAlias.empty()) {
                    if (!appendNamespaceAlias(&namespaceAliases, defaultAlias, importedModule->exports,
                                              canonicalPath, errorMessage)) {
                        activeModules->erase(canonicalKey);
                        return false;
                    }
                }
            } else {
                if (!appendNamespaceAlias(&namespaceAliases, request.namespaceAlias, importedModule->exports,
                                          canonicalPath, errorMessage)) {
                    activeModules->erase(canonicalKey);
                    return false;
                }
            }

            continue;
        }

        if (request.items.size() == 1 && request.items[0].wildcard) {
            for (const auto& exportPair : importedModule->exports) {
                if (!appendImportAlias(&importedAliases, exportPair.first, exportPair.second,
                                       canonicalPath, errorMessage)) {
                    activeModules->erase(canonicalKey);
                    return false;
                }
            }
            continue;
        }

        for (const ImportItem& item : request.items) {
            auto exportIt = importedModule->exports.find(item.name);
            if (exportIt == importedModule->exports.end()) {
                activeModules->erase(canonicalKey);
                return setError(errorMessage,
                                canonicalKey + ":" + std::to_string(request.lineNumber) +
                                ": module '" + request.spec + "' has no export named '" + item.name + "'.");
            }

            std::string aliasName = item.alias.empty() ? item.name : item.alias;
            if (!appendImportAlias(&importedAliases, aliasName, exportIt->second,
                                   canonicalPath, errorMessage)) {
                activeModules->erase(canonicalKey);
                return false;
            }
        }
    }

    for (const auto& declaration : topLevelDeclarations) {
        if (importedAliases.find(declaration.first) != importedAliases.end()) {
            activeModules->erase(canonicalKey);
            return setError(errorMessage,
                            "Import name '" + declaration.first +
                            "' conflicts with a top-level declaration in '" + canonicalKey + "'.");
        }
    }

    std::string rewrittenBody =
        rewriteModuleSource(bodyWithoutImports, ownRenameMap, namespaceAliases, canonicalPath, errorMessage);
    if (errorMessage != nullptr && !errorMessage->empty()) {
        activeModules->erase(canonicalKey);
        return false;
    }

    std::string aliasBlock;
    std::vector<std::pair<std::string, std::string>> sortedAliases(importedAliases.begin(),
                                                                   importedAliases.end());
    std::sort(sortedAliases.begin(), sortedAliases.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    for (const auto& aliasPair : sortedAliases) {
        aliasBlock.append("let ");
        aliasBlock.append(aliasPair.first);
        aliasBlock.append(" = ");
        aliasBlock.append(aliasPair.second);
        aliasBlock.append("\n");
    }

    std::string transformed;
    transformed.append("// begin module ");
    transformed.append(canonicalKey);
    transformed.push_back('\n');
    transformed.append(aliasBlock);
    transformed.append(rewrittenBody);
    transformed.append("// end module ");
    transformed.append(canonicalKey);
    transformed.push_back('\n');

    module.transformedSource = std::move(transformed);
    if (isEntry) {
        module.exports.clear();
    }

    activeModules->erase(canonicalKey);
    auto inserted = state->modules.emplace(canonicalKey, std::move(module));
    *moduleOut = &inserted.first->second;
    return true;
}

void emitModule(const ModuleRecord& module,
                const LoaderState& state,
                std::unordered_set<std::string>* emittedModules,
                std::string* output) {
    std::string key = module.canonicalPath.string();
    if (emittedModules->find(key) != emittedModules->end()) {
        return;
    }

    for (const std::string& dependencyKey : module.dependencies) {
        auto dependency = state.modules.find(dependencyKey);
        if (dependency != state.modules.end()) {
            emitModule(dependency->second, state, emittedModules, output);
        }
    }

    emittedModules->insert(key);
    output->append(module.transformedSource);
}

} // namespace

bool loadProgramWithImports(const std::filesystem::path& entryPath,
                            const std::filesystem::path& workingDirectory,
                            std::string* expandedSource,
                            std::string* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    LoaderState state;
    std::unordered_set<std::string> activeModules;

    ModuleRecord* entryModule = nullptr;
    if (!processModule(entryPath, workingDirectory, true, &state, &activeModules,
                       &entryModule, errorMessage)) {
        return false;
    }

    expandedSource->clear();
    std::unordered_set<std::string> emittedModules;
    emitModule(*entryModule, state, &emittedModules, expandedSource);
    return true;
}
