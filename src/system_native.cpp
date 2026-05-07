#include "system_native.h"
#include "heap.h"
#include "object.h"
#include "value.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct RuntimeProcessContext {
    std::string executablePath;
    std::string entryPath;
    std::vector<std::string> scriptArgs;
};

RuntimeProcessContext& runtimeProcessContext() {
    static RuntimeProcessContext context;
    return context;
}

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool ensureArgCount(int argCount, int expectedCount, std::string* errorMessage) {
    if (argCount == expectedCount) {
        return true;
    }

    return setError(errorMessage,
                    "Expected " + std::to_string(expectedCount) + " argument(s) but got " +
                        std::to_string(argCount) + ".");
}

bool ensureString(const Value& value,
                  const std::string& functionName,
                  int index,
                  std::string* errorMessage) {
    if (value.isString()) {
        return true;
    }

    return setError(errorMessage,
                    functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a string.");
}

bool ensureWholeNumber(const Value& value,
                       const std::string& functionName,
                       int index,
                       std::string* errorMessage) {
    if (!value.isNumber()) {
        return setError(errorMessage,
                        functionName + " expects argument " + std::to_string(index + 1) +
                            " to be a whole number.");
    }

    double numericValue = value.asNumber();
    if (!std::isfinite(numericValue) || std::trunc(numericValue) != numericValue) {
        return setError(errorMessage,
                        functionName + " expects argument " + std::to_string(index + 1) +
                            " to be a whole number.");
    }

    return true;
}

long long asWholeNumber(const Value& value) {
    return static_cast<long long>(value.asNumber());
}

std::string normalizePathString(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

bool readPathArgument(const Value& value,
                      const std::string& functionName,
                      int index,
                      fs::path* out,
                      std::string* errorMessage) {
    if (!ensureString(value, functionName, index, errorMessage)) {
        return false;
    }

    if (out != nullptr) {
        *out = fs::path(value.asString());
    }
    return true;
}

bool readTextFile(const fs::path& path, std::string* text, std::string* errorMessage) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open file '" + normalizePathString(path) + "'.");
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    *text = buffer.str();
    return true;
}

bool writeTextFile(const fs::path& path,
                   const std::string& text,
                   bool append,
                   std::size_t* bytesWritten,
                   std::string* errorMessage) {
    std::ios::openmode mode = std::ios::binary | std::ios::out;
    if (append) {
        mode |= std::ios::app;
    } else {
        mode |= std::ios::trunc;
    }

    std::ofstream file(path, mode);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open file '" + normalizePathString(path) + "' for writing.");
    }

    file << text;
    if (!file.good()) {
        return setError(errorMessage, "Could not write file '" + normalizePathString(path) + "'.");
    }

    if (bytesWritten != nullptr) {
        *bytesWritten = text.size();
    }
    return true;
}

bool pathExists(const fs::path& path) {
    std::error_code errorCode;
    return fs::exists(path, errorCode);
}

bool isRegularFile(const fs::path& path) {
    std::error_code errorCode;
    return fs::is_regular_file(path, errorCode);
}

bool isDirectory(const fs::path& path) {
    std::error_code errorCode;
    return fs::is_directory(path, errorCode);
}

std::string statusToType(const fs::file_status& status) {
    switch (status.type()) {
        case fs::file_type::regular:
            return "file";
        case fs::file_type::directory:
            return "directory";
        case fs::file_type::symlink:
            return "symlink";
        case fs::file_type::not_found:
            return "missing";
        default:
            return "other";
    }
}

double fileTimeToUnixMillis(const fs::file_time_type& fileTime) {
    auto nowFile = fs::file_time_type::clock::now();
    auto nowSystem = std::chrono::system_clock::now();
    auto adjusted = fileTime - nowFile + nowSystem;
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        adjusted.time_since_epoch());
    return static_cast<double>(millis.count());
}

Value makeStringArray(const std::vector<std::string>& values) {
    ArrayPtr array = uraniumHeap().allocateArray();
    array->elements.reserve(values.size());
    for (const std::string& value : values) {
        array->elements.push_back(Value::stringValue(value));
    }
    return Value::arrayValue(array);
}

