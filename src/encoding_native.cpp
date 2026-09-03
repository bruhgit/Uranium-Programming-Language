#include "value.h"
#include "encoding.h"
#include <string>
#include <cctype>

static bool ensureArgCount(int argCount, int expectedCount, std::string* errorMessage) {
    if (argCount != expectedCount) {
        if (errorMessage) {
            *errorMessage = "Expected " + std::to_string(expectedCount) +
                            " arguments but got " + std::to_string(argCount) + ".";
        }
        return false;
    }
    return true;
}

static bool ensureString(const Value& arg, const std::string& funcName, int argIndex, std::string* errorMessage) {
    if (!arg.isString()) {
        if (errorMessage) {
            *errorMessage = "Argument " + std::to_string(argIndex) + " to '" + funcName + "' must be a string.";
        }
        return false;
    }
    return true;
}

Value nativeEncodingDecode(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "encodingDecode", 0, errorMessage) ||
        !ensureString(args[1], "encodingDecode", 1, errorMessage)) {
        return Value::nilValue();
    }
    std::string data = args[0].asString();
    std::string type = args[1].asString();
    
    for (char& c : type) c = std::tolower(c);

    if (type == "utf-8" || type == "utf8") {
        return Value::stringValue(data);
    } else if (type == "utf-16le" || type == "utf16le" || type == "utf-16" || type == "utf16") {
        return Value::stringValue(uranium::encoding::decodeUTF16LE(data));
    } else if (type == "utf-16be" || type == "utf16be") {
        return Value::stringValue(uranium::encoding::decodeUTF16BE(data));
    } else if (type == "utf-32le" || type == "utf32le" || type == "utf-32" || type == "utf32") {
        return Value::stringValue(uranium::encoding::decodeUTF32LE(data));
    } else if (type == "utf-32be" || type == "utf32be") {
        return Value::stringValue(uranium::encoding::decodeUTF32BE(data));
    } else if (type == "iso-8859-1" || type == "iso8859-1" || type == "ascii") {
        return Value::stringValue(uranium::encoding::decodeISO8859_1(data));
    } else {
        if (errorMessage) *errorMessage = "Unknown encoding type: " + type;
        return Value::nilValue();
    }
}

Value nativeEncodingEncode(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "encodingEncode", 0, errorMessage) ||
        !ensureString(args[1], "encodingEncode", 1, errorMessage)) {
        return Value::nilValue();
    }
    std::string data = args[0].asString();
    std::string type = args[1].asString();
    
    for (char& c : type) c = std::tolower(c);

    if (type == "utf-8" || type == "utf8") {
        return Value::stringValue(data);
    } else if (type == "utf-16le" || type == "utf16le" || type == "utf-16" || type == "utf16") {
        return Value::stringValue(uranium::encoding::encodeUTF16LE(data));
    } else if (type == "utf-16be" || type == "utf16be") {
        return Value::stringValue(uranium::encoding::encodeUTF16BE(data));
    } else if (type == "utf-32le" || type == "utf32le" || type == "utf-32" || type == "utf32") {
        return Value::stringValue(uranium::encoding::encodeUTF32LE(data));
    } else if (type == "utf-32be" || type == "utf32be") {
        return Value::stringValue(uranium::encoding::encodeUTF32BE(data));
    } else if (type == "iso-8859-1" || type == "iso8859-1" || type == "ascii") {
        return Value::stringValue(uranium::encoding::encodeISO8859_1(data));
    } else {
        if (errorMessage) *errorMessage = "Unknown encoding type: " + type;
        return Value::nilValue();
    }
}
