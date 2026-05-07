#include "package_manager.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum JsonValueKind {
    JSON_VALUE_STRING,
    JSON_VALUE_OBJECT,
};

struct JsonValue {
    JsonValueKind kind = JSON_VALUE_OBJECT;
    std::string stringValue;
    std::unordered_map<std::string, JsonValue> objectValues;
};

struct SemverVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

enum VersionRequestKind {
    VERSION_REQUEST_ANY,
    VERSION_REQUEST_EXACT,
    VERSION_REQUEST_CARET,
    VERSION_REQUEST_TILDE,
};

struct VersionRequest {
    VersionRequestKind kind = VERSION_REQUEST_EXACT;
    std::string raw;
    SemverVersion base;
};

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
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

bool isDirectoryEmpty(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::is_empty(path, errorCode);
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
                  std::string* errorMessage) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open '" + displayPath(path) + "'.");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    *content = buffer.str();
    return true;
}

bool writeTextFile(const std::filesystem::path& path,
                   const std::string& content,
                   std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create directory '" + displayPath(path.parent_path()) + "'.");
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open '" + displayPath(path) + "' for writing.");
    }

    file << content;
    if (!file.good()) {
        return setError(errorMessage, "Could not write '" + displayPath(path) + "'.");
    }

    return true;
}

void skipJsonWhitespace(const std::string& text, std::size_t* index) {
    while (*index < text.size()) {
        char current = text[*index];
        if (current != ' ' &&
            current != '\t' &&
            current != '\r' &&
            current != '\n') {
            break;
        }
        (*index)++;
    }
}

bool parseJsonString(const std::string& text,
                     std::size_t* index,
                     std::string* value,
                     std::string* errorMessage) {
    skipJsonWhitespace(text, index);
    if (*index >= text.size() || text[*index] != '"') {
        return setError(errorMessage, "Expected JSON string.");
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
            return setError(errorMessage, "Invalid JSON escape sequence.");
        }

        char escaped = text[*index];
        (*index)++;
        switch (escaped) {
            case '"':
                value->push_back('"');
                break;
            case '\\':
                value->push_back('\\');
                break;
            case '/':
                value->push_back('/');
                break;
            case 'n':
                value->push_back('\n');
                break;
            case 'r':
                value->push_back('\r');
                break;
            case 't':
                value->push_back('\t');
                break;
            default:
                return setError(errorMessage, "Unsupported JSON escape sequence.");
        }
    }

    return setError(errorMessage, "Unterminated JSON string.");
}

bool parseJsonValue(const std::string& text,
                    std::size_t* index,
                    JsonValue* value,
                    std::string* errorMessage);

bool parseJsonObject(const std::string& text,
                     std::size_t* index,
                     JsonValue* value,
                     std::string* errorMessage) {
    skipJsonWhitespace(text, index);
    if (*index >= text.size() || text[*index] != '{') {
        return setError(errorMessage, "Expected JSON object.");
    }

    (*index)++;
    value->kind = JSON_VALUE_OBJECT;
    value->objectValues.clear();
    value->stringValue.clear();

    while (true) {
        skipJsonWhitespace(text, index);
        if (*index >= text.size()) {
            return setError(errorMessage, "Unterminated JSON object.");
        }

        if (text[*index] == '}') {
            (*index)++;
            return true;
        }

        std::string key;
        if (!parseJsonString(text, index, &key, errorMessage)) {
            return false;
        }

        skipJsonWhitespace(text, index);
        if (*index >= text.size() || text[*index] != ':') {
            return setError(errorMessage, "Expected ':' after JSON object key.");
        }

        (*index)++;
        JsonValue child;
        if (!parseJsonValue(text, index, &child, errorMessage)) {
            return false;
        }

        value->objectValues[key] = std::move(child);

        skipJsonWhitespace(text, index);
        if (*index >= text.size()) {
            return setError(errorMessage, "Unterminated JSON object.");
        }

        if (text[*index] == ',') {
            (*index)++;
            continue;
        }

        if (text[*index] == '}') {
            (*index)++;
            return true;
        }

        return setError(errorMessage, "Expected ',' or '}' in JSON object.");
    }
}

bool parseJsonValue(const std::string& text,
                    std::size_t* index,
                    JsonValue* value,
                    std::string* errorMessage) {
    skipJsonWhitespace(text, index);
    if (*index >= text.size()) {
        return setError(errorMessage, "Unexpected end of JSON.");
    }

    if (text[*index] == '"') {
        value->kind = JSON_VALUE_STRING;
        value->objectValues.clear();
        return parseJsonString(text, index, &value->stringValue, errorMessage);
    }

    if (text[*index] == '{') {
        return parseJsonObject(text, index, value, errorMessage);
    }

    return setError(errorMessage, "Only JSON strings and objects are supported here.");
}

bool parseJsonDocument(const std::string& text,
                       JsonValue* root,
                       std::string* errorMessage) {
    std::size_t index = 0;
    if (!parseJsonObject(text, &index, root, errorMessage)) {
        return false;
    }

    skipJsonWhitespace(text, &index);
    if (index != text.size()) {
        return setError(errorMessage, "Unexpected trailing JSON content.");
    }

    return true;
}

const JsonValue* findObjectField(const JsonValue& object, const std::string& fieldName) {
    auto it = object.objectValues.find(fieldName);
    if (it == object.objectValues.end()) {
        return nullptr;
    }
    return &it->second;
}

bool extractOptionalStringField(const JsonValue& object,
                                const std::string& fieldName,
                                std::string* value,
                                const std::string& documentName,
                                std::string* errorMessage) {
    const JsonValue* field = findObjectField(object, fieldName);
    if (field == nullptr) {
        return true;
    }

    if (field->kind != JSON_VALUE_STRING) {
        return setError(errorMessage,
                        documentName + " field '" + fieldName + "' must be a string.");
    }

    *value = field->stringValue;
    return true;
}

bool extractStringMapField(const JsonValue& object,
                           const std::string& fieldName,
                           std::unordered_map<std::string, std::string>* values,
                           const std::string& documentName,
                           std::string* errorMessage) {
    values->clear();
    const JsonValue* field = findObjectField(object, fieldName);
    if (field == nullptr) {
        return true;
    }

    if (field->kind != JSON_VALUE_OBJECT) {
        return setError(errorMessage,
                        documentName + " field '" + fieldName + "' must be an object.");
    }

    for (const auto& entry : field->objectValues) {
        if (entry.second.kind != JSON_VALUE_STRING) {
            return setError(errorMessage,
                            documentName + " field '" + fieldName +
                            "' must contain only string values.");
        }
        (*values)[entry.first] = entry.second.stringValue;
    }

    return true;
}