Value buildStatValue(const fs::path& path) {
    std::error_code errorCode;
    fs::file_status status = fs::status(path, errorCode);
    bool exists = !errorCode && fs::exists(status);
    bool isFile = exists && fs::is_regular_file(status);
    bool isDir = exists && fs::is_directory(status);

    fs::path absolutePath = fs::absolute(path, errorCode);
    if (errorCode) {
        absolutePath = path;
        errorCode.clear();
    }

    MapPtr map = uraniumHeap().allocateMap();
    map->entries["path"] = Value::stringValue(normalizePathString(path));
    map->entries["absolutePath"] = Value::stringValue(normalizePathString(absolutePath));
    map->entries["parent"] = Value::stringValue(normalizePathString(path.parent_path()));
    map->entries["name"] = Value::stringValue(path.filename().generic_string());
    map->entries["stem"] = Value::stringValue(path.stem().generic_string());
    map->entries["extension"] = Value::stringValue(path.extension().generic_string());
    map->entries["exists"] = Value::boolValue(exists);
    map->entries["isFile"] = Value::boolValue(isFile);
    map->entries["isDir"] = Value::boolValue(isDir);
    map->entries["type"] = Value::stringValue(statusToType(status));

    if (isFile) {
        std::uintmax_t size = fs::file_size(path, errorCode);
        map->entries["size"] = errorCode
                                   ? Value::nilValue()
                                   : Value::numberValue(static_cast<double>(size));
    } else {
        map->entries["size"] = Value::nilValue();
    }

    if (exists) {
        fs::file_time_type lastWrite = fs::last_write_time(path, errorCode);
        map->entries["lastWriteMillis"] = errorCode
                                              ? Value::nilValue()
                                              : Value::numberValue(fileTimeToUnixMillis(lastWrite));
    } else {
        map->entries["lastWriteMillis"] = Value::nilValue();
    }

    return Value::mapValue(map);
}

Value buildEntryValue(const fs::path& basePath,
                      const fs::path& currentPath,
                      int depth) {
    Value statValue = buildStatValue(currentPath);
    MapPtr map = statValue.asMap();
    if (map != nullptr) {
        fs::path relative = currentPath.lexically_relative(basePath);
        map->entries["relativePath"] = Value::stringValue(normalizePathString(relative));
        map->entries["depth"] = Value::numberValue(static_cast<double>(depth));
    }
    return statValue;
}

std::string currentPlatformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

bool hasGuiSupport() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string guiBackendName() {
#ifdef _WIN32
    return "win32";
#else
    return "stub";
#endif
}

bool hasNativeJitSupport() {
#if defined(_WIN32) && defined(_M_X64)
    return true;
#else
    return false;
#endif
}

std::string jitBackendName() {
    return hasNativeJitSupport() ? "native-x64" : "fastpath";
}

bool hasHttpSupport() {
#if defined(_WIN32) || defined(URANIUM_HAS_CURL)
    return true;
#else
    return false;
#endif
}

std::string httpBackendName() {
#ifdef _WIN32
    return "winhttp";
#elif defined(URANIUM_HAS_CURL)
    return "libcurl";
#else
    return "unavailable";
#endif
}

bool hasCryptoHashSupport() {
#if defined(_WIN32) || defined(__APPLE__) || defined(URANIUM_HAS_OPENSSL)
    return true;
#else
    return false;
#endif
}

bool hasCryptoAesSupport() {
#if defined(_WIN32) || defined(__APPLE__) || defined(URANIUM_HAS_OPENSSL)
    return true;
#else
    return false;
#endif
}

std::string cryptoBackendName() {
#ifdef _WIN32
    return "cryptoapi";
#elif defined(__APPLE__)
    return "commoncrypto";
#elif defined(URANIUM_HAS_OPENSSL)
    return "openssl";
#else
    return "unavailable";
#endif
}

bool hasAotCompileSupport() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

Value buildRuntimeCapabilitiesValue() {
    MapPtr map = uraniumHeap().allocateMap();
    map->entries["platform"] = Value::stringValue(currentPlatformName());
    map->entries["gui"] = Value::boolValue(hasGuiSupport());
    map->entries["guiBackend"] = Value::stringValue(guiBackendName());
    map->entries["godotScaffold"] = Value::boolValue(true);
    map->entries["netTcp"] = Value::boolValue(true);
    map->entries["http"] = Value::boolValue(hasHttpSupport());
    map->entries["httpBackend"] = Value::stringValue(httpBackendName());
    map->entries["cryptoSha256"] = Value::boolValue(hasCryptoHashSupport());
    map->entries["cryptoAes"] = Value::boolValue(hasCryptoAesSupport());
    map->entries["cryptoBackend"] = Value::stringValue(cryptoBackendName());
    map->entries["sqlite"] = Value::boolValue(true);
    map->entries["threads"] = Value::boolValue(true);
    map->entries["workerPool"] = Value::boolValue(true);
    map->entries["asyncScheduler"] = Value::boolValue(true);
    map->entries["packageManager"] = Value::boolValue(true);
    map->entries["formatter"] = Value::boolValue(true);
    map->entries["linter"] = Value::boolValue(true);
    map->entries["debugger"] = Value::boolValue(true);
    map->entries["lsp"] = Value::boolValue(true);
    map->entries["umake"] = Value::boolValue(true);
    map->entries["urc"] = Value::boolValue(true);
    map->entries["ura"] = Value::boolValue(true);
    map->entries["aotCompile"] = Value::boolValue(hasAotCompileSupport());
    map->entries["typeCheckerStatic"] = Value::boolValue(true);
    map->entries["genericSpecialization"] = Value::boolValue(true);
    map->entries["gcYoungGeneration"] = Value::boolValue(true);
    map->entries["gcMoving"] = Value::boolValue(false);
    map->entries["gcConcurrent"] = Value::boolValue(false);
    map->entries["nativeJit"] = Value::boolValue(hasNativeJitSupport());
    map->entries["jitBackend"] = Value::stringValue(jitBackendName());
    return Value::mapValue(map);
}

