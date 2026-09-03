#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif
#include "godot_native.h"
#include "heap.h"
#include "object.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

bool ensureMap(const Value& value,
               const std::string& functionName,
               int index,
               std::string* errorMessage) {
    if (value.isMap()) {
        return true;
    }

    return setError(errorMessage,
                    functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a map.");
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

int asInt(const Value& value) {
    return static_cast<int>(value.asNumber());
}

std::string normalizePathString(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

bool fileExists(const fs::path& path) {
    std::error_code errorCode;
    return fs::exists(path, errorCode) && fs::is_regular_file(path, errorCode);
}

bool directoryExists(const fs::path& path) {
    std::error_code errorCode;
    return fs::exists(path, errorCode) && fs::is_directory(path, errorCode);
}

bool ensureParentDirectory(const fs::path& path, std::string* errorMessage) {
    fs::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code errorCode;
    fs::create_directories(parent, errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create directory '" + normalizePathString(parent) + "'.");
    }

    return true;
}

bool writeTextFile(const fs::path& path,
                   const std::string& text,
                   std::string* errorMessage) {
    if (!ensureParentDirectory(path, errorMessage)) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return setError(errorMessage,
                        "Could not open file '" + normalizePathString(path) + "' for writing.");
    }

    file << text;
    if (!file.good()) {
        return setError(errorMessage,
                        "Could not write file '" + normalizePathString(path) + "'.");
    }

    return true;
}

Value makeStringArray(const std::vector<std::string>& values) {
    ArrayPtr array = uraniumHeap().allocateArray();
    for (const std::string& value : values) {
        array->elements.push_back(Value::stringValue(value));
    }
    return Value::arrayValue(array);
}

MapPtr makeResultMap(const std::vector<std::string>& createdFiles) {
    MapPtr result = uraniumHeap().allocateMap();
    result->entries["created"] = makeStringArray(createdFiles);
    result->entries["createdCount"] =
        Value::numberValue(static_cast<double>(createdFiles.size()));
    return result;
}

const Value* mapLookup(const MapPtr& map, const std::string& key) {
    if (map == nullptr) {
        return nullptr;
    }

    auto iterator = map->entries.find(key);
    if (iterator == map->entries.end()) {
        return nullptr;
    }

    return &iterator->second;
}

bool readOptionalString(MapPtr map,
                        const std::string& key,
                        const std::string& context,
                        std::string* value,
                        std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!entry->isString()) {
        return setError(errorMessage, context + " expects '" + key + "' to be a string.");
    }

    if (value != nullptr) {
        *value = entry->asString();
    }
    return true;
}

bool readOptionalBool(MapPtr map,
                      const std::string& key,
                      const std::string& context,
                      bool* value,
                      std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!entry->isBool()) {
        return setError(errorMessage, context + " expects '" + key + "' to be a boolean.");
    }

    if (value != nullptr) {
        *value = entry->asBool();
    }
    return true;
}

bool readOptionalInt(MapPtr map,
                     const std::string& key,
                     const std::string& context,
                     int* value,
                     std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!ensureWholeNumber(*entry, context, 0, errorMessage)) {
        if (errorMessage != nullptr && !errorMessage->empty()) {
            *errorMessage = context + " expects '" + key + "' to be a whole number.";
        }
        return false;
    }

    if (value != nullptr) {
        *value = asInt(*entry);
    }
    return true;
}

std::string lowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string trimAscii(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        start++;
    }

    std::size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string slugify(const std::string& value) {
    std::string lowered = lowerAscii(value);
    std::string result;
    bool lastSeparator = false;

    for (char character : lowered) {
        unsigned char code = static_cast<unsigned char>(character);
        if (std::isalnum(code)) {
            result.push_back(character);
            lastSeparator = false;
        } else if (!result.empty() && !lastSeparator) {
            result.push_back('_');
            lastSeparator = true;
        }
    }

    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }

    if (result.empty()) {
        return "godot_app";
    }

    if (std::isdigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), '_');
    }

    return result;
}

std::string pascalCase(const std::string& value) {
    std::string result;
    bool capitalize = true;

    for (char character : value) {
        unsigned char code = static_cast<unsigned char>(character);
        if (std::isalnum(code)) {
            result.push_back(static_cast<char>(
                capitalize ? std::toupper(code) : std::tolower(code)));
            capitalize = false;
        } else {
            capitalize = true;
        }
    }

    if (result.empty()) {
        return "BridgeClass";
    }

    if (std::isdigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), 'G');
    }

    return result;
}

std::string screamingSnake(const std::string& value) {
    std::string slug = slugify(value);
    for (char& character : slug) {
        character = character == '_'
                        ? '_'
                        : static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return slug;
}

std::string jsonQuoted(const std::string& value) {
    std::string result = "\"";
    for (unsigned char character : value) {
        switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char* hex = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(hex[(character >> 4) & 0x0F]);
                    result.push_back(hex[character & 0x0F]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    result.push_back('"');
    return result;
}

bool stringifyGodotLiteral(const Value& value,
                           std::string* out,
                           std::string* errorMessage);

bool stringifyGodotArray(const ArrayPtr& array,
                         std::string* out,
                         std::string* errorMessage) {
    out->push_back('[');
    if (array != nullptr) {
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            if (index > 0) {
                out->append(", ");
            }
            if (!stringifyGodotLiteral(array->elements[index], out, errorMessage)) {
                return false;
            }
        }
    }
    out->push_back(']');
    return true;
}

bool stringifyGodotMap(const MapPtr& map,
                       std::string* out,
                       std::string* errorMessage) {
    if (map != nullptr) {
        const Value* raw = mapLookup(map, "__raw");
        if (raw != nullptr) {
            if (!raw->isString()) {
                return setError(errorMessage, "__raw values must be strings.");
            }
            out->append(raw->asString());
            return true;
        }
    }

    out->push_back('{');
    if (map != nullptr) {
        std::vector<std::string> keys;
        keys.reserve(map->entries.size());
        for (const auto& entry : map->entries) {
            keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());

        bool first = true;
        for (const std::string& key : keys) {
            if (key == "__raw") {
                continue;
            }

            if (!first) {
                out->append(", ");
            }
            first = false;
            out->append(jsonQuoted(key));
            out->append(": ");
            if (!stringifyGodotLiteral(map->entries.at(key), out, errorMessage)) {
                return false;
            }
        }
    }
    out->push_back('}');
    return true;
}

bool stringifyGodotLiteral(const Value& value,
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
        std::ostringstream stream;
        stream << value.asNumber();
        out->append(stream.str());
        return true;
    }

    if (value.isString()) {
        out->append(jsonQuoted(value.asString()));
        return true;
    }

    if (value.isArray()) {
        return stringifyGodotArray(value.asArray(), out, errorMessage);
    }

    if (value.isMap()) {
        return stringifyGodotMap(value.asMap(), out, errorMessage);
    }

    return setError(errorMessage,
                    "Godot generation supports only nil, booleans, numbers, strings, arrays and maps.");
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        result += lines[index];
        if (index + 1 < lines.size()) {
            result.push_back('\n');
        }
    }
    return result;
}