std::string escapeJsonString(const std::string& value) {
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

void appendIndentedStringField(std::string* out,
                               int indentLevel,
                               const std::string& key,
                               const std::string& value,
                               bool appendComma) {
    out->append(indentLevel * 2, ' ');
    out->append("\"");
    out->append(escapeJsonString(key));
    out->append("\": \"");
    out->append(escapeJsonString(value));
    out->append("\"");
    if (appendComma) {
        out->append(",");
    }
    out->append("\n");
}

template <typename PairType>
std::vector<PairType> sortedPairs(const std::unordered_map<std::string, std::string>& values) {
    std::vector<PairType> ordered(values.begin(), values.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const PairType& left, const PairType& right) {
                  return left.first < right.first;
              });
    return ordered;
}

bool parseSemverVersion(const std::string& text, SemverVersion* version) {
    std::size_t firstDot = text.find('.');
    if (firstDot == std::string::npos) {
        return false;
    }

    std::size_t secondDot = text.find('.', firstDot + 1);
    if (secondDot == std::string::npos || text.find('.', secondDot + 1) != std::string::npos) {
        return false;
    }

    auto parseComponent = [&](std::size_t start, std::size_t end, int* out) -> bool {
        if (start >= end) {
            return false;
        }

        int value = 0;
        for (std::size_t index = start; index < end; ++index) {
            char current = text[index];
            if (!std::isdigit(static_cast<unsigned char>(current))) {
                return false;
            }

            value = (value * 10) + (current - '0');
        }

        *out = value;
        return true;
    };

    return parseComponent(0, firstDot, &version->major) &&
           parseComponent(firstDot + 1, secondDot, &version->minor) &&
           parseComponent(secondDot + 1, text.size(), &version->patch);
}

int compareSemverVersion(const SemverVersion& left, const SemverVersion& right) {
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    return 0;
}

SemverVersion nextCaretUpperBound(const SemverVersion& version) {
    if (version.major > 0) {
        return {version.major + 1, 0, 0};
    }

    if (version.minor > 0) {
        return {0, version.minor + 1, 0};
    }

    return {0, 0, version.patch + 1};
}

SemverVersion nextTildeUpperBound(const SemverVersion& version) {
    return {version.major, version.minor + 1, 0};
}

bool parseVersionRequest(const std::string& text,
                         VersionRequest* request,
                         std::string* errorMessage) {
    request->raw = text;
    if (text.empty() || text == "*" || text == "latest") {
        request->kind = VERSION_REQUEST_ANY;
        request->base = {};
        return true;
    }

    std::string versionText = text;
    if (text[0] == '^') {
        request->kind = VERSION_REQUEST_CARET;
        versionText = text.substr(1);
    } else if (text[0] == '~') {
        request->kind = VERSION_REQUEST_TILDE;
        versionText = text.substr(1);
    } else {
        request->kind = VERSION_REQUEST_EXACT;
    }

    if (!parseSemverVersion(versionText, &request->base)) {
        return setError(errorMessage,
                        "Unsupported version requirement '" + text +
                        "'. Use exact versions like '1.2.3' or ranges like '^1.2.3'/'~1.2.3'.");
    }

    return true;
}

bool matchesVersionRequest(const SemverVersion& version, const VersionRequest& request) {
    if (request.kind == VERSION_REQUEST_ANY) {
        return true;
    }

    if (compareSemverVersion(version, request.base) < 0) {
        return false;
    }

    if (request.kind == VERSION_REQUEST_EXACT) {
        return compareSemverVersion(version, request.base) == 0;
    }

    SemverVersion upperBound =
        request.kind == VERSION_REQUEST_CARET
            ? nextCaretUpperBound(request.base)
            : nextTildeUpperBound(request.base);
    return compareSemverVersion(version, upperBound) < 0;
}

std::string semverToString(const SemverVersion& version) {
    return std::to_string(version.major) + "." +
           std::to_string(version.minor) + "." +
           std::to_string(version.patch);
}

std::uint64_t fnv1aStep(std::uint64_t hash, const char* data, std::size_t length) {
    const std::uint64_t prime = 1099511628211ull;
    for (std::size_t index = 0; index < length; ++index) {
        hash ^= static_cast<unsigned char>(data[index]);
        hash *= prime;
    }
    return hash;
}

void fnv1aUpdate(std::uint64_t* hash, const std::string& value) {
    *hash = fnv1aStep(*hash, value.data(), value.size());
}

std::string hexDigest(std::uint64_t value) {
    static const char* digits = "0123456789abcdef";
    std::string hex(16, '0');
    for (int index = 15; index >= 0; --index) {
        hex[static_cast<std::size_t>(index)] = digits[value & 0xF];
        value >>= 4;
    }
    return hex;
}

std::string packageLocator(const std::string& name, const std::string& version) {
    return name + "@" + version;
}

std::filesystem::path manifestPathForPackage(const std::filesystem::path& packageRoot) {
    return packageRoot / "uranium.pkg";
}

std::filesystem::path installedPackagesRoot(const std::filesystem::path& ownerPackageRoot) {
    return canonicalize(ownerPackageRoot) / ".uranium" / "packages";
}

std::filesystem::path registryMetadataPath(const std::filesystem::path& registryRoot) {
    return registryRoot / "registry.json";
}

std::filesystem::path registryPackagesRoot(const std::filesystem::path& registryRoot) {
    return registryRoot / "packages";
}

std::filesystem::path registryPackageRoot(const std::filesystem::path& registryRoot,
                                          const std::string& packageName,
                                          const std::string& version) {
    return registryPackagesRoot(registryRoot) / packageName / version;
}

std::filesystem::path registryPackageMetadataPath(const std::filesystem::path& registryRoot,
                                                  const std::string& packageName,
                                                  const std::string& version) {
    return registryPackageRoot(registryRoot, packageName, version) / "registry-entry.json";
}

bool shouldSkipCopiedDirectory(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    std::string lower;
    lower.reserve(name.size());
    for (char current : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(current))));
    }

    return lower == ".uranium" ||
           lower == "compiled" ||
           lower == ".git" ||
           lower == ".vs" ||
           lower == "build" ||
           lower.rfind("build", 0) == 0;
}