int currentProcessId() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

bool runCommandCapture(const std::string& command,
                       int* exitCode,
                       std::string* output,
                       std::string* errorMessage) {
#ifdef _WIN32
    std::string shellCommand = command + " 2>&1";
    FILE* pipe = _popen(shellCommand.c_str(), "r");
#else
    std::string shellCommand = command + " 2>&1";
    FILE* pipe = popen(shellCommand.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return setError(errorMessage, "Could not start process.");
    }

    std::string result;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

#ifdef _WIN32
    int code = _pclose(pipe);
#else
    int code = pclose(pipe);
#endif

    if (output != nullptr) {
        *output = result;
    }
    if (exitCode != nullptr) {
        *exitCode = code;
    }
    return true;
}

void appendJsonEscapedString(const std::string& value, std::string* out) {
    out->push_back('"');
    for (unsigned char character : value) {
        switch (character) {
            case '"':
                out->append("\\\"");
                break;
            case '\\':
                out->append("\\\\");
                break;
            case '\b':
                out->append("\\b");
                break;
            case '\f':
                out->append("\\f");
                break;
            case '\n':
                out->append("\\n");
                break;
            case '\r':
                out->append("\\r");
                break;
            case '\t':
                out->append("\\t");
                break;
            default:
                if (character < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out->append("\\u00");
                    out->push_back(hex[(character >> 4) & 0x0F]);
                    out->push_back(hex[character & 0x0F]);
                } else {
                    out->push_back(static_cast<char>(character));
                }
                break;
        }
    }
    out->push_back('"');
}