bool readOptionalStringList(MapPtr map,
                            const std::string& key,
                            const std::string& context,
                            std::vector<std::string>* values,
                            std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!entry->isArray()) {
        return setError(errorMessage, context + " expects '" + key + "' to be an array.");
    }

    ArrayPtr array = entry->asArray();
    if (array == nullptr) {
        return true;
    }

    for (const Value& value : array->elements) {
        if (!value.isString()) {
            return setError(errorMessage,
                            context + " expects every '" + key + "' element to be a string.");
        }
        values->push_back(value.asString());
    }
    return true;
}

bool readOptionalTextBlock(MapPtr map,
                           const std::string& key,
                           const std::string& context,
                           std::string* text,
                           std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (entry->isString()) {
        *text = entry->asString();
        return true;
    }

    if (!entry->isArray()) {
        return setError(errorMessage,
                        context + " expects '" + key + "' to be a string or array.");
    }

    ArrayPtr array = entry->asArray();
    std::vector<std::string> lines;
    if (array != nullptr) {
        for (const Value& value : array->elements) {
            if (!value.isString()) {
                return setError(errorMessage,
                                context + " expects every '" + key + "' line to be a string.");
            }
            lines.push_back(value.asString());
        }
    }

    *text = joinLines(lines);
    return true;
}

bool readOptionalMap(MapPtr map,
                     const std::string& key,
                     const std::string& context,
                     MapPtr* out,
                     std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!entry->isMap()) {
        return setError(errorMessage, context + " expects '" + key + "' to be a map.");
    }

    if (out != nullptr) {
        *out = entry->asMap();
    }
    return true;
}

bool readOptionalArray(MapPtr map,
                       const std::string& key,
                       const std::string& context,
                       ArrayPtr* out,
                       std::string* errorMessage) {
    const Value* entry = mapLookup(map, key);
    if (entry == nullptr || entry->isNil()) {
        return true;
    }

    if (!entry->isArray()) {
        return setError(errorMessage, context + " expects '" + key + "' to be an array.");
    }

    if (out != nullptr) {
        *out = entry->asArray();
    }
    return true;
}

std::string ensureTrailingNewline(std::string text) {
    if (text.empty() || text.back() != '\n') {
        text.push_back('\n');
    }
    return text;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n' && (lines.empty() || !lines.back().empty())) {
        lines.push_back("");
    }
    return lines;
}

std::string indentLines(const std::string& text, const std::string& prefix) {
    std::vector<std::string> lines = splitLines(text);
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!lines[index].empty()) {
            result += prefix + lines[index];
        }
        if (index + 1 < lines.size()) {
            result.push_back('\n');
        }
    }
    if (!result.empty() && result.back() != '\n') {
        result.push_back('\n');
    }
    return result;
}

std::string stripExtension(std::string value) {
    std::size_t dot = value.find_last_of('.');
    if (dot != std::string::npos) {
        value.erase(dot);
    }
    return value;
}