bool shouldSkipCopiedFile(const std::filesystem::path& path) {
    std::string lower = path.filename().string();
    for (char& current : lower) {
        current = static_cast<char>(std::tolower(static_cast<unsigned char>(current)));
    }

    return lower == "uranium.lock";
}

bool copyPackageTree(const std::filesystem::path& sourceRoot,
                     const std::filesystem::path& targetRoot,
                     std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(targetRoot, errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create package directory '" + displayPath(targetRoot) + "'.");
    }

    std::filesystem::recursive_directory_iterator iterator(sourceRoot, errorCode);
    std::filesystem::recursive_directory_iterator end;
    while (!errorCode && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::filesystem::path relative =
            std::filesystem::relative(entry.path(), sourceRoot, errorCode);
        if (errorCode) {
            return setError(errorMessage,
                            "Could not compute relative path while copying package.");
        }

        std::filesystem::path destination = targetRoot / relative;
        if (entry.is_directory(errorCode)) {
            if (shouldSkipCopiedDirectory(entry.path())) {
                iterator.disable_recursion_pending();
            } else {
                std::filesystem::create_directories(destination, errorCode);
                if (errorCode) {
                    return setError(errorMessage,
                                    "Could not create directory '" +
                                    displayPath(destination) + "'.");
                }
            }

            iterator.increment(errorCode);
            continue;
        }

        if (errorCode) {
            return setError(errorMessage, "Could not inspect package tree while copying.");
        }

        if (shouldSkipCopiedFile(entry.path())) {
            iterator.increment(errorCode);
            continue;
        }

        std::filesystem::create_directories(destination.parent_path(), errorCode);
        if (errorCode) {
            return setError(errorMessage,
                            "Could not create directory '" +
                            displayPath(destination.parent_path()) + "'.");
        }

        std::filesystem::copy_file(entry.path(), destination,
                                   std::filesystem::copy_options::overwrite_existing,
                                   errorCode);
        if (errorCode) {
            return setError(errorMessage,
                            "Could not copy '" + displayPath(entry.path()) +
                            "' to '" + displayPath(destination) + "'.");
        }

        iterator.increment(errorCode);
    }

    if (errorCode) {
        return setError(errorMessage, "Could not traverse package tree while copying.");
    }

    return true;
}

bool removeDirectoryTree(const std::filesystem::path& path, std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::remove_all(path, errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not remove directory tree '" + displayPath(path) + "'.");
    }
    return true;
}

bool computePackageIntegrity(const std::filesystem::path& packageRoot,
                             std::string* integrity,
                             std::string* errorMessage) {
    std::vector<std::filesystem::path> files;
    std::error_code errorCode;
    std::filesystem::recursive_directory_iterator iterator(packageRoot, errorCode);
    std::filesystem::recursive_directory_iterator end;
    while (!errorCode && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_directory(errorCode)) {
            if (shouldSkipCopiedDirectory(entry.path())) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(errorCode);
            continue;
        }

        if (errorCode) {
            return setError(errorMessage,
                            "Could not inspect package tree under '" +
                            displayPath(packageRoot) + "'.");
        }

        if (entry.is_regular_file(errorCode) && !shouldSkipCopiedFile(entry.path())) {
            files.push_back(canonicalize(entry.path()));
        }

        iterator.increment(errorCode);
    }

    if (errorCode) {
        return setError(errorMessage,
                        "Could not traverse package tree under '" +
                        displayPath(packageRoot) + "'.");
    }

    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return left.generic_string() < right.generic_string();
              });

    std::uint64_t hash = 1469598103934665603ull;
    for (const std::filesystem::path& filePath : files) {
        std::error_code relativeError;
        std::filesystem::path relative =
            std::filesystem::relative(filePath, packageRoot, relativeError);
        if (relativeError) {
            return setError(errorMessage,
                            "Could not compute relative path while hashing '" +
                            displayPath(filePath) + "'.");
        }

        std::string relativeText = relative.generic_string();
        fnv1aUpdate(&hash, relativeText);
        hash = fnv1aStep(hash, "\0", 1);

        std::string content;
        if (!readTextFile(filePath, &content, errorMessage)) {
            return false;
        }

        fnv1aUpdate(&hash, content);
        hash = fnv1aStep(hash, "\0", 1);
    }

    *integrity = "fnv64:" + hexDigest(hash);
    return true;
}

bool ensureRegistryLayout(const std::filesystem::path& registryRoot,
                          std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(registryPackagesRoot(registryRoot), errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create registry directory '" +
                        displayPath(registryPackagesRoot(registryRoot)) + "'.");
    }

    if (!fileExists(registryMetadataPath(registryRoot))) {
        std::string metadata =
            "{\n"
            "  \"kind\": \"uranium-registry\",\n"
            "  \"format\": \"1\"\n"
            "}\n";
        if (!writeTextFile(registryMetadataPath(registryRoot), metadata, errorMessage)) {
            return false;
        }
    }

    return true;
}

std::string buildManifestJsonText(const PackageManifestData& manifest) {
    std::string output;
    output.reserve(256);
    output.append("{\n");
    appendIndentedStringField(&output, 1, "name", manifest.name, true);
    appendIndentedStringField(&output, 1, "version", manifest.version, true);
    appendIndentedStringField(&output, 1, "entry", manifest.entry, true);
    appendIndentedStringField(&output, 1, "tests", manifest.tests, true);
    output.append("  \"dependencies\": {\n");
    auto orderedDependencies =
        sortedPairs<std::pair<std::string, std::string>>(manifest.dependencies);
    for (std::size_t index = 0; index < orderedDependencies.size(); ++index) {
        appendIndentedStringField(&output, 2, orderedDependencies[index].first,
                                  orderedDependencies[index].second,
                                  index + 1 < orderedDependencies.size());
    }
    output.append("  }\n");
    output.append("}\n");
    return output;
}

std::string buildRegistryPackageMetadataJsonText(const PackageManifestData& manifest,
                                                 const std::string& integrity) {
    std::string output;
    output.reserve(192);
    output.append("{\n");
    appendIndentedStringField(&output, 1, "kind", "uranium-registry-package", true);
    appendIndentedStringField(&output, 1, "name", manifest.name, true);
    appendIndentedStringField(&output, 1, "version", manifest.version, true);
    appendIndentedStringField(&output, 1, "entry", manifest.entry, true);
    appendIndentedStringField(&output, 1, "integrity", integrity, false);
    output.append("}\n");
    return output;
}