void appendCodePointUtf8(unsigned int codePoint, std::string* out) {
    if (codePoint <= 0x7F) {
        out->push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

bool hexDigitValue(char character, unsigned int* value) {
    if (character >= '0' && character <= '9') {
        *value = static_cast<unsigned int>(character - '0');
        return true;
    }
    if (character >= 'a' && character <= 'f') {
        *value = static_cast<unsigned int>(character - 'a' + 10);
        return true;
    }
    if (character >= 'A' && character <= 'F') {
        *value = static_cast<unsigned int>(character - 'A' + 10);
        return true;
    }
    return false;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& source)
        : source_(source), index_(0) {
    }

    bool parse(Value* value, std::string* errorMessage) {
        skipWhitespace();
        if (!parseValue(value, errorMessage)) {
            return false;
        }

        skipWhitespace();
        if (index_ != source_.size()) {
            return error(errorMessage, "Unexpected trailing content.");
        }

        return true;
    }

private:
    const std::string& source_;
    std::size_t index_;

    bool error(std::string* errorMessage, const std::string& message) {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < index_ && i < source_.size(); ++i) {
            if (source_[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }

        return setError(errorMessage,
                        "JSON parse error at line " + std::to_string(line) +
                            ", column " + std::to_string(column) + ": " + message);
    }

    void skipWhitespace() {
        while (index_ < source_.size()) {
            char character = source_[index_];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                index_++;
            } else {
                break;
            }
        }
    }

    bool consume(char expected, std::string* errorMessage) {
        if (index_ >= source_.size() || source_[index_] != expected) {
            return error(errorMessage, std::string("Expected '") + expected + "'.");
        }
        index_++;
        return true;
    }

    bool parseValue(Value* value, std::string* errorMessage) {
        if (index_ >= source_.size()) {
            return error(errorMessage, "Unexpected end of input.");
        }

        char character = source_[index_];
        if (character == '"') {
            std::string text;
            if (!parseString(&text, errorMessage)) {
                return false;
            }
            *value = Value::stringValue(text);
            return true;
        }

        if (character == '{') {
            return parseObject(value, errorMessage);
        }

        if (character == '[') {
            return parseArray(value, errorMessage);
        }

        if (character == 't' && source_.compare(index_, 4, "true") == 0) {
            index_ += 4;
            *value = Value::boolValue(true);
            return true;
        }

        if (character == 'f' && source_.compare(index_, 5, "false") == 0) {
            index_ += 5;
            *value = Value::boolValue(false);
            return true;
        }

        if (character == 'n' && source_.compare(index_, 4, "null") == 0) {
            index_ += 4;
            *value = Value::nilValue();
            return true;
        }

        if (character == '-' || (character >= '0' && character <= '9')) {
            return parseNumber(value, errorMessage);
        }

        return error(errorMessage, "Unexpected token.");
    }

    bool parseString(std::string* value, std::string* errorMessage) {
        if (!consume('"', errorMessage)) {
            return false;
        }

        std::string result;
        while (index_ < source_.size()) {
            char character = source_[index_++];
            if (character == '"') {
                *value = result;
                return true;
            }

            if (static_cast<unsigned char>(character) < 0x20) {
                return error(errorMessage, "Control character is not allowed in JSON string.");
            }

            if (character != '\\') {
                result.push_back(character);
                continue;
            }

            if (index_ >= source_.size()) {
                return error(errorMessage, "Unexpected end of escape sequence.");
            }

            char escape = source_[index_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(escape);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    unsigned int codeUnit = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (index_ >= source_.size()) {
                            return error(errorMessage, "Incomplete unicode escape.");
                        }
                        unsigned int digit = 0;
                        if (!hexDigitValue(source_[index_++], &digit)) {
                            return error(errorMessage, "Invalid unicode escape.");
                        }
                        codeUnit = (codeUnit << 4) | digit;
                    }

                    if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF &&
                        index_ + 6 <= source_.size() &&
                        source_[index_] == '\\' && source_[index_ + 1] == 'u') {
                        std::size_t savedIndex = index_;
                        index_ += 2;
                        unsigned int lowSurrogate = 0;
                        bool validLow = true;
                        for (int i = 0; i < 4; ++i) {
                            unsigned int digit = 0;
                            if (!hexDigitValue(source_[index_++], &digit)) {
                                validLow = false;
                                break;
                            }
                            lowSurrogate = (lowSurrogate << 4) | digit;
                        }

                        if (validLow && lowSurrogate >= 0xDC00 && lowSurrogate <= 0xDFFF) {
                            unsigned int codePoint =
                                0x10000 + (((codeUnit - 0xD800) << 10) | (lowSurrogate - 0xDC00));
                            appendCodePointUtf8(codePoint, &result);
                            break;
                        }

                        index_ = savedIndex;
                    }

                    appendCodePointUtf8(codeUnit, &result);
                    break;
                }
                default:
                    return error(errorMessage, "Invalid escape sequence.");
            }
        }

        return error(errorMessage, "Unterminated string.");
    }

    bool parseNumber(Value* value, std::string* errorMessage) {
        std::size_t start = index_;

        if (source_[index_] == '-') {
            index_++;
        }

        if (index_ >= source_.size()) {
            return error(errorMessage, "Incomplete number.");
        }

        if (source_[index_] == '0') {
            index_++;
        } else if (source_[index_] >= '1' && source_[index_] <= '9') {
            while (index_ < source_.size() &&
                   source_[index_] >= '0' && source_[index_] <= '9') {
                index_++;
            }
        } else {
            return error(errorMessage, "Invalid number.");
        }

        if (index_ < source_.size() && source_[index_] == '.') {
            index_++;
            if (index_ >= source_.size() || source_[index_] < '0' || source_[index_] > '9') {
                return error(errorMessage, "Invalid number.");
            }
            while (index_ < source_.size() &&
                   source_[index_] >= '0' && source_[index_] <= '9') {
                index_++;
            }
        }

        if (index_ < source_.size() && (source_[index_] == 'e' || source_[index_] == 'E')) {
            index_++;
            if (index_ < source_.size() && (source_[index_] == '+' || source_[index_] == '-')) {
                index_++;
            }
            if (index_ >= source_.size() || source_[index_] < '0' || source_[index_] > '9') {
                return error(errorMessage, "Invalid exponent.");
            }
            while (index_ < source_.size() &&
                   source_[index_] >= '0' && source_[index_] <= '9') {
                index_++;
            }
        }

        std::string slice = source_.substr(start, index_ - start);
        char* end = nullptr;
        double number = std::strtod(slice.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(number)) {
            return error(errorMessage, "Invalid numeric value.");
        }

        *value = Value::numberValue(number);
        return true;
    }

    bool parseArray(Value* value, std::string* errorMessage) {
        if (!consume('[', errorMessage)) {
            return false;
        }

        ArrayPtr array = uraniumHeap().allocateArray();
        skipWhitespace();
        if (index_ < source_.size() && source_[index_] == ']') {
            index_++;
            *value = Value::arrayValue(array);
            return true;
        }

        for (;;) {
            skipWhitespace();
            Value element;
            if (!parseValue(&element, errorMessage)) {
                return false;
            }
            array->elements.push_back(element);

            skipWhitespace();
            if (index_ < source_.size() && source_[index_] == ',') {
                index_++;
                continue;
            }

            if (index_ < source_.size() && source_[index_] == ']') {
                index_++;
                *value = Value::arrayValue(array);
                return true;
            }

            return error(errorMessage, "Expected ',' or ']'.");
        }
    }

    bool parseObject(Value* value, std::string* errorMessage) {
        if (!consume('{', errorMessage)) {
            return false;
        }

        MapPtr map = uraniumHeap().allocateMap();
        skipWhitespace();
        if (index_ < source_.size() && source_[index_] == '}') {
            index_++;
            *value = Value::mapValue(map);
            return true;
        }

        for (;;) {
            skipWhitespace();
            std::string key;
            if (!parseString(&key, errorMessage)) {
                return false;
            }

            skipWhitespace();
            if (!consume(':', errorMessage)) {
                return false;
            }

            skipWhitespace();
            Value element;
            if (!parseValue(&element, errorMessage)) {
                return false;
            }
            map->entries[key] = element;

            skipWhitespace();
            if (index_ < source_.size() && source_[index_] == ',') {
                index_++;
                continue;
            }

            if (index_ < source_.size() && source_[index_] == '}') {
                index_++;
                *value = Value::mapValue(map);
                return true;
            }

            return error(errorMessage, "Expected ',' or '}'.");
        }
    }
};