fs::path findGodotProjectRoot(fs::path start) {
    if (start.empty()) {
        return {};
    }

    std::error_code errorCode;
    fs::path current = fs::absolute(start, errorCode);
    if (errorCode) {
        current = start;
    }

    for (;;) {
        if (fileExists(current / "project.godot")) {
            return current;
        }

        fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

std::string toGodotResourcePath(const std::string& value,
                                const fs::path& projectRoot,
                                const fs::path& anchorDir) {
    if (value.rfind("res://", 0) == 0 || value.rfind("uid://", 0) == 0) {
        return value;
    }

    fs::path path(value);
    if (path.is_relative()) {
        if (!anchorDir.empty()) {
            path = anchorDir / path;
        } else if (!projectRoot.empty()) {
            path = projectRoot / path;
        }
    }

    std::error_code errorCode;
    if (!projectRoot.empty()) {
        fs::path relative = fs::relative(path, projectRoot, errorCode);
        if (!errorCode) {
            return "res://" + normalizePathString(relative);
        }
    }

    return normalizePathString(path);
}

struct EditorDiscoveryResult {
    bool found = false;
    std::string path;
    std::string source = "none";
};

std::vector<std::string> splitPathEnv(const std::string& raw) {
    std::vector<std::string> parts;
    std::string current;
    for (char character : raw) {
#ifdef _WIN32
        if (character == ';') {
#else
        if (character == ':') {
#endif
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

std::vector<fs::path> scanPathCandidates() {
    std::vector<fs::path> matches;
    const char* rawPath = std::getenv("PATH");
    if (rawPath == nullptr) {
        return matches;
    }

    std::set<std::string> seen;
    for (const std::string& entry : splitPathEnv(rawPath)) {
        fs::path directory(entry);
        std::error_code errorCode;
        if (!directoryExists(directory)) {
            continue;
        }

        for (const auto& item : fs::directory_iterator(directory, errorCode)) {
            if (errorCode) {
                break;
            }

            const fs::path candidate = item.path();
            if (!fileExists(candidate)) {
                continue;
            }

            std::string filename = lowerAscii(candidate.filename().string());
#ifdef _WIN32
            if (candidate.extension() != ".exe") {
                continue;
            }
#endif
            if (filename.rfind("godot", 0) != 0) {
                continue;
            }

            std::string normalized = normalizePathString(candidate);
            if (seen.insert(normalized).second) {
                matches.push_back(candidate);
            }
        }
    }

    std::sort(matches.begin(), matches.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.filename().string() < right.filename().string();
              });
    return matches;
}

EditorDiscoveryResult detectGodotEditor(const std::string& hint) {
    std::vector<std::pair<std::string, std::string>> candidates;
    if (!trimAscii(hint).empty()) {
        candidates.push_back({hint, "hint"});
    }

    const char* envNames[] = {
        "GODOT_EXE",
        "GODOT_BIN",
        "GODOT4_EXE",
        "GODOT4_BIN",
    };

    for (const char* envName : envNames) {
        const char* value = std::getenv(envName);
        if (value != nullptr && *value != '\0') {
            candidates.push_back({value, std::string("env:") + envName});
        }
    }

    for (const fs::path& candidate : scanPathCandidates()) {
        candidates.push_back({normalizePathString(candidate), "path"});
    }

    for (const auto& candidate : candidates) {
        fs::path path(candidate.first);
        if (fileExists(path)) {
            EditorDiscoveryResult result;
            result.found = true;
            result.path = normalizePathString(path);
            result.source = candidate.second;
            return result;
        }
    }

    return EditorDiscoveryResult();
}

std::string buildPackedStringArray(const std::vector<std::string>& values) {
    std::string result = "PackedStringArray(";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            result += ", ";
        }
        result += jsonQuoted(values[index]);
    }
    result.push_back(')');
    return result;
}

std::string buildProjectFileText(const fs::path& rootPath,
                                 MapPtr options,
                                 std::string* errorMessage) {
    std::string projectName = rootPath.filename().generic_string();
    if (projectName.empty()) {
        projectName = "UraniumGodotProject";
    }

    std::string mainScene = "res://scenes/main.tscn";
    std::string renderer = "forward_plus";
    std::string iconPath;
    int width = 1280;
    int height = 720;
    std::vector<std::string> features;

    if (!readOptionalString(options, "name", "godotCreateProject", &projectName, errorMessage) ||
        !readOptionalString(options, "mainScene", "godotCreateProject", &mainScene, errorMessage) ||
        !readOptionalString(options, "renderer", "godotCreateProject", &renderer, errorMessage) ||
        !readOptionalString(options, "icon", "godotCreateProject", &iconPath, errorMessage) ||
        !readOptionalInt(options, "width", "godotCreateProject", &width, errorMessage) ||
        !readOptionalInt(options, "height", "godotCreateProject", &height, errorMessage) ||
        !readOptionalStringList(options, "features", "godotCreateProject", &features, errorMessage)) {
        return {};
    }

    if (!mainScene.empty() && mainScene.rfind("res://", 0) != 0) {
        mainScene = toGodotResourcePath(mainScene, rootPath, rootPath);
    }
    if (!iconPath.empty() && iconPath.rfind("res://", 0) != 0) {
        iconPath = toGodotResourcePath(iconPath, rootPath, rootPath);
    }

    std::vector<std::string> lines;
    lines.push_back("; Engine configuration file.");
    lines.push_back("; It's best edited using the Godot editor and not directly.");
    lines.push_back("");
    lines.push_back("config_version=5");
    lines.push_back("");
    lines.push_back("[application]");
    lines.push_back("");
    lines.push_back("config/name=" + jsonQuoted(projectName));
    if (!mainScene.empty()) {
        lines.push_back("run/main_scene=" + jsonQuoted(mainScene));
    }
    if (!features.empty()) {
        lines.push_back("config/features=" + buildPackedStringArray(features));
    }
    if (!iconPath.empty()) {
        lines.push_back("config/icon=" + jsonQuoted(iconPath));
    }
    lines.push_back("");
    lines.push_back("[display]");
    lines.push_back("");
    lines.push_back("window/size/viewport_width=" + std::to_string(width));
    lines.push_back("window/size/viewport_height=" + std::to_string(height));
    lines.push_back("");
    lines.push_back("[rendering]");
    lines.push_back("");
    lines.push_back("renderer/rendering_method=" + jsonQuoted(renderer));
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildDefaultGdScript(const std::string& extendsName,
                                 const std::string& className,
                                 bool toolScript) {
    std::vector<std::string> lines;
    if (toolScript) {
        lines.push_back("@tool");
    }
    lines.push_back("extends " + extendsName);
    if (!className.empty()) {
        lines.push_back("class_name " + className);
    }
    lines.push_back("");
    lines.push_back("func _ready():");
    lines.push_back("    pass");
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildDefaultCSharp(const std::string& extendsName,
                               const std::string& className,
                               bool toolScript,
                               const std::string& rawBody) {
    std::vector<std::string> lines;
    lines.push_back("using Godot;");
    lines.push_back("using System;");
    lines.push_back("");
    if (toolScript) {
        lines.push_back("[Tool]");
    }
    lines.push_back("public partial class " + className + " : " + extendsName);
    lines.push_back("{");
    if (trimAscii(rawBody).empty()) {
        lines.push_back("    public override void _Ready()");
        lines.push_back("    {");
        lines.push_back("    }");
    } else {
        std::vector<std::string> bodyLines = splitLines(indentLines(rawBody, "    "));
        for (const std::string& line : bodyLines) {
            lines.push_back(line);
        }
    }
    lines.push_back("}");
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildPluginScriptBody(const std::string& customBody) {
    if (!trimAscii(customBody).empty()) {
        return ensureTrailingNewline(customBody);
    }

    return
        "@tool\n"
        "extends EditorPlugin\n"
        "\n"
        "func _enter_tree():\n"
        "    pass\n"
        "\n"
        "func _exit_tree():\n"
        "    pass\n";
}

std::string buildPluginConfigText(const std::string& name,
                                  const std::string& description,
                                  const std::string& author,
                                  const std::string& version,
                                  const std::string& scriptResPath) {
    std::vector<std::string> lines;
    lines.push_back("[plugin]");
    lines.push_back("");
    lines.push_back("name=" + jsonQuoted(name));
    lines.push_back("description=" + jsonQuoted(description));
    lines.push_back("author=" + jsonQuoted(author));
    lines.push_back("version=" + jsonQuoted(version));
    lines.push_back("script=" + jsonQuoted(scriptResPath));
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildRegisterTypesHeader(const std::string& headerGuard,
                                     const std::string& moduleSlug) {
    std::vector<std::string> lines;
    lines.push_back("#ifndef " + headerGuard);
    lines.push_back("#define " + headerGuard);
    lines.push_back("");
    lines.push_back("#include <godot_cpp/core/class_db.hpp>");
    lines.push_back("");
    lines.push_back("void initialize_" + moduleSlug + "_module(godot::ModuleInitializationLevel p_level);");
    lines.push_back("void uninitialize_" + moduleSlug + "_module(godot::ModuleInitializationLevel p_level);");
    lines.push_back("");
    lines.push_back("#endif");
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildClassHeader(const std::string& headerGuard,
                             const std::string& namespaceName,
                             const std::string& className,
                             const std::string& inherits) {
    std::vector<std::string> lines;
    lines.push_back("#ifndef " + headerGuard);
    lines.push_back("#define " + headerGuard);
    lines.push_back("");
    lines.push_back("#include <godot_cpp/classes/" + lowerAscii(inherits) + ".hpp>");
    lines.push_back("");
    lines.push_back("namespace " + namespaceName + " {");
    lines.push_back("");
    lines.push_back("class " + className + " : public godot::" + inherits + " {");
    lines.push_back("    GDCLASS(" + className + ", godot::" + inherits + ")");
    lines.push_back("");
    lines.push_back("private:");
    lines.push_back("    double speed = 120.0;");
    lines.push_back("");
    lines.push_back("protected:");
    lines.push_back("    static void _bind_methods();");
    lines.push_back("");
    lines.push_back("public:");
    lines.push_back("    " + className + "() = default;");
    lines.push_back("    void hello();");
    lines.push_back("    void set_speed(double p_speed);");
    lines.push_back("    double get_speed() const;");
    lines.push_back("};");
    lines.push_back("");
    lines.push_back("} // namespace " + namespaceName);
    lines.push_back("");
    lines.push_back("#endif");
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildClassSource(const std::string& namespaceName,
                             const std::string& className,
                             const std::string& headerName) {
    std::vector<std::string> lines;
    lines.push_back("#include " + jsonQuoted(headerName));
    lines.push_back("");
    lines.push_back("#include <godot_cpp/core/class_db.hpp>");
    lines.push_back("#include <godot_cpp/variant/utility_functions.hpp>");
    lines.push_back("");
    lines.push_back("using namespace godot;");
    lines.push_back("");
    lines.push_back("namespace " + namespaceName + " {");
    lines.push_back("");
    lines.push_back("void " + className + "::_bind_methods() {");
    lines.push_back("    ClassDB::bind_method(D_METHOD(\"hello\"), &" + className + "::hello);");
    lines.push_back("    ClassDB::bind_method(D_METHOD(\"set_speed\", \"speed\"), &" + className + "::set_speed);");
    lines.push_back("    ClassDB::bind_method(D_METHOD(\"get_speed\"), &" + className + "::get_speed);");
    lines.push_back("    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, \"speed\"), \"set_speed\", \"get_speed\");");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("void " + className + "::hello() {");
    lines.push_back("    UtilityFunctions::print(\"" + className + " says hello from Uranium's Godot scaffold.\");");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("void " + className + "::set_speed(double p_speed) {");
    lines.push_back("    speed = p_speed;");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("double " + className + "::get_speed() const {");
    lines.push_back("    return speed;");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("} // namespace " + namespaceName);
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildRegisterTypesSource(const std::string& moduleSlug,
                                     const std::string& namespaceName,
                                     const std::string& className,
                                     const std::string& classHeaderName,
                                     const std::string& entrySymbol) {
    std::vector<std::string> lines;
    lines.push_back("#include \"register_types.h\"");
    lines.push_back("#include " + jsonQuoted(classHeaderName));
    lines.push_back("");
    lines.push_back("#include <godot_cpp/core/class_db.hpp>");
    lines.push_back("#include <godot_cpp/godot.hpp>");
    lines.push_back("");
    lines.push_back("using namespace godot;");
    lines.push_back("");
    lines.push_back("void initialize_" + moduleSlug + "_module(ModuleInitializationLevel p_level) {");
    lines.push_back("    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {");
    lines.push_back("        return;");
    lines.push_back("    }");
    lines.push_back("");
    lines.push_back("    ClassDB::register_class<" + namespaceName + "::" + className + ">();");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("void uninitialize_" + moduleSlug + "_module(ModuleInitializationLevel p_level) {");
    lines.push_back("    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {");
    lines.push_back("        return;");
    lines.push_back("    }");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("extern \"C\" {");
    lines.push_back("GDExtensionBool GDE_EXPORT " + entrySymbol + "(");
    lines.push_back("    GDExtensionInterfaceGetProcAddress p_get_proc_address,");
    lines.push_back("    const GDExtensionClassLibraryPtr p_library,");
    lines.push_back("    GDExtensionInitialization* r_initialization) {");
    lines.push_back("    GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);");
    lines.push_back("    init_obj.register_initializer(initialize_" + moduleSlug + "_module);");
    lines.push_back("    init_obj.register_terminator(uninitialize_" + moduleSlug + "_module);");
    lines.push_back("    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);");
    lines.push_back("    return init_obj.init();");
    lines.push_back("}");
    lines.push_back("}");
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildGDExtensionFileText(const std::string& entrySymbol,
                                     const std::string& libraryBase,
                                     const std::string& slug) {
    std::string baseRes = "res://native/" + slug + "/bin/" + libraryBase;
    std::vector<std::string> lines;
    lines.push_back("[configuration]");
    lines.push_back("");
    lines.push_back("entry_symbol=" + jsonQuoted(entrySymbol));
    lines.push_back("compatibility_minimum=" + jsonQuoted("4.2"));
    lines.push_back("");
    lines.push_back("[libraries]");
    lines.push_back("");
    lines.push_back("windows.debug.x86_64=" + jsonQuoted(baseRes + ".windows.template_debug.x86_64.dll"));
    lines.push_back("windows.release.x86_64=" + jsonQuoted(baseRes + ".windows.template_release.x86_64.dll"));
    lines.push_back("linux.debug.x86_64=" + jsonQuoted(baseRes + ".linux.template_debug.x86_64.so"));
    lines.push_back("linux.release.x86_64=" + jsonQuoted(baseRes + ".linux.template_release.x86_64.so"));
    lines.push_back("macos.debug=" + jsonQuoted(baseRes + ".macos.template_debug.framework"));
    lines.push_back("macos.release=" + jsonQuoted(baseRes + ".macos.template_release.framework"));
    return ensureTrailingNewline(joinLines(lines));
}

std::string buildGDExtensionReadme(const std::string& slug,
                                   const std::string& className) {
    std::vector<std::string> lines;
    lines.push_back("# " + className + " GDExtension Scaffold");
    lines.push_back("");
    lines.push_back("This scaffold was generated by Uranium's native Godot module.");
    lines.push_back("");
    lines.push_back("Suggested next steps:");
    lines.push_back("1. Clone the `godot-cpp` bindings that match your Godot version.");
    lines.push_back("2. Point your build system to the bindings include + generated headers.");
    lines.push_back("3. Build a shared library into `native/" + slug + "/bin/`.");
    lines.push_back("4. Open the project in Godot and enable the generated `.gdextension` file.");
    lines.push_back("");
    lines.push_back("Generated entry class:");
    lines.push_back("- " + className);
    return ensureTrailingNewline(joinLines(lines));
}

struct ExternalResource {
    std::string type;
    std::string path;
    std::string id;
};

std::string nextResourceId(std::size_t index) {
    return std::to_string(index + 1);
}

std::string sceneHeaderLine(const std::string& name,
                            const std::string& type,
                            const std::string& parent,
                            const std::string& instanceId) {
    std::string header = "[node name=" + jsonQuoted(name);
    if (!type.empty()) {
        header += " type=" + jsonQuoted(type);
    }
    if (!parent.empty()) {
        header += " parent=" + jsonQuoted(parent);
    }
    if (!instanceId.empty()) {
        header += " instance=ExtResource(" + jsonQuoted(instanceId) + ")";
    }
    header += "]";
    return header;
}

bool appendPropertyLines(const MapPtr& properties,
                         std::vector<std::string>* lines,
                         std::string* errorMessage) {
    if (properties == nullptr) {
        return true;
    }

    std::vector<std::string> keys;
    keys.reserve(properties->entries.size());
    for (const auto& entry : properties->entries) {
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    for (const std::string& key : keys) {
        std::string literal;
        if (!stringifyGodotLiteral(properties->entries.at(key), &literal, errorMessage)) {
            return false;
        }
        lines->push_back(key + " = " + literal);
    }

    return true;
}

std::string quoteShellArg(const std::string& value) {
    std::string result = "\"";
    for (char character : value) {
        if (character == '"') {
            result += "\\\"";
        } else {
            result.push_back(character);
        }
    }
    result.push_back('"');
    return result;
}

bool appendExtraArgs(MapPtr options,
                     std::string* command,
                     std::string* errorMessage) {
    const Value* extraArgs = mapLookup(options, "extraArgs");
    if (extraArgs == nullptr || extraArgs->isNil()) {
        return true;
    }

    if (!extraArgs->isArray()) {
        return setError(errorMessage, "godotBuildCommand expects 'extraArgs' to be an array.");
    }

    ArrayPtr array = extraArgs->asArray();
    if (array == nullptr) {
        return true;
    }

    for (const Value& value : array->elements) {
        if (!value.isString()) {
            return setError(errorMessage,
                            "godotBuildCommand expects every 'extraArgs' element to be a string.");
        }
        command->append(" ");
        command->append(quoteShellArg(value.asString()));
    }

    return true;
}

} // namespace

Value nativeGodotFindEditor(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "godotFindEditor", 0, errorMessage)) {
        return Value::nilValue();
    }

    EditorDiscoveryResult result = detectGodotEditor(args[0].asString());
    MapPtr map = uraniumHeap().allocateMap();
    map->entries["found"] = Value::boolValue(result.found);
    map->entries["path"] = result.found ? Value::stringValue(result.path) : Value::nilValue();
    map->entries["source"] = Value::stringValue(result.source);
    return Value::mapValue(map);
}

Value nativeGodotCreateProject(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotCreateProject", 0, errorMessage) ||
        !ensureMap(args[1], "godotCreateProject", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path rootPath(args[0].asString());
    MapPtr options = args[1].asMap();
    std::error_code errorCode;
    fs::create_directories(rootPath, errorCode);
    if (errorCode) {
        setError(errorMessage,
                 "Could not create Godot project directory '" +
                     normalizePathString(rootPath) + "'.");
        return Value::nilValue();
    }

    fs::path scenesDir = rootPath / "scenes";
    fs::path scriptsDir = rootPath / "scripts";
    fs::path assetsDir = rootPath / "assets";
    fs::path addonsDir = rootPath / "addons";
    fs::path nativeDir = rootPath / "native";
    fs::create_directories(scenesDir, errorCode);
    fs::create_directories(scriptsDir, errorCode);
    fs::create_directories(assetsDir, errorCode);
    fs::create_directories(addonsDir, errorCode);
    fs::create_directories(nativeDir, errorCode);

    std::string projectText = buildProjectFileText(rootPath, options, errorMessage);
    if (errorMessage != nullptr && !errorMessage->empty()) {
        return Value::nilValue();
    }

    std::vector<std::string> createdFiles;
    fs::path projectFile = rootPath / "project.godot";
    if (!writeTextFile(projectFile, projectText, errorMessage)) {
        return Value::nilValue();
    }
    createdFiles.push_back(normalizePathString(projectFile));

    bool addGitIgnore = true;
    if (!readOptionalBool(options, "gitIgnore", "godotCreateProject", &addGitIgnore, errorMessage)) {
        return Value::nilValue();
    }

    if (addGitIgnore) {
        fs::path gitIgnore = rootPath / ".gitignore";
        if (!fileExists(gitIgnore)) {
            std::string gitIgnoreText =
                ".godot/\n"
                ".import/\n"
                "export_presets.cfg\n";
            if (!writeTextFile(gitIgnore, gitIgnoreText, errorMessage)) {
                return Value::nilValue();
            }
            createdFiles.push_back(normalizePathString(gitIgnore));
        }
    }

    MapPtr result = makeResultMap(createdFiles);
    result->entries["root"] = Value::stringValue(normalizePathString(rootPath));
    result->entries["projectFile"] = Value::stringValue(normalizePathString(projectFile));
    result->entries["scenesDir"] = Value::stringValue(normalizePathString(scenesDir));
    result->entries["scriptsDir"] = Value::stringValue(normalizePathString(scriptsDir));
    result->entries["assetsDir"] = Value::stringValue(normalizePathString(assetsDir));
    result->entries["addonsDir"] = Value::stringValue(normalizePathString(addonsDir));
    result->entries["nativeDir"] = Value::stringValue(normalizePathString(nativeDir));
    return Value::mapValue(result);
}

Value nativeGodotCreateScript(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotCreateScript", 0, errorMessage) ||
        !ensureMap(args[1], "godotCreateScript", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path scriptPath(args[0].asString());
    MapPtr options = args[1].asMap();
    std::string language = "gdscript";
    std::string extendsName = "Node";
    std::string className = pascalCase(stripExtension(scriptPath.filename().generic_string()));
    std::string customText;
    std::string bodyText;
    bool toolScript = false;

    if (!readOptionalString(options, "language", "godotCreateScript", &language, errorMessage) ||
        !readOptionalString(options, "extends", "godotCreateScript", &extendsName, errorMessage) ||
        !readOptionalString(options, "className", "godotCreateScript", &className, errorMessage) ||
        !readOptionalTextBlock(options, "content", "godotCreateScript", &customText, errorMessage) ||
        !readOptionalTextBlock(options, "body", "godotCreateScript", &bodyText, errorMessage) ||
        !readOptionalBool(options, "tool", "godotCreateScript", &toolScript, errorMessage)) {
        return Value::nilValue();
    }

    std::string generated;
    std::string normalizedLanguage = lowerAscii(language);
    if (!trimAscii(customText).empty()) {
        generated = ensureTrailingNewline(customText);
    } else if (normalizedLanguage == "gdscript" || normalizedLanguage == "gd") {
        generated = trimAscii(bodyText).empty()
                        ? buildDefaultGdScript(extendsName, className, toolScript)
                        : ensureTrailingNewline(bodyText);
    } else if (normalizedLanguage == "csharp" || normalizedLanguage == "cs") {
        generated = buildDefaultCSharp(extendsName, className, toolScript, bodyText);
    } else {
        setError(errorMessage,
                 "godotCreateScript currently supports only 'gdscript' and 'csharp'.");
        return Value::nilValue();
    }

    if (!writeTextFile(scriptPath, generated, errorMessage)) {
        return Value::nilValue();
    }

    std::vector<std::string> createdFiles = {normalizePathString(scriptPath)};
    MapPtr result = makeResultMap(createdFiles);
    result->entries["path"] = Value::stringValue(normalizePathString(scriptPath));
    result->entries["language"] = Value::stringValue(normalizedLanguage);
    result->entries["className"] = Value::stringValue(className);
    result->entries["extends"] = Value::stringValue(extendsName);
    return Value::mapValue(result);
}

Value nativeGodotCreateScene(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotCreateScene", 0, errorMessage) ||
        !ensureMap(args[1], "godotCreateScene", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path scenePath(args[0].asString());
    MapPtr options = args[1].asMap();
    fs::path projectRoot = findGodotProjectRoot(scenePath.parent_path());
    fs::path anchorDir = scenePath.parent_path();

    std::string rootType = "Node2D";
    std::string rootName = "Main";
    std::string rootScript;
    MapPtr rootProperties = nullptr;
    ArrayPtr children = nullptr;

    if (!readOptionalString(options, "rootType", "godotCreateScene", &rootType, errorMessage) ||
        !readOptionalString(options, "rootName", "godotCreateScene", &rootName, errorMessage) ||
        !readOptionalString(options, "script", "godotCreateScene", &rootScript, errorMessage) ||
        !readOptionalMap(options, "properties", "godotCreateScene", &rootProperties, errorMessage) ||
        !readOptionalArray(options, "children", "godotCreateScene", &children, errorMessage)) {
        return Value::nilValue();
    }

    std::vector<ExternalResource> resources;
    auto acquireResource = [&](const std::string& type, const std::string& path) -> std::string {
        for (const ExternalResource& resource : resources) {
            if (resource.type == type && resource.path == path) {
                return resource.id;
            }
        }

        ExternalResource resource;
        resource.type = type;
        resource.path = path;
        resource.id = nextResourceId(resources.size());
        resources.push_back(resource);
        return resource.id;
    };

    std::string rootScriptId;
    if (!trimAscii(rootScript).empty()) {
        rootScriptId =
            acquireResource("Script", toGodotResourcePath(rootScript, projectRoot, anchorDir));
    }

    struct ChildNode {
        std::string name;
        std::string type;
        std::string parent;
        std::string scriptId;
        std::string instanceId;
        MapPtr properties = nullptr;
    };

    std::vector<ChildNode> childNodes;
    if (children != nullptr) {
        for (const Value& childValue : children->elements) {
            if (!childValue.isMap()) {
                setError(errorMessage, "godotCreateScene expects every child to be a map.");
                return Value::nilValue();
            }

            MapPtr child = childValue.asMap();
            std::string type;
            std::string name;
            std::string parent = ".";
            std::string scriptPath;
            std::string instancePath;
            MapPtr properties = nullptr;

            if (!readOptionalString(child, "type", "godotCreateScene child", &type, errorMessage) ||
                !readOptionalString(child, "name", "godotCreateScene child", &name, errorMessage) ||
                !readOptionalString(child, "parent", "godotCreateScene child", &parent, errorMessage) ||
                !readOptionalString(child, "script", "godotCreateScene child", &scriptPath, errorMessage) ||
                !readOptionalString(child, "instance", "godotCreateScene child", &instancePath, errorMessage) ||
                !readOptionalString(child, "scene", "godotCreateScene child", &instancePath, errorMessage) ||
                !readOptionalMap(child, "properties", "godotCreateScene child", &properties, errorMessage)) {
                return Value::nilValue();
            }

            if (trimAscii(type).empty() && trimAscii(instancePath).empty()) {
                setError(errorMessage,
                         "godotCreateScene child entries need either 'type' or 'instance'.");
                return Value::nilValue();
            }

            if (trimAscii(name).empty()) {
                name = !trimAscii(instancePath).empty()
                           ? pascalCase(stripExtension(fs::path(instancePath).filename().generic_string()))
                           : type;
            }

            ChildNode node;
            node.name = name;
            node.type = type;
            node.parent = parent;
            node.properties = properties;
            if (!trimAscii(scriptPath).empty()) {
                node.scriptId = acquireResource(
                    "Script", toGodotResourcePath(scriptPath, projectRoot, anchorDir));
            }
            if (!trimAscii(instancePath).empty()) {
                node.instanceId = acquireResource(
                    "PackedScene", toGodotResourcePath(instancePath, projectRoot, anchorDir));
            }
            childNodes.push_back(node);
        }
    }

    std::vector<std::string> lines;
    lines.push_back("[gd_scene load_steps=" + std::to_string(1 + resources.size()) + " format=3]");
    lines.push_back("");
    for (const ExternalResource& resource : resources) {
        lines.push_back("[ext_resource type=" + jsonQuoted(resource.type) +
                        " path=" + jsonQuoted(resource.path) +
                        " id=" + jsonQuoted(resource.id) + "]");
        lines.push_back("");
    }

    lines.push_back(sceneHeaderLine(rootName, rootType, "", ""));
    if (!rootScriptId.empty()) {
        lines.push_back("script = ExtResource(" + jsonQuoted(rootScriptId) + ")");
    }
    if (!appendPropertyLines(rootProperties, &lines, errorMessage)) {
        return Value::nilValue();
    }
    lines.push_back("");

    for (const ChildNode& child : childNodes) {
        lines.push_back(sceneHeaderLine(child.name,
                                        child.instanceId.empty() ? child.type : "",
                                        child.parent,
                                        child.instanceId));
        if (!child.scriptId.empty()) {
            lines.push_back("script = ExtResource(" + jsonQuoted(child.scriptId) + ")");
        }
        if (!appendPropertyLines(child.properties, &lines, errorMessage)) {
            return Value::nilValue();
        }
        lines.push_back("");
    }

    if (!writeTextFile(scenePath, ensureTrailingNewline(joinLines(lines)), errorMessage)) {
        return Value::nilValue();
    }

    std::vector<std::string> createdFiles = {normalizePathString(scenePath)};
    MapPtr result = makeResultMap(createdFiles);
    result->entries["path"] = Value::stringValue(normalizePathString(scenePath));
    result->entries["rootType"] = Value::stringValue(rootType);
    result->entries["rootName"] = Value::stringValue(rootName);
    result->entries["resourceCount"] =
        Value::numberValue(static_cast<double>(resources.size()));
    return Value::mapValue(result);
}

Value nativeGodotCreatePlugin(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotCreatePlugin", 0, errorMessage) ||
        !ensureMap(args[1], "godotCreatePlugin", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path projectRoot(args[0].asString());
    MapPtr options = args[1].asMap();
    std::string name = projectRoot.filename().generic_string() + " Tools";
    std::string slug = slugify(name);
    std::string author = "Uranium";
    std::string version = "1.0.0";
    std::string description = "Generated by Uranium's Godot module.";
    std::string body;

    if (!readOptionalString(options, "name", "godotCreatePlugin", &name, errorMessage) ||
        !readOptionalString(options, "slug", "godotCreatePlugin", &slug, errorMessage) ||
        !readOptionalString(options, "author", "godotCreatePlugin", &author, errorMessage) ||
        !readOptionalString(options, "version", "godotCreatePlugin", &version, errorMessage) ||
        !readOptionalString(options, "description", "godotCreatePlugin", &description, errorMessage) ||
        !readOptionalTextBlock(options, "body", "godotCreatePlugin", &body, errorMessage)) {
        return Value::nilValue();
    }

    slug = slugify(slug);
    fs::path addonDir = projectRoot / "addons" / slug;
    fs::path pluginConfig = addonDir / "plugin.cfg";
    fs::path pluginScript = addonDir / "plugin.gd";
    std::vector<std::string> createdFiles;

    if (!writeTextFile(pluginScript, buildPluginScriptBody(body), errorMessage)) {
        return Value::nilValue();
    }
    createdFiles.push_back(normalizePathString(pluginScript));

    std::string scriptResPath = "res://addons/" + slug + "/plugin.gd";
    if (!writeTextFile(pluginConfig,
                       buildPluginConfigText(name, description, author, version, scriptResPath),
                       errorMessage)) {
        return Value::nilValue();
    }
    createdFiles.push_back(normalizePathString(pluginConfig));

    MapPtr result = makeResultMap(createdFiles);
    result->entries["name"] = Value::stringValue(name);
    result->entries["slug"] = Value::stringValue(slug);
    result->entries["addonDir"] = Value::stringValue(normalizePathString(addonDir));
    result->entries["configPath"] = Value::stringValue(normalizePathString(pluginConfig));
    result->entries["scriptPath"] = Value::stringValue(normalizePathString(pluginScript));
    return Value::mapValue(result);
}

Value nativeGodotCreateGDExtension(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotCreateGDExtension", 0, errorMessage) ||
        !ensureMap(args[1], "godotCreateGDExtension", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path projectRoot(args[0].asString());
    MapPtr options = args[1].asMap();
    std::string name = projectRoot.filename().generic_string() + " Native";
    std::string slug = slugify(name);
    std::string className = pascalCase(name) + "Bridge";
    std::string namespaceName = slugify(name);
    std::string entrySymbol;
    std::string inherits = "Node";

    if (!readOptionalString(options, "name", "godotCreateGDExtension", &name, errorMessage) ||
        !readOptionalString(options, "slug", "godotCreateGDExtension", &slug, errorMessage) ||
        !readOptionalString(options, "className", "godotCreateGDExtension", &className, errorMessage) ||
        !readOptionalString(options, "namespace", "godotCreateGDExtension", &namespaceName, errorMessage) ||
        !readOptionalString(options, "entrySymbol", "godotCreateGDExtension", &entrySymbol, errorMessage) ||
        !readOptionalString(options, "inherits", "godotCreateGDExtension", &inherits, errorMessage)) {
        return Value::nilValue();
    }

    slug = slugify(slug);
    namespaceName = slugify(namespaceName);
    if (entrySymbol.empty()) {
        entrySymbol = slug + "_library_init";
    }

    std::string classSnake = slugify(className);
    std::string classHeaderName = classSnake + ".h";
    std::string classSourceName = classSnake + ".cpp";
    std::string libraryBase = "lib" + slug;

    fs::path addonDir = projectRoot / "addons" / slug;
    fs::path nativeDir = projectRoot / "native" / slug;
    fs::path sourceDir = nativeDir / "src";
    fs::path binDir = nativeDir / "bin";
    fs::path extensionPath = addonDir / (slug + ".gdextension");
    fs::path readmePath = nativeDir / "README.md";
    fs::path registerHeaderPath = sourceDir / "register_types.h";
    fs::path registerSourcePath = sourceDir / "register_types.cpp";
    fs::path classHeaderPath = sourceDir / classHeaderName;
    fs::path classSourcePath = sourceDir / classSourceName;

    std::vector<std::string> createdFiles;
    if (!writeTextFile(registerHeaderPath,
                       buildRegisterTypesHeader(screamingSnake(slug + "_REGISTER_TYPES_H"), slug),
                       errorMessage) ||
        !writeTextFile(classHeaderPath,
                       buildClassHeader(screamingSnake(className + "_H"),
                                        namespaceName,
                                        className,
                                        inherits),
                       errorMessage) ||
        !writeTextFile(classSourcePath,
                       buildClassSource(namespaceName, className, classHeaderName),
                       errorMessage) ||
        !writeTextFile(registerSourcePath,
                       buildRegisterTypesSource(slug,
                                                namespaceName,
                                                className,
                                                classHeaderName,
                                                entrySymbol),
                       errorMessage) ||
        !writeTextFile(extensionPath,
                       buildGDExtensionFileText(entrySymbol, libraryBase, slug),
                       errorMessage) ||
        !writeTextFile(readmePath,
                       buildGDExtensionReadme(slug, className),
                       errorMessage)) {
        return Value::nilValue();
    }

    createdFiles.push_back(normalizePathString(registerHeaderPath));
    createdFiles.push_back(normalizePathString(registerSourcePath));
    createdFiles.push_back(normalizePathString(classHeaderPath));
    createdFiles.push_back(normalizePathString(classSourcePath));
    createdFiles.push_back(normalizePathString(extensionPath));
    createdFiles.push_back(normalizePathString(readmePath));

    std::error_code errorCode;
    fs::create_directories(binDir, errorCode);

    MapPtr result = makeResultMap(createdFiles);
    result->entries["slug"] = Value::stringValue(slug);
    result->entries["className"] = Value::stringValue(className);
    result->entries["namespace"] = Value::stringValue(namespaceName);
    result->entries["entrySymbol"] = Value::stringValue(entrySymbol);
    result->entries["addonDir"] = Value::stringValue(normalizePathString(addonDir));
    result->entries["nativeDir"] = Value::stringValue(normalizePathString(nativeDir));
    result->entries["sourceDir"] = Value::stringValue(normalizePathString(sourceDir));
    result->entries["binDir"] = Value::stringValue(normalizePathString(binDir));
    result->entries["gdextensionPath"] = Value::stringValue(normalizePathString(extensionPath));
    return Value::mapValue(result);
}

Value nativeGodotBuildCommand(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "godotBuildCommand", 0, errorMessage) ||
        !ensureMap(args[1], "godotBuildCommand", 1, errorMessage)) {
        return Value::nilValue();
    }

    fs::path projectRoot(args[0].asString());
    MapPtr options = args[1].asMap();
    std::string executable;
    std::string mode = "editor";
    std::string scene;
    bool headless = false;

    if (!readOptionalString(options, "executable", "godotBuildCommand", &executable, errorMessage) ||
        !readOptionalString(options, "mode", "godotBuildCommand", &mode, errorMessage) ||
        !readOptionalString(options, "scene", "godotBuildCommand", &scene, errorMessage) ||
        !readOptionalBool(options, "headless", "godotBuildCommand", &headless, errorMessage)) {
        return Value::nilValue();
    }

    if (trimAscii(executable).empty()) {
        EditorDiscoveryResult result = detectGodotEditor("");
        if (!result.found) {
            setError(errorMessage,
                     "Could not locate a Godot executable. Set GODOT_EXE or pass 'executable'.");
            return Value::nilValue();
        }
        executable = result.path;
    }

    std::string normalizedMode = lowerAscii(mode);
    if (normalizedMode != "editor" &&
        normalizedMode != "project" &&
        normalizedMode != "run" &&
        normalizedMode != "scene") {
        setError(errorMessage,
                 "godotBuildCommand mode must be 'editor', 'project', 'run' or 'scene'.");
        return Value::nilValue();
    }

    std::string command = quoteShellArg(executable);
    if (headless) {
        command += " --headless";
    }
    if (normalizedMode == "editor") {
        command += " --editor";
    }

    command += " --path " + quoteShellArg(normalizePathString(projectRoot));
    if (normalizedMode == "scene") {
        if (trimAscii(scene).empty()) {
            setError(errorMessage, "godotBuildCommand scene mode expects a 'scene' path.");
            return Value::nilValue();
        }
        command += " " + quoteShellArg(toGodotResourcePath(scene, projectRoot, projectRoot));
    }

    if (!appendExtraArgs(options, &command, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(command);
    }