std::string buildLockJsonText(const PackageLockData& lockData) {
    std::string output;
    output.reserve(512);
    output.append("{\n");
    appendIndentedStringField(&output, 1, "name", lockData.name, true);
    appendIndentedStringField(&output, 1, "version", lockData.version, true);
    appendIndentedStringField(&output, 1, "registry", lockData.registryPath, true);

    output.append("  \"directDependencies\": {\n");
    auto orderedDirects =
        sortedPairs<std::pair<std::string, std::string>>(lockData.directDependencies);
    for (std::size_t index = 0; index < orderedDirects.size(); ++index) {
        appendIndentedStringField(&output, 2, orderedDirects[index].first,
                                  orderedDirects[index].second,
                                  index + 1 < orderedDirects.size());
    }
    output.append("  },\n");

    output.append("  \"packages\": {\n");
    std::vector<std::pair<std::string, PackageLockEntryData>> orderedPackages(
        lockData.packages.begin(), lockData.packages.end());
    std::sort(orderedPackages.begin(), orderedPackages.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    for (std::size_t packageIndex = 0; packageIndex < orderedPackages.size(); ++packageIndex) {
        const auto& packageEntry = orderedPackages[packageIndex];
        output.append("    \"");
        output.append(escapeJsonString(packageEntry.first));
        output.append("\": {\n");
        appendIndentedStringField(&output, 3, "name", packageEntry.second.name, true);
        appendIndentedStringField(&output, 3, "version", packageEntry.second.version, true);
        appendIndentedStringField(&output, 3, "entry", packageEntry.second.entry, true);
        appendIndentedStringField(&output, 3, "integrity", packageEntry.second.integrity, true);

        output.append("      \"dependencies\": {\n");
        auto orderedDependencies =
            sortedPairs<std::pair<std::string, std::string>>(packageEntry.second.dependencies);
        for (std::size_t dependencyIndex = 0;
             dependencyIndex < orderedDependencies.size();
             ++dependencyIndex) {
            appendIndentedStringField(&output, 4,
                                      orderedDependencies[dependencyIndex].first,
                                      orderedDependencies[dependencyIndex].second,
                                      dependencyIndex + 1 < orderedDependencies.size());
        }
        output.append("      }\n");
        output.append("    }");
        if (packageIndex + 1 < orderedPackages.size()) {
            output.append(",");
        }
        output.append("\n");
    }

    output.append("  }\n");
    output.append("}\n");
    return output;
}

bool resolveRegistryDependencyVersion(const std::filesystem::path& registryRoot,
                                      const std::string& packageName,
                                      const std::string& versionRequestText,
                                      std::string* resolvedVersion,
                                      std::string* errorMessage) {
    std::filesystem::path packageRoot = registryPackagesRoot(registryRoot) / packageName;
    if (!directoryExists(packageRoot)) {
        return setError(errorMessage,
                        "Registry has no package named '" + packageName +
                        "' under '" + displayPath(registryRoot) + "'.");
    }

    VersionRequest request;
    if (!parseVersionRequest(versionRequestText, &request, errorMessage)) {
        return false;
    }

    bool found = false;
    SemverVersion bestVersion;
    std::string bestVersionText;
    std::error_code errorCode;
    std::filesystem::directory_iterator iterator(packageRoot, errorCode);
    std::filesystem::directory_iterator end;
    while (!errorCode && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (!entry.is_directory(errorCode)) {
            iterator.increment(errorCode);
            continue;
        }

        std::string candidateText = entry.path().filename().string();
        SemverVersion candidateVersion;
        if (!parseSemverVersion(candidateText, &candidateVersion) ||
            !matchesVersionRequest(candidateVersion, request)) {
            iterator.increment(errorCode);
            continue;
        }

        if (!found || compareSemverVersion(candidateVersion, bestVersion) > 0) {
            found = true;
            bestVersion = candidateVersion;
            bestVersionText = candidateText;
        }

        iterator.increment(errorCode);
    }

    if (errorCode) {
        return setError(errorMessage,
                        "Could not enumerate versions for package '" + packageName + "'.");
    }

    if (!found) {
        return setError(errorMessage,
                        "Registry has no version of '" + packageName +
                        "' matching '" + versionRequestText + "'.");
    }

    *resolvedVersion = bestVersionText;
    return true;
}

bool tryLoadRegistryPackageIntegrity(const std::filesystem::path& registryRoot,
                                     const std::string& packageName,
                                     const std::string& version,
                                     std::string* integrity,
                                     std::string* errorMessage) {
    std::filesystem::path metadataPath =
        registryPackageMetadataPath(registryRoot, packageName, version);
    if (!fileExists(metadataPath)) {
        integrity->clear();
        return true;
    }

    std::string text;
    if (!readTextFile(metadataPath, &text, errorMessage)) {
        return false;
    }

    JsonValue root;
    std::string parseError;
    if (!parseJsonDocument(text, &root, &parseError)) {
        return setError(errorMessage,
                        "Invalid registry metadata '" + displayPath(metadataPath) +
                        "': " + parseError);
    }

    return extractOptionalStringField(root, "integrity", integrity,
                                      "Registry metadata '" + displayPath(metadataPath) + "'",
                                      errorMessage);
}

bool loadRegistryPackageManifest(const std::filesystem::path& registryRoot,
                                 const std::string& packageName,
                                 const std::string& version,
                                 PackageManifestData* manifest,
                                 std::string* errorMessage) {
    std::filesystem::path packageRoot = registryPackageRoot(registryRoot, packageName, version);
    if (!directoryExists(packageRoot) || !fileExists(packageRoot / "uranium.pkg")) {
        return setError(errorMessage,
                        "Registry has no package '" + packageName +
                        "' version '" + version + "' under '" +
                        displayPath(registryRoot) + "'.");
    }

    if (!loadPackageManifestData(packageRoot, manifest, errorMessage)) {
        return false;
    }

    if (manifest->name != packageName) {
        return setError(errorMessage,
                        "Registry package '" + displayPath(packageRoot) +
                        "' declares name '" + manifest->name +
                        "' instead of '" + packageName + "'.");
    }

    if (manifest->version != version) {
        return setError(errorMessage,
                        "Registry package '" + displayPath(packageRoot) +
                        "' declares version '" + manifest->version +
                        "' instead of '" + version + "'.");
    }

    return true;
}

bool buildPackageLockForSource(const std::filesystem::path& sourcePackageRoot,
                               const std::filesystem::path& registryRoot,
                               PackageLockData* lockData,
                               std::unordered_set<std::string>* activeLocators,
                               std::string* errorMessage) {
    PackageManifestData manifest;
    if (!loadPackageManifestData(sourcePackageRoot, &manifest, errorMessage)) {
        return false;
    }

    lockData->name = manifest.name;
    lockData->version = manifest.version;
    lockData->registryPath = canonicalize(registryRoot).generic_string();
    lockData->directDependencies.clear();
    lockData->packages.clear();
    lockData->rawText.clear();
    lockData->lockPath = packageLockFilePath(sourcePackageRoot);

    std::vector<std::pair<std::string, std::string>> orderedDependencies(
        manifest.dependencies.begin(), manifest.dependencies.end());
    std::sort(orderedDependencies.begin(), orderedDependencies.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    for (const auto& dependency : orderedDependencies) {
        const std::string& dependencyName = dependency.first;
        const std::string& dependencyRequest = dependency.second;
        std::string dependencyVersion;
        if (!resolveRegistryDependencyVersion(registryRoot, dependencyName,
                                             dependencyRequest, &dependencyVersion,
                                             errorMessage)) {
            return false;
        }

        std::string locator = packageLocator(dependencyName, dependencyVersion);

        if (activeLocators->find(locator) != activeLocators->end()) {
            return setError(errorMessage,
                            "Dependency cycle detected while resolving '" + locator + "'.");
        }

        PackageManifestData dependencyManifest;
        if (!loadRegistryPackageManifest(registryRoot, dependencyName, dependencyVersion,
                                         &dependencyManifest, errorMessage)) {
            return false;
        }

        activeLocators->insert(locator);
        PackageLockData nestedLock;
        if (!buildPackageLockForSource(dependencyManifest.packageRoot, registryRoot,
                                       &nestedLock, activeLocators, errorMessage)) {
            activeLocators->erase(locator);
            return false;
        }
        activeLocators->erase(locator);

        std::string integrity;
        if (!tryLoadRegistryPackageIntegrity(registryRoot, dependencyName, dependencyVersion,
                                             &integrity, errorMessage)) {
            return false;
        }
        if (integrity.empty() &&
            !computePackageIntegrity(dependencyManifest.packageRoot, &integrity, errorMessage)) {
            return false;
        }

        PackageLockEntryData entry;
        entry.name = dependencyManifest.name;
        entry.version = dependencyManifest.version;
        entry.entry = dependencyManifest.entry;
        entry.integrity = integrity;
        entry.dependencies = nestedLock.directDependencies;

        lockData->directDependencies[dependencyName] = dependencyVersion;
        lockData->packages[locator] = entry;

        for (const auto& nestedEntry : nestedLock.packages) {
            lockData->packages[nestedEntry.first] = nestedEntry.second;
        }
    }

    return true;
}

bool appendTransitivePackagesToSubLock(const PackageLockData& rootLock,
                                       const std::string& locator,
                                       PackageLockData* subLock,
                                       std::unordered_set<std::string>* visited,
                                       std::string* errorMessage) {
    if (visited->find(locator) != visited->end()) {
        return true;
    }

    auto packageIt = rootLock.packages.find(locator);
    if (packageIt == rootLock.packages.end()) {
        return setError(errorMessage,
                        "Lockfile is missing package entry '" + locator + "'.");
    }

    visited->insert(locator);
    subLock->packages[locator] = packageIt->second;
    auto orderedDependencies =
        sortedPairs<std::pair<std::string, std::string>>(packageIt->second.dependencies);
    for (const auto& dependency : orderedDependencies) {
        std::string dependencyLocator =
            packageLocator(dependency.first, dependency.second);
        if (!appendTransitivePackagesToSubLock(rootLock, dependencyLocator, subLock,
                                               visited, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool buildSubLockForInstalledPackage(const PackageLockData& rootLock,
                                     const std::string& locator,
                                     const std::filesystem::path& targetPackageRoot,
                                     PackageLockData* subLock,
                                     std::string* errorMessage) {
    auto packageIt = rootLock.packages.find(locator);
    if (packageIt == rootLock.packages.end()) {
        return setError(errorMessage,
                        "Lockfile is missing package entry '" + locator + "'.");
    }

    subLock->name = packageIt->second.name;
    subLock->version = packageIt->second.version;
    subLock->registryPath = rootLock.registryPath;
    subLock->directDependencies = packageIt->second.dependencies;
    subLock->packages.clear();
    subLock->rawText.clear();
    subLock->lockPath = packageLockFilePath(targetPackageRoot);

    std::unordered_set<std::string> visited;
    auto orderedDependencies =
        sortedPairs<std::pair<std::string, std::string>>(packageIt->second.dependencies);
    for (const auto& dependency : orderedDependencies) {
        std::string dependencyLocator =
            packageLocator(dependency.first, dependency.second);
        if (!appendTransitivePackagesToSubLock(rootLock, dependencyLocator, subLock,
                                               &visited, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool installLockedDependencyGraph(const std::filesystem::path& ownerPackageRoot,
                                  const std::filesystem::path& registryRoot,
                                  const PackageLockData& rootLock,
                                  const std::string& dependencyName,
                                  const std::string& dependencyVersion,
                                  std::unordered_set<std::string>* activeLocators,
                                  std::string* errorMessage) {
    std::string locator = packageLocator(dependencyName, dependencyVersion);
    if (activeLocators->find(locator) != activeLocators->end()) {
        return setError(errorMessage,
                        "Dependency cycle detected while installing '" + locator + "'.");
    }

    auto packageIt = rootLock.packages.find(locator);
    if (packageIt == rootLock.packages.end()) {
        return setError(errorMessage,
                        "Lockfile is missing package entry '" + locator + "'.");
    }

    PackageManifestData dependencyManifest;
    if (!loadRegistryPackageManifest(registryRoot, dependencyName, dependencyVersion,
                                     &dependencyManifest, errorMessage)) {
        return false;
    }

    std::string registryIntegrity = packageIt->second.integrity;
    if (registryIntegrity.empty() &&
        !tryLoadRegistryPackageIntegrity(registryRoot, dependencyName, dependencyVersion,
                                         &registryIntegrity, errorMessage)) {
        return false;
    }
    if (registryIntegrity.empty() &&
        !computePackageIntegrity(dependencyManifest.packageRoot, &registryIntegrity,
                                 errorMessage)) {
        return false;
    }
    if (!packageIt->second.integrity.empty() &&
        registryIntegrity != packageIt->second.integrity) {
        return setError(errorMessage,
                        "Integrity mismatch for '" + locator +
                        "'. Expected '" + packageIt->second.integrity +
                        "' but registry has '" + registryIntegrity + "'.");
    }

    std::filesystem::path childTargetRoot =
        installedDependencyRoot(ownerPackageRoot, dependencyName, dependencyVersion);
    if (directoryExists(childTargetRoot) &&
        !removeDirectoryTree(childTargetRoot, errorMessage)) {
        return false;
    }

    if (!copyPackageTree(dependencyManifest.packageRoot, childTargetRoot, errorMessage)) {
        return false;
    }

    activeLocators->insert(locator);
    auto orderedDependencies =
        sortedPairs<std::pair<std::string, std::string>>(packageIt->second.dependencies);
    for (const auto& dependency : orderedDependencies) {
        if (!installLockedDependencyGraph(childTargetRoot, registryRoot, rootLock,
                                          dependency.first, dependency.second,
                                          activeLocators, errorMessage)) {
            activeLocators->erase(locator);
            return false;
        }
    }
    activeLocators->erase(locator);

    PackageLockData childLock;
    if (!buildSubLockForInstalledPackage(rootLock, locator, childTargetRoot,
                                         &childLock, errorMessage) ||
        !writePackageLockData(childTargetRoot, childLock, errorMessage)) {
        return false;
    }

    return true;
}

bool installPackageDependenciesFromLockData(const std::filesystem::path& packageRoot,
                                            const std::filesystem::path& registryRoot,
                                            const PackageLockData& lockData,
                                            std::string* errorMessage) {
    std::filesystem::path installedRoot = installedPackagesRoot(packageRoot);
    if (directoryExists(installedRoot) &&
        !removeDirectoryTree(installedRoot, errorMessage)) {
        return false;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(installedRoot, errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create installed package directory '" +
                        displayPath(installedRoot) + "'.");
    }

    std::unordered_set<std::string> activeLocators;
    auto orderedDependencies =
        sortedPairs<std::pair<std::string, std::string>>(lockData.directDependencies);
    for (const auto& dependency : orderedDependencies) {
        if (!installLockedDependencyGraph(packageRoot, registryRoot, lockData,
                                          dependency.first, dependency.second,
                                          &activeLocators, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool resolveInstalledPackageEntry(const std::filesystem::path& ownerPackageRoot,
                                  const PackageLockData& lockData,
                                  const std::string& dependencyName,
                                  const std::string& dependencyVersion,
                                  const std::string& submodulePath,
                                  std::filesystem::path* resolvedPath,
                                  std::string* errorMessage) {
    std::filesystem::path dependencyRoot =
        installedDependencyRoot(ownerPackageRoot, dependencyName, dependencyVersion);
    if (!directoryExists(dependencyRoot) || !fileExists(dependencyRoot / "uranium.pkg")) {
        return setError(errorMessage,
                        "Dependency '" + dependencyName + "@" + dependencyVersion +
                        "' is locked but not installed under '" +
                        displayPath(ownerPackageRoot) +
                        "'. Run `uranium --install` for this package.");
    }

    auto assignIfExists = [&](const std::filesystem::path& candidate) -> bool {
        if (!fileExists(candidate)) {
            return false;
        }

        *resolvedPath = canonicalize(candidate);
        return true;
    };

    if (submodulePath.empty()) {
        std::string locator = packageLocator(dependencyName, dependencyVersion);
        std::string entry = "src/main.ur";
        auto entryIt = lockData.packages.find(locator);
        if (entryIt != lockData.packages.end() && !entryIt->second.entry.empty()) {
            entry = entryIt->second.entry;
        }

        std::filesystem::path candidate = dependencyRoot / entry;
        if (assignIfExists(candidate)) {
            return true;
        }

        PackageManifestData manifest;
        if (!loadPackageManifestData(dependencyRoot, &manifest, errorMessage)) {
            return false;
        }

        candidate = dependencyRoot / manifest.entry;
        if (assignIfExists(candidate)) {
            return true;
        }

        return setError(errorMessage,
                        "Installed dependency '" + dependencyName +
                        "' has no readable entry file.");
    }

    std::filesystem::path candidate = dependencyRoot / submodulePath;
    if (candidate.extension().empty()) {
        std::filesystem::path sourceCandidate = candidate;
        sourceCandidate += ".ur";
        if (assignIfExists(sourceCandidate)) {
            return true;
        }
    } else if (assignIfExists(candidate)) {
        return true;
    }

    std::filesystem::path indexCandidate = dependencyRoot / submodulePath / "index.ur";
    if (assignIfExists(indexCandidate)) {
        return true;
    }

    return setError(errorMessage,
                    "Installed dependency '" + dependencyName +
                    "' has no module '" + submodulePath + "'.");
}

} // namespace

bool loadPackageManifestData(const std::filesystem::path& packageRoot,
                             PackageManifestData* manifest,
                             std::string* errorMessage) {
    std::filesystem::path canonicalRoot = canonicalize(packageRoot);
    std::filesystem::path manifestPath = manifestPathForPackage(canonicalRoot);
    if (!fileExists(manifestPath)) {
        return setError(errorMessage,
                        "Package manifest does not exist: " + displayPath(manifestPath));
    }

    if (!readTextFile(manifestPath, &manifest->rawText, errorMessage)) {
        return false;
    }

    JsonValue root;
    std::string parseError;
    if (!parseJsonDocument(manifest->rawText, &root, &parseError)) {
        return setError(errorMessage,
                        "Invalid package manifest '" + displayPath(manifestPath) +
                        "': " + parseError);
    }

    manifest->packageRoot = canonicalRoot;
    manifest->name = canonicalRoot.filename().string();
    manifest->version = "0.1.0";
    manifest->entry.clear();
    manifest->tests = "tests";
    manifest->dependencies.clear();

    const std::string documentName = "Package manifest '" + displayPath(manifestPath) + "'";
    if (!extractOptionalStringField(root, "name", &manifest->name, documentName, errorMessage) ||
        !extractOptionalStringField(root, "version", &manifest->version, documentName, errorMessage) ||
        !extractOptionalStringField(root, "entry", &manifest->entry, documentName, errorMessage) ||
        !extractOptionalStringField(root, "tests", &manifest->tests, documentName, errorMessage) ||
        !extractStringMapField(root, "dependencies", &manifest->dependencies,
                               documentName, errorMessage)) {
        return false;
    }

    if (manifest->name.empty()) {
        return setError(errorMessage, documentName + " has an empty 'name' field.");
    }
    if (manifest->version.empty()) {
        return setError(errorMessage, documentName + " has an empty 'version' field.");
    }
    if (manifest->entry.empty()) {
        return setError(errorMessage, documentName + " is missing a string 'entry' field.");
    }

    return true;
}

bool loadPackageLockData(const std::filesystem::path& packageRoot,
                         PackageLockData* lockData,
                         std::string* errorMessage) {
    std::filesystem::path lockPath = packageLockFilePath(packageRoot);
    std::string text;
    if (!readTextFile(lockPath, &text, errorMessage)) {
        return false;
    }

    JsonValue root;
    std::string parseError;
    if (!parseJsonDocument(text, &root, &parseError)) {
        return setError(errorMessage,
                        "Invalid package lock '" + displayPath(lockPath) + "': " + parseError);
    }

    lockData->name.clear();
    lockData->version.clear();
    lockData->registryPath.clear();
    lockData->directDependencies.clear();
    lockData->packages.clear();
    lockData->rawText = text;
    lockData->lockPath = canonicalize(lockPath);

    const std::string documentName = "Package lock '" + displayPath(lockPath) + "'";
    if (!extractOptionalStringField(root, "name", &lockData->name, documentName, errorMessage) ||
        !extractOptionalStringField(root, "version", &lockData->version, documentName, errorMessage) ||
        !extractOptionalStringField(root, "registry", &lockData->registryPath,
                                    documentName, errorMessage) ||
        !extractStringMapField(root, "directDependencies", &lockData->directDependencies,
                               documentName, errorMessage)) {
        return false;
    }

    const JsonValue* packagesField = findObjectField(root, "packages");
    if (packagesField != nullptr) {
        if (packagesField->kind != JSON_VALUE_OBJECT) {
            return setError(errorMessage, documentName + " field 'packages' must be an object.");
        }

        for (const auto& packagePair : packagesField->objectValues) {
            if (packagePair.second.kind != JSON_VALUE_OBJECT) {
                return setError(errorMessage,
                                documentName + " field 'packages' must contain objects.");
            }

            PackageLockEntryData entry;
            if (!extractOptionalStringField(packagePair.second, "name", &entry.name,
                                            documentName, errorMessage) ||
                !extractOptionalStringField(packagePair.second, "version", &entry.version,
                                            documentName, errorMessage) ||
                !extractOptionalStringField(packagePair.second, "entry", &entry.entry,
                                            documentName, errorMessage) ||
                !extractOptionalStringField(packagePair.second, "integrity", &entry.integrity,
                                            documentName, errorMessage) ||
                !extractStringMapField(packagePair.second, "dependencies", &entry.dependencies,
                                       documentName, errorMessage)) {
                return false;
            }

            if (entry.name.empty() || entry.version.empty() || entry.entry.empty()) {
                return setError(errorMessage,
                                documentName + " package entry '" + packagePair.first +
                                "' is missing name, version, or entry.");
            }

            lockData->packages[packagePair.first] = std::move(entry);
        }
    }

    return true;
}

bool writePackageManifestData(const std::filesystem::path& packageRoot,
                              const PackageManifestData& manifest,
                              std::string* errorMessage) {
    std::filesystem::path canonicalRoot = canonicalize(packageRoot);
    return writeTextFile(manifestPathForPackage(canonicalRoot),
                         buildManifestJsonText(manifest),
                         errorMessage);
}

bool writePackageLockData(const std::filesystem::path& packageRoot,
                          const PackageLockData& lockData,
                          std::string* errorMessage) {
    return writeTextFile(packageLockFilePath(packageRoot),
                         buildLockJsonText(lockData),
                         errorMessage);
}

bool generatePackageLockData(const std::filesystem::path& packageRoot,
                             const std::filesystem::path& registryRoot,
                             PackageLockData* lockData,
                             std::string* errorMessage) {
    std::filesystem::path canonicalRegistry = canonicalize(registryRoot);
    if (!ensureRegistryLayout(canonicalRegistry, errorMessage)) {
        return false;
    }

    PackageManifestData manifest;
    if (!loadPackageManifestData(packageRoot, &manifest, errorMessage)) {
        return false;
    }

    std::unordered_set<std::string> activeLocators;
    activeLocators.insert(packageLocator(manifest.name, manifest.version));
    return buildPackageLockForSource(canonicalize(packageRoot), canonicalRegistry,
                                     lockData, &activeLocators, errorMessage);
}

bool installPackageDependencies(const std::filesystem::path& packageRoot,
                                const std::filesystem::path& registryRoot,
                                PackageLockData* installedLock,
                                std::string* errorMessage) {
    std::filesystem::path canonicalPackageRoot = canonicalize(packageRoot);
    std::filesystem::path canonicalRegistry = canonicalize(registryRoot);
    if (!ensureRegistryLayout(canonicalRegistry, errorMessage)) {
        return false;
    }

    PackageLockData lockData;
    std::filesystem::path lockPath = packageLockFilePath(canonicalPackageRoot);
    if (fileExists(lockPath)) {
        if (!loadPackageLockData(canonicalPackageRoot, &lockData, errorMessage)) {
            return false;
        }
    } else {
        if (!generatePackageLockData(canonicalPackageRoot, canonicalRegistry, &lockData,
                                     errorMessage) ||
            !writePackageLockData(canonicalPackageRoot, lockData, errorMessage)) {
            return false;
        }
    }

    if (lockData.registryPath.empty()) {
        lockData.registryPath = canonicalRegistry.generic_string();
    }

    if (!installPackageDependenciesFromLockData(canonicalPackageRoot, canonicalRegistry,
                                                lockData, errorMessage)) {
        return false;
    }

    if (installedLock != nullptr) {
        *installedLock = std::move(lockData);
    }

    return true;
}

bool updatePackageDependencies(const std::filesystem::path& packageRoot,
                               const std::filesystem::path& registryRoot,
                               PackageLockData* installedLock,
                               std::string* errorMessage) {
    std::filesystem::path canonicalPackageRoot = canonicalize(packageRoot);
    std::filesystem::path canonicalRegistry = canonicalize(registryRoot);
    if (!ensureRegistryLayout(canonicalRegistry, errorMessage)) {
        return false;
    }

    PackageLockData lockData;
    if (!generatePackageLockData(canonicalPackageRoot, canonicalRegistry, &lockData,
                                 errorMessage) ||
        !writePackageLockData(canonicalPackageRoot, lockData, errorMessage) ||
        !installPackageDependenciesFromLockData(canonicalPackageRoot, canonicalRegistry,
                                                lockData, errorMessage)) {
        return false;
    }

    if (installedLock != nullptr) {
        *installedLock = std::move(lockData);
    }

    return true;
}

bool removePackageDependency(const std::filesystem::path& packageRoot,
                             const std::filesystem::path& registryRoot,
                             const std::string& dependencyName,
                             PackageLockData* installedLock,
                             std::string* errorMessage) {
    if (dependencyName.empty()) {
        return setError(errorMessage, "Dependency name for removal cannot be empty.");
    }

    std::filesystem::path canonicalPackageRoot = canonicalize(packageRoot);
    PackageManifestData manifest;
    if (!loadPackageManifestData(canonicalPackageRoot, &manifest, errorMessage)) {
        return false;
    }

    auto dependencyIt = manifest.dependencies.find(dependencyName);
    if (dependencyIt == manifest.dependencies.end()) {
        return setError(errorMessage,
                        "Package '" + manifest.name + "' does not depend on '" +
                        dependencyName + "'.");
    }

    manifest.dependencies.erase(dependencyIt);
    if (!writePackageManifestData(canonicalPackageRoot, manifest, errorMessage)) {
        return false;
    }

    return updatePackageDependencies(canonicalPackageRoot, registryRoot,
                                     installedLock, errorMessage);
}

bool initializePackageRegistry(const std::filesystem::path& registryRoot,
                               std::string* errorMessage) {
    return ensureRegistryLayout(canonicalize(registryRoot), errorMessage);
}

bool publishPackageToRegistry(const std::filesystem::path& packageRoot,
                              const std::filesystem::path& registryRoot,
                              std::filesystem::path* publishedRoot,
                              std::string* errorMessage) {
    std::filesystem::path canonicalPackageRoot = canonicalize(packageRoot);
    std::filesystem::path canonicalRegistry = canonicalize(registryRoot);
    if (!ensureRegistryLayout(canonicalRegistry, errorMessage)) {
        return false;
    }

    PackageManifestData manifest;
    if (!loadPackageManifestData(canonicalPackageRoot, &manifest, errorMessage)) {
        return false;
    }

    std::filesystem::path registryTarget =
        registryPackageRoot(canonicalRegistry, manifest.name, manifest.version);
    if (directoryExists(registryTarget) && !isDirectoryEmpty(registryTarget)) {
        return setError(errorMessage,
                        "Registry already contains '" + manifest.name +
                        "' version '" + manifest.version + "'.");
    }

    if (!copyPackageTree(canonicalPackageRoot, registryTarget, errorMessage)) {
        return false;
    }

    std::string integrity;
    if (!computePackageIntegrity(registryTarget, &integrity, errorMessage)) {
        return false;
    }

    std::filesystem::path metadataPath =
        registryPackageMetadataPath(canonicalRegistry, manifest.name, manifest.version);
    if (!writeTextFile(metadataPath,
                       buildRegistryPackageMetadataJsonText(manifest, integrity),
                       errorMessage)) {
        return false;
    }

    if (publishedRoot != nullptr) {
        *publishedRoot = canonicalize(registryTarget);
    }

    return true;
}

bool findPackageRootForPath(const std::filesystem::path& startPath,
                            std::filesystem::path* packageRoot) {
    if (startPath.empty()) {
        return false;
    }

    std::filesystem::path current = startPath;
    if (fileExists(current)) {
        current = current.parent_path();
    }
    current = canonicalize(current);

    for (;;) {
        if (fileExists(current / "uranium.pkg")) {
            *packageRoot = current;
            return true;
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return false;
}

std::filesystem::path packageLockFilePath(const std::filesystem::path& packageRoot) {
    return canonicalize(packageRoot) / "uranium.lock";
}

std::filesystem::path defaultPackageRegistryPath(const std::filesystem::path& startPath) {
    std::filesystem::path current = startPath.empty()
                                        ? std::filesystem::current_path()
                                        : startPath;
    if (fileExists(current)) {
        current = current.parent_path();
    }
    current = canonicalize(current);

    std::filesystem::path fallback = current / ".uranium-registry";

    for (;;) {
        std::filesystem::path hiddenRegistry = current / ".uranium-registry";
        if (directoryExists(hiddenRegistry) || fileExists(registryMetadataPath(hiddenRegistry))) {
            return canonicalize(hiddenRegistry);
        }

        std::filesystem::path namedRegistry = current / "registry";
        if (directoryExists(namedRegistry) && fileExists(registryMetadataPath(namedRegistry))) {
            return canonicalize(namedRegistry);
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return canonicalize(fallback);
}

std::filesystem::path installedDependencyRoot(const std::filesystem::path& ownerPackageRoot,
                                              const std::string& packageName,
                                              const std::string& version) {
    return canonicalize(ownerPackageRoot) / ".uranium" / "packages" / packageName / version;
}

bool tryResolveInstalledPackageImport(const std::string& spec,
                                      const std::filesystem::path& importerPath,
                                      const std::filesystem::path& workingDirectory,
                                      bool* resolved,
                                      std::filesystem::path* resolvedPath,
                                      std::string* errorMessage) {
    *resolved = false;

    if (spec.empty() || spec[0] == '.' || spec[0] == '"' || spec[0] == '/') {
        return true;
    }

    std::size_t slashIndex = spec.find('/');
    std::string packageName =
        slashIndex == std::string::npos ? spec : spec.substr(0, slashIndex);
    std::string submodulePath =
        slashIndex == std::string::npos ? "" : spec.substr(slashIndex + 1);
    if (packageName.empty()) {
        return true;
    }

    std::filesystem::path packageRoot;
    if (!findPackageRootForPath(importerPath, &packageRoot) &&
        !findPackageRootForPath(workingDirectory, &packageRoot)) {
        return true;
    }

    std::filesystem::path lockPath = packageLockFilePath(packageRoot);
    if (!fileExists(lockPath)) {
        return true;
    }

    PackageLockData lockData;
    if (!loadPackageLockData(packageRoot, &lockData, errorMessage)) {
        return false;
    }

    auto versionIt = lockData.directDependencies.find(packageName);
    if (versionIt == lockData.directDependencies.end()) {
        return true;
    }

    if (!resolveInstalledPackageEntry(packageRoot, lockData, packageName, versionIt->second,
                                      submodulePath, resolvedPath, errorMessage)) {
        return false;
    }

    *resolved = true;
    return true;
}