bool stringifyJsonValue(const Value& value,
                        int indentSize,
                        int depth,
                        std::unordered_set<const ArrayObject*>* seenArrays,
                        std::unordered_set<const MapObject*>* seenMaps,
                        std::string* out,
                        std::string* errorMessage) {
    if (value.isNil()) {
        out->append("null");
        return true;
    }

    if (value.isBool()) {
        out->append(value.asBool() ? "true" : "false");
        return true;
    }

    if (value.isNumber()) {
        double number = value.asNumber();
        if (!std::isfinite(number)) {
            return setError(errorMessage, "JSON cannot encode non-finite numbers.");
        }

        std::ostringstream stream;
        stream << number;
        out->append(stream.str());
        return true;
    }

    if (value.isString()) {
        appendJsonEscapedString(value.asString(), out);
        return true;
    }

    if (value.isArray()) {
        ArrayPtr array = value.asArray();
        if (array == nullptr) {
            out->append("[]");
            return true;
        }

        if (!seenArrays->insert(array).second) {
            return setError(errorMessage, "JSON cannot encode cyclic arrays.");
        }

        bool pretty = indentSize > 0;
        out->push_back('[');
        if (!array->elements.empty()) {
            for (std::size_t index = 0; index < array->elements.size(); ++index) {
                if (index > 0) {
                    out->push_back(',');
                }
                if (pretty) {
                    out->push_back('\n');
                    out->append(static_cast<std::size_t>((depth + 1) * indentSize), ' ');
                }
                if (!stringifyJsonValue(array->elements[index], indentSize, depth + 1,
                                        seenArrays, seenMaps, out, errorMessage)) {
                    seenArrays->erase(array);
                    return false;
                }
            }
            if (pretty) {
                out->push_back('\n');
                out->append(static_cast<std::size_t>(depth * indentSize), ' ');
            }
        }
        out->push_back(']');
        seenArrays->erase(array);
        return true;
    }

    if (value.isMap()) {
        MapPtr map = value.asMap();
        if (map == nullptr) {
            out->append("{}");
            return true;
        }

        if (!seenMaps->insert(map).second) {
            return setError(errorMessage, "JSON cannot encode cyclic maps.");
        }

        std::vector<std::string> keys;
        keys.reserve(map->entries.size());
        for (const auto& entry : map->entries) {
            keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());

        bool pretty = indentSize > 0;
        out->push_back('{');
        if (!keys.empty()) {
            for (std::size_t index = 0; index < keys.size(); ++index) {
                if (index > 0) {
                    out->push_back(',');
                }
                if (pretty) {
                    out->push_back('\n');
                    out->append(static_cast<std::size_t>((depth + 1) * indentSize), ' ');
                }
                appendJsonEscapedString(keys[index], out);
                out->append(pretty ? ": " : ":");
                if (!stringifyJsonValue(map->entries.at(keys[index]), indentSize, depth + 1,
                                        seenArrays, seenMaps, out, errorMessage)) {
                    seenMaps->erase(map);
                    return false;
                }
            }
            if (pretty) {
                out->push_back('\n');
                out->append(static_cast<std::size_t>(depth * indentSize), ' ');
            }
        }
        out->push_back('}');
        seenMaps->erase(map);
        return true;
    }

    return setError(errorMessage,
                    "JSON can only encode nil, booleans, numbers, strings, arrays and maps.");
}

} // namespace

