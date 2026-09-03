#include "stdlib_extensions.h"
#include "heap.h"
#include <algorithm>
#include <cctype>

NativeFunctionPtr g_stringSplitMethod = nullptr;
NativeFunctionPtr g_stringReplaceMethod = nullptr;
NativeFunctionPtr g_stringToUpperMethod = nullptr;
NativeFunctionPtr g_stringToLowerMethod = nullptr;
NativeFunctionPtr g_stringTrimMethod = nullptr;

NativeFunctionPtr g_mapKeysMethod = nullptr;
NativeFunctionPtr g_mapValuesMethod = nullptr;
NativeFunctionPtr g_mapHasMethod = nullptr;
NativeFunctionPtr g_mapRemoveMethod = nullptr;
NativeFunctionPtr g_mapClearMethod = nullptr;

// --- STRING METHODS ---
static Value stringSplitNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    if (!args[0].isString()) {
        if (errorMessage != nullptr) *errorMessage = "Separator must be a string.";
        return Value::nilValue();
    }
    std::string str = receiver.asString();
    std::string sep = args[0].asString();
    ArrayPtr result = uraniumHeap().allocateArray();
    
    if (sep.empty()) {
        for (char c : str) {
            result->elements.push_back(Value::stringValue(std::string(1, c)));
        }
        return Value::arrayValue(result);
    }
    
    size_t start = 0;
    size_t end = str.find(sep);
    while (end != std::string::npos) {
        result->elements.push_back(Value::stringValue(str.substr(start, end - start)));
        start = end + sep.length();
        end = str.find(sep, start);
    }
    result->elements.push_back(Value::stringValue(str.substr(start)));
    
    return Value::arrayValue(result);
}

static Value stringReplaceNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    if (!args[0].isString() || !args[1].isString()) {
        if (errorMessage != nullptr) *errorMessage = "Arguments must be strings.";
        return Value::nilValue();
    }
    std::string str = receiver.asString();
    std::string oldStr = args[0].asString();
    std::string newStr = args[1].asString();
    
    if (oldStr.empty()) return receiver;
    
    size_t pos = 0;
    while ((pos = str.find(oldStr, pos)) != std::string::npos) {
        str.replace(pos, oldStr.length(), newStr);
        pos += newStr.length();
    }
    return Value::stringValue(str);
}

static Value stringToUpperNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    std::string str = receiver.asString();
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return Value::stringValue(str);
}

static Value stringToLowerNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    std::string str = receiver.asString();
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return Value::stringValue(str);
}

static Value stringTrimNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    std::string str = receiver.asString();
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return Value::stringValue("");
    auto end = str.find_last_not_of(" \t\r\n");
    return Value::stringValue(str.substr(start, end - start + 1));
}

// --- MAP METHODS ---
static Value mapKeysNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    MapPtr map = receiver.asMap();
    ArrayPtr result = uraniumHeap().allocateArray();
    if (map != nullptr) {
        for (const auto& pair : map->entries) {
            result->elements.push_back(Value::stringValue(pair.first));
        }
    }
    return Value::arrayValue(result);
}

static Value mapValuesNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    MapPtr map = receiver.asMap();
    ArrayPtr result = uraniumHeap().allocateArray();
    if (map != nullptr) {
        for (const auto& pair : map->entries) {
            result->elements.push_back(pair.second);
        }
    }
    return Value::arrayValue(result);
}

static Value mapHasNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    if (!args[0].isString()) {
        if (errorMessage != nullptr) *errorMessage = "Key must be a string.";
        return Value::nilValue();
    }
    MapPtr map = receiver.asMap();
    if (map == nullptr) return Value::boolValue(false);
    return Value::boolValue(map->entries.find(args[0].asString()) != map->entries.end());
}

static Value mapRemoveNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    if (!args[0].isString()) {
        if (errorMessage != nullptr) *errorMessage = "Key must be a string.";
        return Value::nilValue();
    }
    MapPtr map = receiver.asMap();
    if (map == nullptr) return Value::nilValue();
    
    std::string key = args[0].asString();
    auto it = map->entries.find(key);
    if (it != map->entries.end()) {
        Value removed = it->second;
        map->entries.erase(it);
        return removed;
    }
    return Value::nilValue();
}

static Value mapClearNative(int argCount, const Value* args, std::string* errorMessage) {
    Value receiver = args[-1];
    MapPtr map = receiver.asMap();
    if (map != nullptr) {
        map->entries.clear();
    }
    return receiver;
}

void registerStdlibExtensions() {
    if (g_stringSplitMethod == nullptr) {
        g_stringSplitMethod = uraniumHeap().allocateNativeFunction("split", 1, stringSplitNative);
        g_stringReplaceMethod = uraniumHeap().allocateNativeFunction("replace", 2, stringReplaceNative);
        g_stringToUpperMethod = uraniumHeap().allocateNativeFunction("toUpper", 0, stringToUpperNative);
        g_stringToLowerMethod = uraniumHeap().allocateNativeFunction("toLower", 0, stringToLowerNative);
        g_stringTrimMethod = uraniumHeap().allocateNativeFunction("trim", 0, stringTrimNative);

        g_mapKeysMethod = uraniumHeap().allocateNativeFunction("keys", 0, mapKeysNative);
        g_mapValuesMethod = uraniumHeap().allocateNativeFunction("values", 0, mapValuesNative);
        g_mapHasMethod = uraniumHeap().allocateNativeFunction("has", 1, mapHasNative);
        g_mapRemoveMethod = uraniumHeap().allocateNativeFunction("remove", 1, mapRemoveNative);
        g_mapClearMethod = uraniumHeap().allocateNativeFunction("clear", 0, mapClearNative);
    }
}

bool bindStringMethod(const Value& receiver, const std::string& property, Value* result) {
    if (property == "split") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_stringSplitMethod)); return true; }
    if (property == "replace") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_stringReplaceMethod)); return true; }
    if (property == "toUpper") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_stringToUpperMethod)); return true; }
    if (property == "toLower") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_stringToLowerMethod)); return true; }
    if (property == "trim") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_stringTrimMethod)); return true; }
    return false;
}

bool bindMapMethod(const Value& receiver, const std::string& property, Value* result) {
    if (property == "keys") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_mapKeysMethod)); return true; }
    if (property == "values") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_mapValuesMethod)); return true; }
    if (property == "has") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_mapHasMethod)); return true; }
    if (property == "remove") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_mapRemoveMethod)); return true; }
    if (property == "clear") { *result = Value::boundMethodValue(uraniumHeap().allocateBoundNativeMethod(receiver, g_mapClearMethod)); return true; }
    return false;
}