void configureRuntimeProcessContext(const std::string& executablePath,
                                    const std::string& entryPath,
                                    const std::vector<std::string>& scriptArgs) {
    RuntimeProcessContext& context = runtimeProcessContext();
    context.executablePath = executablePath;
    context.entryPath = entryPath;
    context.scriptArgs = scriptArgs;
}

Value nativeFsCwd(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    fs::path current = fs::current_path(errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not read current directory.");
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(current));
}

Value nativeFsChangeDir(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsChangeDir", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    fs::current_path(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not change directory to '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    fs::path current = fs::current_path(errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not read current directory after chdir.");
        return Value::nilValue();
    }
    return Value::stringValue(normalizePathString(current));
}

Value nativeFsExists(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsExists", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(pathExists(path));
}

Value nativeFsIsFile(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsIsFile", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(isRegularFile(path));
}

Value nativeFsIsDir(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsIsDir", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(isDirectory(path));
}

Value nativeFsNormalize(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsNormalize", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(path));
}

Value nativeFsAbsolute(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsAbsolute", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    fs::path absolute = fs::absolute(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not resolve absolute path for '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(absolute));
}

Value nativeFsParent(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsParent", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(path.parent_path()));
}

Value nativeFsFileName(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsFileName", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(path.filename().generic_string());
}

Value nativeFsStem(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsStem", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(path.stem().generic_string());
}

Value nativeFsExtension(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsExtension", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(path.extension().generic_string());
}

Value nativeFsJoin2(int argCount, const Value* args, std::string* errorMessage) {
    fs::path left;
    fs::path right;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !readPathArgument(args[0], "fsJoin2", 0, &left, errorMessage) ||
        !readPathArgument(args[1], "fsJoin2", 1, &right, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(left / right));
}

Value nativeFsJoin3(int argCount, const Value* args, std::string* errorMessage) {
    fs::path first;
    fs::path second;
    fs::path third;
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !readPathArgument(args[0], "fsJoin3", 0, &first, errorMessage) ||
        !readPathArgument(args[1], "fsJoin3", 1, &second, errorMessage) ||
        !readPathArgument(args[2], "fsJoin3", 2, &third, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString((first / second) / third));
}

Value nativeFsJoin4(int argCount, const Value* args, std::string* errorMessage) {
    fs::path first;
    fs::path second;
    fs::path third;
    fs::path fourth;
    if (!ensureArgCount(argCount, 4, errorMessage) ||
        !readPathArgument(args[0], "fsJoin4", 0, &first, errorMessage) ||
        !readPathArgument(args[1], "fsJoin4", 1, &second, errorMessage) ||
        !readPathArgument(args[2], "fsJoin4", 2, &third, errorMessage) ||
        !readPathArgument(args[3], "fsJoin4", 3, &fourth, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(((first / second) / third) / fourth));
}

Value nativeFsReadText(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    std::string text;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsReadText", 0, &path, errorMessage) ||
        !readTextFile(path, &text, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(text);
}

Value nativeFsReadLines(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    std::string text;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsReadLines", 0, &path, errorMessage) ||
        !readTextFile(path, &text, errorMessage)) {
        return Value::nilValue();
    }

    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return makeStringArray(lines);
}

Value nativeFsWriteText(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    std::size_t bytesWritten = 0;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !readPathArgument(args[0], "fsWriteText", 0, &path, errorMessage) ||
        !ensureString(args[1], "fsWriteText", 1, errorMessage) ||
        !writeTextFile(path, args[1].asString(), false, &bytesWritten, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(bytesWritten));
}

Value nativeFsAppendText(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    std::size_t bytesWritten = 0;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !readPathArgument(args[0], "fsAppendText", 0, &path, errorMessage) ||
        !ensureString(args[1], "fsAppendText", 1, errorMessage) ||
        !writeTextFile(path, args[1].asString(), true, &bytesWritten, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(bytesWritten));
}

Value nativeFsCreateDir(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsCreateDir", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    bool created = fs::create_directory(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not create directory '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    return Value::boolValue(created);
}

Value nativeFsCreateDirs(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsCreateDirs", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    bool created = fs::create_directories(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not create directories '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    return Value::boolValue(created);
}

Value nativeFsRemove(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsRemove", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    bool removed = fs::remove(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not remove '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    return Value::boolValue(removed);
}

Value nativeFsRemoveTree(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsRemoveTree", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    std::uintmax_t removed = fs::remove_all(path, errorCode);
    if (errorCode) {
        setError(errorMessage, "Could not remove tree '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(removed));
}

Value nativeFsCopy(int argCount, const Value* args, std::string* errorMessage) {
    fs::path fromPath;
    fs::path toPath;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !readPathArgument(args[0], "fsCopy", 0, &fromPath, errorMessage) ||
        !readPathArgument(args[1], "fsCopy", 1, &toPath, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    fs::copy(fromPath, toPath,
             fs::copy_options::overwrite_existing | fs::copy_options::recursive,
             errorCode);
    if (errorCode) {
        setError(errorMessage,
                 "Could not copy '" + normalizePathString(fromPath) +
                     "' to '" + normalizePathString(toPath) + "'.");
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(toPath));
}

Value nativeFsMove(int argCount, const Value* args, std::string* errorMessage) {
    fs::path fromPath;
    fs::path toPath;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !readPathArgument(args[0], "fsMove", 0, &fromPath, errorMessage) ||
        !readPathArgument(args[1], "fsMove", 1, &toPath, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    fs::rename(fromPath, toPath, errorCode);
    if (errorCode) {
        setError(errorMessage,
                 "Could not move '" + normalizePathString(fromPath) +
                     "' to '" + normalizePathString(toPath) + "'.");
        return Value::nilValue();
    }

    return Value::stringValue(normalizePathString(toPath));
}

Value nativeFsStat(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsStat", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    return buildStatValue(path);
}

Value nativeFsListNames(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsListNames", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    if (!fs::is_directory(path, errorCode)) {
        setError(errorMessage, "'" + normalizePathString(path) + "' is not a directory.");
        return Value::nilValue();
    }

    std::vector<std::string> names;
    for (fs::directory_iterator iterator(path, errorCode);
         !errorCode && iterator != fs::directory_iterator();
         iterator.increment(errorCode)) {
        names.push_back(iterator->path().filename().generic_string());
    }

    if (errorCode) {
        setError(errorMessage, "Could not list directory '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    std::sort(names.begin(), names.end());
    return makeStringArray(names);
}

Value nativeFsListEntries(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsListEntries", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    if (!fs::is_directory(path, errorCode)) {
        setError(errorMessage, "'" + normalizePathString(path) + "' is not a directory.");
        return Value::nilValue();
    }

    std::vector<fs::path> paths;
    for (fs::directory_iterator iterator(path, errorCode);
         !errorCode && iterator != fs::directory_iterator();
         iterator.increment(errorCode)) {
        paths.push_back(iterator->path());
    }

    if (errorCode) {
        setError(errorMessage, "Could not list directory '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    std::sort(paths.begin(), paths.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.filename().generic_string() < right.filename().generic_string();
              });

    ArrayPtr result = uraniumHeap().allocateArray();
    result->elements.reserve(paths.size());
    for (const fs::path& entryPath : paths) {
        result->elements.push_back(buildEntryValue(path, entryPath, 1));
    }

    return Value::arrayValue(result);
}

Value nativeFsWalk(int argCount, const Value* args, std::string* errorMessage) {
    fs::path path;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !readPathArgument(args[0], "fsWalk", 0, &path, errorMessage)) {
        return Value::nilValue();
    }

    std::error_code errorCode;
    if (!fs::is_directory(path, errorCode)) {
        setError(errorMessage, "'" + normalizePathString(path) + "' is not a directory.");
        return Value::nilValue();
    }

    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator iterator(path, fs::directory_options::skip_permission_denied, errorCode);
         !errorCode && iterator != fs::recursive_directory_iterator();
         iterator.increment(errorCode)) {
        paths.push_back(iterator->path());
    }

    if (errorCode) {
        setError(errorMessage, "Could not walk directory '" + normalizePathString(path) + "'.");
        return Value::nilValue();
    }

    std::sort(paths.begin(), paths.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.generic_string() < right.generic_string();
              });

    ArrayPtr result = uraniumHeap().allocateArray();
    result->elements.reserve(paths.size());
    for (const fs::path& entryPath : paths) {
        fs::path relative = entryPath.lexically_relative(path);
        int depth = 0;
        for (const auto& ignored : relative) {
            (void)ignored;
            depth++;
        }
        result->elements.push_back(buildEntryValue(path, entryPath, depth));
    }

    return Value::arrayValue(result);
}

Value nativeProcessArgs(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return makeStringArray(runtimeProcessContext().scriptArgs);
}

Value nativeProcessArgCount(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(runtimeProcessContext().scriptArgs.size()));
}

Value nativeProcessExecutablePath(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(runtimeProcessContext().executablePath);
}

Value nativeProcessEntryPath(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    if (runtimeProcessContext().entryPath.empty()) {
        return Value::nilValue();
    }

    return Value::stringValue(runtimeProcessContext().entryPath);
}

Value nativeProcessCwd(int argCount, const Value* args, std::string* errorMessage) {
    return nativeFsCwd(argCount, args, errorMessage);
}

Value nativeProcessChangeDir(int argCount, const Value* args, std::string* errorMessage) {
    return nativeFsChangeDir(argCount, args, errorMessage);
}

Value nativeProcessGetEnv(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "processGetEnv", 0, errorMessage)) {
        return Value::nilValue();
    }

#ifdef _WIN32
    char* rawValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&rawValue, &length, args[0].asString().c_str()) != 0 || rawValue == nullptr) {
        return Value::nilValue();
    }

    std::string value(rawValue);
    std::free(rawValue);
    return Value::stringValue(value);
#else
    const char* value = std::getenv(args[0].asString().c_str());
    if (value == nullptr) {
        return Value::nilValue();
    }

    return Value::stringValue(std::string(value));
#endif
}

Value nativeProcessSetEnv(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "processSetEnv", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string name = args[0].asString();
    int result = 0;
    if (args[1].isNil()) {
#ifdef _WIN32
        result = _putenv_s(name.c_str(), "");
#else
        result = unsetenv(name.c_str());
#endif
    } else {
        std::string value = valueToString(args[1]);
#ifdef _WIN32
        result = _putenv_s(name.c_str(), value.c_str());
#else
        result = setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    if (result != 0) {
        setError(errorMessage, "Could not update environment variable '" + name + "'.");
        return Value::nilValue();
    }

    return Value::boolValue(true);
}

Value nativeProcessPlatform(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(currentPlatformName());
}

Value nativeRuntimeCapabilities(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return buildRuntimeCapabilitiesValue();
}

Value nativeProcessPid(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(currentProcessId()));
}

Value nativeProcessSleep(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "processSleep", 0, errorMessage)) {
        return Value::nilValue();
    }

    long long milliseconds = asWholeNumber(args[0]);
    if (milliseconds < 0) {
        setError(errorMessage, "processSleep expects a non-negative duration.");
        return Value::nilValue();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    return Value::nilValue();
}

Value nativeThreadHardwareConcurrency(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    unsigned int value = std::thread::hardware_concurrency();
    if (value == 0) {
        value = 1;
    }

    return Value::numberValue(static_cast<double>(value));
}

Value nativeThreadYield(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    std::this_thread::yield();
    return Value::nilValue();
}

Value nativeProcessRun(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "processRun", 0, errorMessage)) {
        return Value::nilValue();
    }

    int exitCode = 0;
    std::string output;
    if (!runCommandCapture(args[0].asString(), &exitCode, &output, errorMessage)) {
        return Value::nilValue();
    }

    MapPtr map = uraniumHeap().allocateMap();
    map->entries["command"] = Value::stringValue(args[0].asString());
    map->entries["code"] = Value::numberValue(static_cast<double>(exitCode));
    map->entries["ok"] = Value::boolValue(exitCode == 0);
    map->entries["output"] = Value::stringValue(output);
    return Value::mapValue(map);
}

Value nativeProcessSystem(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "processSystem", 0, errorMessage)) {
        return Value::nilValue();
    }

    int exitCode = std::system(args[0].asString().c_str());
    return Value::numberValue(static_cast<double>(exitCode));
}

Value nativeProcessExit(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "processExit", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::exit(static_cast<int>(asWholeNumber(args[0])));
}

Value nativeJsonParse(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "jsonParse", 0, errorMessage)) {
        return Value::nilValue();
    }

    JsonParser parser(args[0].asString());
    Value value;
    if (!parser.parse(&value, errorMessage)) {
        return Value::nilValue();
    }

    return value;
}

Value nativeJsonValid(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "jsonValid", 0, errorMessage)) {
        return Value::nilValue();
    }

    JsonParser parser(args[0].asString());
    Value value;
    std::string ignoredError;
    return Value::boolValue(parser.parse(&value, &ignoredError));
}

Value nativeJsonStringify(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    std::unordered_set<const ArrayObject*> seenArrays;
    std::unordered_set<const MapObject*> seenMaps;
    std::string output;
    if (!stringifyJsonValue(args[0], 0, 0, &seenArrays, &seenMaps, &output, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(output);
}

Value nativeJsonStringifyPretty(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[1], "jsonStringifyPretty", 1, errorMessage)) {
        return Value::nilValue();
    }

    int indentSize = static_cast<int>(asWholeNumber(args[1]));
    if (indentSize < 0) {
        setError(errorMessage, "jsonStringifyPretty expects a non-negative indent.");
        return Value::nilValue();
    }

    std::unordered_set<const ArrayObject*> seenArrays;
    std::unordered_set<const MapObject*> seenMaps;
    std::string output;
    if (!stringifyJsonValue(args[0], indentSize, 0, &seenArrays, &seenMaps, &output, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(output);
}
