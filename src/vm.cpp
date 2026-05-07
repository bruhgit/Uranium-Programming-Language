#include "vm.h"
#include "compiler.h"
#include "godot_native.h"
#include "http_native.h"
#include "gui_native.h"
#include "heap.h"
#include "native_jit.h"
#include "object.h"
#include "optimizer.h"
#include "system_native.h"
#include "net_native.h"
#include "crypto_native.h"
#include "db_native.h"
#include "thread_native.h"
#include "type_system.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <regex>
#include <thread>

namespace {

VM* activeVm = nullptr;

long long currentUnixMillis() {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return static_cast<long long>(millis.count());
}

const char* taskStateName(TaskState state) {
    switch (state) {
        case TASK_READY:
            return "ready";
        case TASK_RUNNING:
            return "running";
        case TASK_WAITING:
            return "waiting";
        case TASK_SLEEPING:
            return "sleeping";
        case TASK_COMPLETED:
            return "completed";
        case TASK_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

constexpr uint32_t kFastPathHotThreshold = 32;
constexpr long long kLoopBrokerInstructionLimit = 8000000;
constexpr long long kLoopBrokerLoopLimit = 500000;
constexpr long long kLoopBrokerWallClockLimitMillis = 4000;

FunctionPtr functionForJitValue(const Value& value) {
    if (value.isClosure() && value.asClosure() != nullptr) {
        return value.asClosure()->function;
    }

    if (value.isFunction()) {
        return value.asFunction();
    }

    if (value.isBoundMethod() &&
        value.asBoundMethod() != nullptr &&
        value.asBoundMethod()->method != nullptr) {
        return value.asBoundMethod()->method->function;
    }

    return nullptr;
}

const char* fastPathStatusName(FastPathStatus status) {
    switch (status) {
        case FASTPATH_COMPILED:
            return "compiled";
        case FASTPATH_UNSUPPORTED:
            return "unsupported";
        case FASTPATH_UNCHECKED:
        default:
            return "pending";
    }
}

const char* jitBackendName(JitBackendKind kind) {
    switch (kind) {
        case JIT_BACKEND_NATIVE:
            return "native";
        case JIT_BACKEND_FASTPATH:
            return "fastpath";
        case JIT_BACKEND_NONE:
        default:
            return "none";
    }
}

void writeBarrier(HeapObject* owner, const Value& value) {
    uraniumHeap().writeBarrier(owner, value);
}

bool ensureArgCount(int argCount, int expectedCount, std::string* errorMessage) {
    if (argCount == expectedCount) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Expected " + std::to_string(expectedCount) +
                        " argument(s) but got " + std::to_string(argCount) + ".";
    }
    return false;
}

bool ensureNumber(const Value& value, const std::string& functionName,
                  int index, std::string* errorMessage) {
    if (value.isNumber()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a number.";
    }
    return false;
}

bool ensureString(const Value& value, const std::string& functionName,
                  int index, std::string* errorMessage) {
    if (value.isString()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a string.";
    }
    return false;
}

bool ensureArray(const Value& value, const std::string& functionName,
                 int index, std::string* errorMessage) {
    if (value.isArray()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                        " to be an array.";
    }
    return false;
}

bool ensureMap(const Value& value, const std::string& functionName,
               int index, std::string* errorMessage) {
    if (value.isMap()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a map.";
    }
    return false;
}

bool ensureBool(const Value& value, const std::string& functionName,
                int index, std::string* errorMessage) {
    if (value.isBool()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a boolean.";
    }
    return false;
}

bool ensureWholeNumber(const Value& value, const std::string& functionName,
                       int index, std::string* errorMessage) {
    if (!ensureNumber(value, functionName, index, errorMessage)) {
        return false;
    }

    double numericValue = value.asNumber();
    if (!std::isfinite(numericValue) || std::trunc(numericValue) != numericValue) {
        if (errorMessage != nullptr) {
            *errorMessage = functionName + " expects argument " + std::to_string(index + 1) +
                            " to be a whole number.";
        }
        return false;
    }

    return true;
}

long long asWholeNumber(const Value& value) {
    return static_cast<long long>(value.asNumber());
}

int asInt(const Value& value) {
    return static_cast<int>(asWholeNumber(value));
}

std::mt19937_64& randomEngine() {
    static std::mt19937_64 engine(
        static_cast<std::mt19937_64::result_type>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    return engine;
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

std::size_t normalizeSliceStart(std::size_t size, long long start) {
    long long normalized = start;
    if (normalized < 0) {
        normalized += static_cast<long long>(size);
    }

    if (normalized < 0) {
        return 0;
    }

    if (normalized > static_cast<long long>(size)) {
        return size;
    }

    return static_cast<std::size_t>(normalized);
}

const char* kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64EncodeBytes(const std::string& input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    std::size_t index = 0;
    while (index < input.size()) {
        uint32_t octetA = static_cast<unsigned char>(input[index++]);
        bool hasB = index < input.size();
        uint32_t octetB = hasB ? static_cast<unsigned char>(input[index++]) : 0;
        bool hasC = index < input.size();
        uint32_t octetC = hasC ? static_cast<unsigned char>(input[index++]) : 0;

        uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;
        output.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
        output.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
        output.push_back(hasB ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
        output.push_back(hasC ? kBase64Alphabet[triple & 0x3F] : '=');
    }

    return output;
}

bool decodeBase64Char(char c, uint8_t* value) {
    if (c >= 'A' && c <= 'Z') {
        *value = static_cast<uint8_t>(c - 'A');
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        *value = static_cast<uint8_t>(c - 'a' + 26);
        return true;
    }
    if (c >= '0' && c <= '9') {
        *value = static_cast<uint8_t>(c - '0' + 52);
        return true;
    }
    if (c == '+') {
        *value = 62;
        return true;
    }
    if (c == '/') {
        *value = 63;
        return true;
    }
    return false;
}

bool base64DecodeBytes(const std::string& input,
                       std::string* output,
                       std::string* errorMessage) {
    std::string compact;
    compact.reserve(input.size());
    for (char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            compact.push_back(c);
        }
    }

    if (compact.empty()) {
        if (output != nullptr) {
            output->clear();
        }
        return true;
    }

    if (compact.size() % 4 != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "base64Decode expects input length to be a multiple of 4.";
        }
        return false;
    }

    std::string decoded;
    decoded.reserve((compact.size() / 4) * 3);

    for (std::size_t index = 0; index < compact.size(); index += 4) {
        uint8_t values[4] = {0, 0, 0, 0};
        int padding = 0;

        for (int offset = 0; offset < 4; ++offset) {
            char c = compact[index + static_cast<std::size_t>(offset)];
            if (c == '=') {
                values[offset] = 0;
                padding++;
                continue;
            }

            if (!decodeBase64Char(c, &values[offset])) {
                if (errorMessage != nullptr) {
                    *errorMessage = "base64Decode found an invalid character.";
                }
                return false;
            }
        }

        uint32_t triple =
            (static_cast<uint32_t>(values[0]) << 18) |
            (static_cast<uint32_t>(values[1]) << 12) |
            (static_cast<uint32_t>(values[2]) << 6) |
            static_cast<uint32_t>(values[3]);

        decoded.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) {
            decoded.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            decoded.push_back(static_cast<char>(triple & 0xFF));
        }
    }

    if (output != nullptr) {
        *output = std::move(decoded);
    }
    return true;
}

bool valueToMapKey(const Value& value, const std::string& context,
                   std::string* key, std::string* errorMessage) {
    if (!value.isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = context + " expects a string key.";
        }
        return false;
    }

    if (key != nullptr) {
        *key = value.asString();
    }
    return true;
}

bool valueToArrayIndex(const Value& value, std::size_t size, bool allowEnd,
                       const std::string& context, std::size_t* index,
                       std::string* errorMessage) {
    if (!value.isNumber()) {
        if (errorMessage != nullptr) {
            *errorMessage = context + " expects a numeric index.";
        }
        return false;
    }

    double numericIndex = value.asNumber();
    if (!std::isfinite(numericIndex) || std::trunc(numericIndex) != numericIndex) {
        if (errorMessage != nullptr) {
            *errorMessage = context + " expects a whole-number index.";
        }
        return false;
    }

    if (numericIndex < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = context + " expects a non-negative index.";
        }
        return false;
    }

    std::size_t converted = static_cast<std::size_t>(numericIndex);
    if (converted > size || (!allowEnd && converted >= size)) {
        if (errorMessage != nullptr) {
            *errorMessage = context + " index " + std::to_string(converted) +
                            " is out of range.";
        }
        return false;
    }

    if (index != nullptr) {
        *index = converted;
    }
    return true;
}

Value nativeAbs(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "abs", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::fabs(args[0].asNumber()));
}

Value nativeSqrt(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "sqrt", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (args[0].asNumber() < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "sqrt expects a non-negative number.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::sqrt(args[0].asNumber()));
}

Value nativePow(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "pow", 0, errorMessage) ||
        !ensureNumber(args[1], "pow", 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::pow(args[0].asNumber(), args[1].asNumber()));
}

Value nativeMin(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "min", 0, errorMessage) ||
        !ensureNumber(args[1], "min", 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::min(args[0].asNumber(), args[1].asNumber()));
}

Value nativeMax(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "max", 0, errorMessage) ||
        !ensureNumber(args[1], "max", 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::max(args[0].asNumber(), args[1].asNumber()));
}

Value nativeClamp(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureNumber(args[0], "clamp", 0, errorMessage) ||
        !ensureNumber(args[1], "clamp", 1, errorMessage) ||
        !ensureNumber(args[2], "clamp", 2, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(
        std::clamp(args[0].asNumber(), args[1].asNumber(), args[2].asNumber()));
}

Value nativeFloor(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "floor", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::floor(args[0].asNumber()));
}

Value nativeCeil(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "ceil", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::ceil(args[0].asNumber()));
}

Value nativeRound(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "round", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::round(args[0].asNumber()));
}

Value nativeSin(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "sin", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::sin(args[0].asNumber()));
}

Value nativeCos(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "cos", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::cos(args[0].asNumber()));
}

Value nativeTan(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "tan", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::tan(args[0].asNumber()));
}

Value nativeAsin(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "asin", 0, errorMessage)) {
        return Value::nilValue();
    }

    double value = args[0].asNumber();
    if (value < -1.0 || value > 1.0) {
        if (errorMessage != nullptr) {
            *errorMessage = "asin expects a number between -1 and 1.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::asin(value));
}

Value nativeAcos(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "acos", 0, errorMessage)) {
        return Value::nilValue();
    }

    double value = args[0].asNumber();
    if (value < -1.0 || value > 1.0) {
        if (errorMessage != nullptr) {
            *errorMessage = "acos expects a number between -1 and 1.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::acos(value));
}

Value nativeAtan(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "atan", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::atan(args[0].asNumber()));
}

Value nativeAtan2(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "atan2", 0, errorMessage) ||
        !ensureNumber(args[1], "atan2", 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::atan2(args[0].asNumber(), args[1].asNumber()));
}

Value nativeExp(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "exp", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(std::exp(args[0].asNumber()));
}

Value nativeLog(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "log", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (args[0].asNumber() <= 0.0) {
        if (errorMessage != nullptr) {
            *errorMessage = "log expects a positive number.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::log(args[0].asNumber()));
}

Value nativeLog10(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "log10", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (args[0].asNumber() <= 0.0) {
        if (errorMessage != nullptr) {
            *errorMessage = "log10 expects a positive number.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::log10(args[0].asNumber()));
}

Value nativeMod(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "mod", 0, errorMessage) ||
        !ensureNumber(args[1], "mod", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (args[1].asNumber() == 0.0) {
        if (errorMessage != nullptr) {
            *errorMessage = "mod expects a non-zero divisor.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(std::fmod(args[0].asNumber(), args[1].asNumber()));
}

Value nativeSign(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "sign", 0, errorMessage)) {
        return Value::nilValue();
    }

    double value = args[0].asNumber();
    if (value > 0.0) {
        return Value::numberValue(1.0);
    }

    if (value < 0.0) {
        return Value::numberValue(-1.0);
    }

    return Value::numberValue(0.0);
}

Value nativeRadians(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "radians", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(args[0].asNumber() * 3.14159265358979323846 / 180.0);
}

Value nativeDegrees(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "degrees", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(args[0].asNumber() * 180.0 / 3.14159265358979323846);
}

Value nativeRandom(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return Value::numberValue(distribution(randomEngine()));
}

Value nativeRandRange(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureNumber(args[0], "randRange", 0, errorMessage) ||
        !ensureNumber(args[1], "randRange", 1, errorMessage)) {
        return Value::nilValue();
    }

    double low = args[0].asNumber();
    double high = args[1].asNumber();
    if (high < low) {
        if (errorMessage != nullptr) {
            *errorMessage = "randRange expects the first argument to be <= the second.";
        }
        return Value::nilValue();
    }

    if (low == high) {
        return Value::numberValue(low);
    }

    std::uniform_real_distribution<double> distribution(low, high);
    return Value::numberValue(distribution(randomEngine()));
}

Value nativeRandInt(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "randInt", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "randInt", 1, errorMessage)) {
        return Value::nilValue();
    }

    long long low = asWholeNumber(args[0]);
    long long high = asWholeNumber(args[1]);
    if (high < low) {
        if (errorMessage != nullptr) {
            *errorMessage = "randInt expects the first argument to be <= the second.";
        }
        return Value::nilValue();
    }

    std::uniform_int_distribution<long long> distribution(low, high);
    return Value::numberValue(static_cast<double>(distribution(randomEngine())));
}

Value nativeSeedRandom(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "seedRandom", 0, errorMessage)) {
        return Value::nilValue();
    }

    randomEngine().seed(static_cast<std::mt19937_64::result_type>(asWholeNumber(args[0])));
    return Value::nilValue();
}

Value nativeLen(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (args[0].isString()) {
        return Value::numberValue(static_cast<double>(args[0].asString().size()));
    }

    if (args[0].isArray()) {
        const ArrayPtr& array = args[0].asArray();
        return Value::numberValue(
            static_cast<double>(array == nullptr ? 0 : array->elements.size()));
    }

    if (args[0].isMap()) {
        const MapPtr& map = args[0].asMap();
        return Value::numberValue(
            static_cast<double>(map == nullptr ? 0 : map->entries.size()));
    }

    if (errorMessage != nullptr) {
        *errorMessage = "len expects a string, array, or map.";
    }
    return Value::nilValue();
}

Value nativeUpper(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "upper", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string result = args[0].asString();
    for (char& character : result) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return Value::stringValue(result);
}

Value nativeLower(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "lower", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string result = args[0].asString();
    for (char& character : result) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return Value::stringValue(result);
}

Value nativeTrim(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "trim", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(trimAscii(args[0].asString()));
}

Value nativeRepeat(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "repeat", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "repeat", 1, errorMessage)) {
        return Value::nilValue();
    }

    long long count = asWholeNumber(args[1]);
    if (count < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "repeat expects a non-negative count.";
        }
        return Value::nilValue();
    }

    const std::string& source = args[0].asString();
    std::string result;
    result.reserve(source.size() * static_cast<std::size_t>(count));

    for (long long i = 0; i < count; ++i) {
        result += source;
    }

    return Value::stringValue(result);
}

Value nativeContains(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "contains", 0, errorMessage) ||
        !ensureString(args[1], "contains", 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(
        args[0].asString().find(args[1].asString()) != std::string::npos);
}

Value nativeStartsWith(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "startsWith", 0, errorMessage) ||
        !ensureString(args[1], "startsWith", 1, errorMessage)) {
        return Value::nilValue();
    }

    const std::string& text = args[0].asString();
    const std::string& prefix = args[1].asString();
    bool matches = text.size() >= prefix.size() &&
                   text.compare(0, prefix.size(), prefix) == 0;
    return Value::boolValue(matches);
}

Value nativeEndsWith(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "endsWith", 0, errorMessage) ||
        !ensureString(args[1], "endsWith", 1, errorMessage)) {
        return Value::nilValue();
    }

    const std::string& text = args[0].asString();
    const std::string& suffix = args[1].asString();
    bool matches = text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    return Value::boolValue(matches);
}

Value nativeLeft(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "left", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "left", 1, errorMessage)) {
        return Value::nilValue();
    }

    long long count = asWholeNumber(args[1]);
    if (count < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "left expects a non-negative count.";
        }
        return Value::nilValue();
    }

    const std::string& text = args[0].asString();
    std::size_t boundedCount = std::min<std::size_t>(text.size(), static_cast<std::size_t>(count));
    return Value::stringValue(text.substr(0, boundedCount));
}

Value nativeRight(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "right", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "right", 1, errorMessage)) {
        return Value::nilValue();
    }

    long long count = asWholeNumber(args[1]);
    if (count < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "right expects a non-negative count.";
        }
        return Value::nilValue();
    }

    const std::string& text = args[0].asString();
    std::size_t boundedCount = std::min<std::size_t>(text.size(), static_cast<std::size_t>(count));
    return Value::stringValue(text.substr(text.size() - boundedCount));
}

Value nativeSlice(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureString(args[0], "slice", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "slice", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "slice", 2, errorMessage)) {
        return Value::nilValue();
    }

    long long start = asWholeNumber(args[1]);
    long long count = asWholeNumber(args[2]);
    if (count < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "slice expects a non-negative length.";
        }
        return Value::nilValue();
    }

    const std::string& text = args[0].asString();
    std::size_t begin = normalizeSliceStart(text.size(), start);
    std::size_t remaining = text.size() - begin;
    std::size_t boundedCount = std::min<std::size_t>(remaining, static_cast<std::size_t>(count));
    return Value::stringValue(text.substr(begin, boundedCount));
}

Value nativeReplace(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureString(args[0], "replace", 0, errorMessage) ||
        !ensureString(args[1], "replace", 1, errorMessage) ||
        !ensureString(args[2], "replace", 2, errorMessage)) {
        return Value::nilValue();
    }

    const std::string& target = args[1].asString();
    if (target.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "replace expects a non-empty search string.";
        }
        return Value::nilValue();
    }

    std::string result = args[0].asString();
    const std::string& replacement = args[2].asString();
    std::size_t position = 0;

    while ((position = result.find(target, position)) != std::string::npos) {
        result.replace(position, target.size(), replacement);
        position += replacement.size();
    }

    return Value::stringValue(result);
}

Value nativeToNumber(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "toNumber", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string trimmed = trimAscii(args[0].asString());
    if (trimmed.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "toNumber expects a non-empty numeric string.";
        }
        return Value::nilValue();
    }

    char* end = nullptr;
    const char* raw = trimmed.c_str();
    double result = std::strtod(raw, &end);

    if (end == raw || end == nullptr || *end != '\0') {
        if (errorMessage != nullptr) {
            *errorMessage = "toNumber could not parse '" + trimmed + "'.";
        }
        return Value::nilValue();
    }

    return Value::numberValue(result);
}

Value nativeRegexMatch(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "regexMatch", 0, errorMessage) ||
        !ensureString(args[1], "regexMatch", 1, errorMessage)) {
        return Value::nilValue();
    }

    try {
        std::regex pattern(args[0].asString());
        return Value::boolValue(std::regex_match(args[1].asString(), pattern));
    } catch (const std::regex_error&) {
        if (errorMessage != nullptr) {
            *errorMessage = "regexMatch received an invalid regex pattern.";
        }
        return Value::nilValue();
    }
}

Value nativeRegexSearch(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "regexSearch", 0, errorMessage) ||
        !ensureString(args[1], "regexSearch", 1, errorMessage)) {
        return Value::nilValue();
    }

    try {
        std::regex pattern(args[0].asString());
        std::smatch matchResult;
        if (!std::regex_search(args[1].asString(), matchResult, pattern) ||
            matchResult.empty()) {
            return Value::nilValue();
        }

        return Value::stringValue(matchResult.str(0));
    } catch (const std::regex_error&) {
        if (errorMessage != nullptr) {
            *errorMessage = "regexSearch received an invalid regex pattern.";
        }
        return Value::nilValue();
    }
}

Value nativeRegexFindAll(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "regexFindAll", 0, errorMessage) ||
        !ensureString(args[1], "regexFindAll", 1, errorMessage)) {
        return Value::nilValue();
    }

    try {
        std::regex pattern(args[0].asString());
        ArrayPtr matches = uraniumHeap().allocateArray();
        const std::string& text = args[1].asString();

        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end;
             it != end;
             ++it) {
            matches->elements.push_back(Value::stringValue((*it).str(0)));
        }

        return Value::arrayValue(matches);
    } catch (const std::regex_error&) {
        if (errorMessage != nullptr) {
            *errorMessage = "regexFindAll received an invalid regex pattern.";
        }
        return Value::nilValue();
    }
}

Value nativeRegexReplace(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureString(args[0], "regexReplace", 0, errorMessage) ||
        !ensureString(args[1], "regexReplace", 1, errorMessage) ||
        !ensureString(args[2], "regexReplace", 2, errorMessage)) {
        return Value::nilValue();
    }

    try {
        std::regex pattern(args[0].asString());
        return Value::stringValue(
            std::regex_replace(args[1].asString(), pattern, args[2].asString()));
    } catch (const std::regex_error&) {
        if (errorMessage != nullptr) {
            *errorMessage = "regexReplace received an invalid regex pattern.";
        }
        return Value::nilValue();
    }
}

Value nativeRegexSplit(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "regexSplit", 0, errorMessage) ||
        !ensureString(args[1], "regexSplit", 1, errorMessage)) {
        return Value::nilValue();
    }

    try {
        std::regex pattern(args[0].asString());
        ArrayPtr parts = uraniumHeap().allocateArray();
        const std::string& text = args[1].asString();

        for (std::sregex_token_iterator it(text.begin(), text.end(), pattern, -1), end;
             it != end;
             ++it) {
            parts->elements.push_back(Value::stringValue(it->str()));
        }

        return Value::arrayValue(parts);
    } catch (const std::regex_error&) {
        if (errorMessage != nullptr) {
            *errorMessage = "regexSplit received an invalid regex pattern.";
        }
        return Value::nilValue();
    }
}

Value nativeBase64Encode(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "base64Encode", 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(base64EncodeBytes(args[0].asString()));
}

Value nativeBase64Decode(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "base64Decode", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string decoded;
    if (!base64DecodeBytes(args[0].asString(), &decoded, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(decoded);
}

Value nativeStr(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(valueToString(args[0]));
}

Value nativeTypeOf(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    switch (args[0].type) {
        case VAL_NIL:
            return Value::stringValue("nil");
        case VAL_BOOL:
            return Value::stringValue("bool");
        case VAL_NUMBER:
            return Value::stringValue("number");
        case VAL_STRING:
            return Value::stringValue("string");
        case VAL_FUNCTION:
        case VAL_CLOSURE:
            return Value::stringValue("function");
        case VAL_NATIVE_FUNCTION:
            return Value::stringValue("native_function");
        case VAL_ARRAY:
            return Value::stringValue("array");
        case VAL_MAP:
            return Value::stringValue("map");
        case VAL_CLASS:
            return Value::stringValue("class");
        case VAL_INSTANCE:
            return Value::stringValue("instance");
        case VAL_BOUND_METHOD:
            return Value::stringValue("bound_method");
        case VAL_TASK:
            return Value::stringValue("task");
        default:
            return Value::stringValue("unknown");
    }
}

Value nativeIsNil(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isNil());
}

Value nativeIsBool(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isBool());
}

Value nativeIsNumber(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isNumber());
}

Value nativeIsString(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isString());
}

Value nativeIsFunction(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isFunction() || args[0].isClosure());
}

Value nativeIsNativeFunction(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isNativeFunction());
}

Value nativeIsArray(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isArray());
}

Value nativeIsMap(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isMap());
}

Value nativeIsClass(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isClass());
}

Value nativeIsInstance(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isInstance());
}

Value nativeIsTask(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isTask());
}

Value nativeTaskStatus(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!args[0].isTask() || args[0].asTask() == nullptr) {
        return Value::stringValue("invalid");
    }

    return Value::stringValue(taskStateName(args[0].asTask()->state));
}

Value nativeTaskDone(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!args[0].isTask() || args[0].asTask() == nullptr) {
        return Value::boolValue(false);
    }

    TaskState state = args[0].asTask()->state;
    return Value::boolValue(state == TASK_COMPLETED || state == TASK_FAILED);
}

Value nativeTaskFailed(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(args[0].isTask() &&
                            args[0].asTask() != nullptr &&
                            args[0].asTask()->state == TASK_FAILED);
}

Value nativeTaskResult(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!args[0].isTask() || args[0].asTask() == nullptr ||
        args[0].asTask()->state != TASK_COMPLETED) {
        return Value::nilValue();
    }

    args[0].asTask()->observed = true;
    return args[0].asTask()->result;
}

Value nativeTaskError(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!args[0].isTask() || args[0].asTask() == nullptr ||
        args[0].asTask()->state != TASK_FAILED) {
        return Value::nilValue();
    }

    args[0].asTask()->observed = true;
    return args[0].asTask()->failure;
}

Value nativeSleepAsync(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "sleepAsync", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (activeVm == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "sleepAsync is not available outside a running VM.";
        }
        return Value::nilValue();
    }

    long long milliseconds = asWholeNumber(args[0]);
    if (milliseconds < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "sleepAsync expects a non-negative duration.";
        }
        return Value::nilValue();
    }

    TaskPtr task = activeVm->createTimerTask(
        currentUnixMillis() + milliseconds, Value::nilValue(), "sleep");
    return Value::taskValue(task);
}

Value nativeYieldAsync(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    if (activeVm == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "yieldAsync is not available outside a running VM.";
        }
        return Value::nilValue();
    }

    TaskPtr task = activeVm->createTimerTask(currentUnixMillis(), Value::nilValue(), "yield");
    return Value::taskValue(task);
}

Value nativeJitCompile(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    if (activeVm == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "jitCompile is not available outside a running VM.";
        }
        return Value::nilValue();
    }

    FunctionPtr function = functionForJitValue(args[0]);
    if (function == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "jitCompile expects a function, closure, or bound method.";
        }
        return Value::nilValue();
    }

    return Value::boolValue(activeVm->jitCompile(function));
}

Value nativeJitStatus(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    FunctionPtr function = functionForJitValue(args[0]);
    if (function == nullptr) {
        return Value::stringValue("invalid");
    }

    return Value::stringValue(fastPathStatusName(function->fastPathStatus));
}

Value nativeJitBackend(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    FunctionPtr function = functionForJitValue(args[0]);
    if (function == nullptr) {
        return Value::stringValue("invalid");
    }

    return Value::stringValue(jitBackendName(function->jitBackend));
}

Value nativeJitStats(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    if (activeVm == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "jitStats is not available outside a running VM.";
        }
        return Value::nilValue();
    }

    MapPtr stats = uraniumHeap().allocateMap();
    stats->entries["compiled"] = Value::numberValue(activeVm->compiledFastPathCount());
    stats->entries["unsupported"] = Value::numberValue(activeVm->unsupportedFastPathCount());
    stats->entries["nativeCompiled"] =
        Value::numberValue(activeVm->nativeCompiledCount());
    stats->entries["fastPathCompiled"] =
        Value::numberValue(activeVm->fastPathCompiledOnlyCount());
    stats->entries["hotThreshold"] = Value::numberValue(kFastPathHotThreshold);
    return Value::mapValue(stats);
}

Value nativeGcCollect(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    if (activeVm == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "gcCollect is not available outside a running VM.";
        }
        return Value::nilValue();
    }

    activeVm->forceGarbageCollection();
    return Value::nilValue();
}

Value nativeGcStats(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    MapPtr stats = uraniumHeap().allocateMap();
    stats->entries["objects"] =
        Value::numberValue(static_cast<double>(uraniumHeap().objectCount()));
    stats->entries["youngObjects"] =
        Value::numberValue(static_cast<double>(uraniumHeap().youngObjectCount()));
    stats->entries["oldObjects"] =
        Value::numberValue(static_cast<double>(uraniumHeap().oldObjectCount()));
    stats->entries["rememberedObjects"] =
        Value::numberValue(static_cast<double>(uraniumHeap().rememberedObjectCount()));
    stats->entries["collections"] =
        Value::numberValue(static_cast<double>(uraniumHeap().collectionCount()));
    stats->entries["minorCollections"] =
        Value::numberValue(static_cast<double>(uraniumHeap().minorCollectionCount()));
    stats->entries["fullCollections"] =
        Value::numberValue(static_cast<double>(uraniumHeap().fullCollectionCount()));
    stats->entries["bytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().allocatedBytes()));
    stats->entries["youngBytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().youngAllocatedBytes()));
    stats->entries["oldBytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().oldAllocatedBytes()));
    stats->entries["thresholdBytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().nextCollectionThreshold()));
    stats->entries["youngThresholdBytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().nextYoungCollectionThreshold()));
    stats->entries["fullThresholdBytes"] =
        Value::numberValue(static_cast<double>(uraniumHeap().nextFullCollectionThreshold()));
    stats->entries["lastMode"] =
        Value::stringValue(uraniumHeap().lastCollectionMode() == HEAP_COLLECT_FULL
                               ? "full"
                               : "young");
    return Value::mapValue(stats);
}

Value nativeArray(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::arrayValue(uraniumHeap().allocateArray());
}

Value nativeMap(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::mapValue(uraniumHeap().allocateMap());
}

Value nativePush(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureArray(args[0], "push", 0, errorMessage)) {
        return Value::nilValue();
    }

    const ArrayPtr& array = args[0].asArray();
    if (array == nullptr) {
        return Value::nilValue();
    }

    array->elements.push_back(args[1]);
    writeBarrier(array, args[1]);
    return Value::numberValue(static_cast<double>(array->elements.size()));
}

Value nativePop(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureArray(args[0], "pop", 0, errorMessage)) {
        return Value::nilValue();
    }

    const ArrayPtr& array = args[0].asArray();
    if (array == nullptr || array->elements.empty()) {
        return Value::nilValue();
    }

    Value result = array->elements.back();
    array->elements.pop_back();
    return result;
}

Value nativeHasKey(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureMap(args[0], "hasKey", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string key;
    if (!valueToMapKey(args[1], "hasKey", &key, errorMessage)) {
        return Value::nilValue();
    }

    const MapPtr& map = args[0].asMap();
    if (map == nullptr) {
        return Value::boolValue(false);
    }

    return Value::boolValue(map->entries.find(key) != map->entries.end());
}

Value nativeDeleteKey(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureMap(args[0], "deleteKey", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string key;
    if (!valueToMapKey(args[1], "deleteKey", &key, errorMessage)) {
        return Value::nilValue();
    }

    const MapPtr& map = args[0].asMap();
    if (map == nullptr) {
        return Value::boolValue(false);
    }

    return Value::boolValue(map->entries.erase(key) > 0);
}

Value nativeKeys(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureMap(args[0], "keys", 0, errorMessage)) {
        return Value::nilValue();
    }

    ArrayPtr result = uraniumHeap().allocateArray();
    const MapPtr& map = args[0].asMap();
    if (map == nullptr) {
        return Value::arrayValue(result);
    }

    std::vector<std::string> keys;
    keys.reserve(map->entries.size());
    for (const auto& entry : map->entries) {
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());

    for (const std::string& key : keys) {
        Value keyValue = Value::stringValue(key);
        result->elements.push_back(keyValue);
        writeBarrier(result, keyValue);
    }

    return Value::arrayValue(result);
}

Value nativeClock(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(std::time(nullptr)));
}

Value nativeUnixMillis(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return Value::numberValue(static_cast<double>(millis));
}

Value nativePrintln(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage)) {
        return Value::nilValue();
    }

    printValue(args[0]);
    std::cout << std::endl;
    return Value::nilValue();
}

Value nativeGuiInfo(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiInfo", 0, errorMessage) ||
        !ensureString(args[1], "guiInfo", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiShowMessage(args[0].asString(), args[1].asString(), GuiMessageKind::Info, errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiWarn(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiWarn", 0, errorMessage) ||
        !ensureString(args[1], "guiWarn", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiShowMessage(args[0].asString(), args[1].asString(), GuiMessageKind::Warning, errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiError(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiError", 0, errorMessage) ||
        !ensureString(args[1], "guiError", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiShowMessage(args[0].asString(), args[1].asString(), GuiMessageKind::Error, errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiConfirm(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiConfirm", 0, errorMessage) ||
        !ensureString(args[1], "guiConfirm", 1, errorMessage)) {
        return Value::nilValue();
    }

    bool confirmed = false;
    if (!guiAskYesNo(args[0].asString(), args[1].asString(), GuiMessageKind::Info,
                     &confirmed, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(confirmed);
}

Value nativeGuiConfirmCancel(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiConfirmCancel", 0, errorMessage) ||
        !ensureString(args[1], "guiConfirmCancel", 1, errorMessage)) {
        return Value::nilValue();
    }

    bool confirmed = false;
    if (!guiAskOkCancel(args[0].asString(), args[1].asString(), GuiMessageKind::Info,
                        &confirmed, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(confirmed);
}

Value nativeGuiPrompt(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureString(args[0], "guiPrompt", 0, errorMessage) ||
        !ensureString(args[1], "guiPrompt", 1, errorMessage) ||
        !ensureString(args[2], "guiPrompt", 2, errorMessage)) {
        return Value::nilValue();
    }

    std::string result;
    bool accepted = false;
    if (!guiPromptText(args[0].asString(), args[1].asString(), args[2].asString(),
                       &result, &accepted, errorMessage)) {
        return Value::nilValue();
    }

    if (!accepted) {
        return Value::nilValue();
    }

    return Value::stringValue(result);
}

Value nativeGuiOpenFile(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "guiOpenFile", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string result;
    bool accepted = false;
    if (!guiOpenFileDialog(args[0].asString(), &result, &accepted, errorMessage)) {
        return Value::nilValue();
    }

    if (!accepted) {
        return Value::nilValue();
    }

    return Value::stringValue(result);
}

Value nativeGuiSaveFile(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "guiSaveFile", 0, errorMessage) ||
        !ensureString(args[1], "guiSaveFile", 1, errorMessage)) {
        return Value::nilValue();
    }

    std::string result;
    bool accepted = false;
    if (!guiSaveFileDialog(args[0].asString(), args[1].asString(),
                           &result, &accepted, errorMessage)) {
        return Value::nilValue();
    }

    if (!accepted) {
        return Value::nilValue();
    }

    return Value::stringValue(result);
}

Value nativeGuiPickFolder(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "guiPickFolder", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string result;
    bool accepted = false;
    if (!guiPickFolderDialog(args[0].asString(), &result, &accepted, errorMessage)) {
        return Value::nilValue();
    }

    if (!accepted) {
        return Value::nilValue();
    }

    return Value::stringValue(result);
}

Value nativeGuiScreenWidth(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    int width = 0;
    if (!guiGetScreenWidth(&width, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(width));
}

Value nativeGuiScreenHeight(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    int height = 0;
    if (!guiGetScreenHeight(&height, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(height));
}

Value nativeGuiCreateWindow(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureString(args[0], "guiCreateWindow", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiCreateWindow", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiCreateWindow", 2, errorMessage)) {
        return Value::nilValue();
    }

    int windowId = 0;
    if (!guiCreateWindow(args[0].asString(), asInt(args[1]), asInt(args[2]),
                         &windowId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(windowId));
}

Value nativeGuiShowWindowEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiShowWindow", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiShowWindow(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiHideWindowEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiHideWindow", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiHideWindow(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiCloseWindowEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiCloseWindow", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiCloseWindow(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiCenterWindowEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiCenterWindow", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiCenterWindow(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiSetWindowTitleEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetWindowTitle", 0, errorMessage) ||
        !ensureString(args[1], "guiSetWindowTitle", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetWindowTitle(asInt(args[0]), args[1].asString(), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiSetWindowSizeEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetWindowSize", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiSetWindowSize", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiSetWindowSize", 2, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetWindowSize(asInt(args[0]), asInt(args[1]), asInt(args[2]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiAddLabelEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 6, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddLabel", 0, errorMessage) ||
        !ensureString(args[1], "guiAddLabel", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddLabel", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddLabel", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddLabel", 4, errorMessage) ||
        !ensureWholeNumber(args[5], "guiAddLabel", 5, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddLabel(asInt(args[0]), args[1].asString(),
                     asInt(args[2]), asInt(args[3]), asInt(args[4]), asInt(args[5]),
                     &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddButtonEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 6, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddButton", 0, errorMessage) ||
        !ensureString(args[1], "guiAddButton", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddButton", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddButton", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddButton", 4, errorMessage) ||
        !ensureWholeNumber(args[5], "guiAddButton", 5, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddButton(asInt(args[0]), args[1].asString(),
                      asInt(args[2]), asInt(args[3]), asInt(args[4]), asInt(args[5]),
                      &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddInputEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 6, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddInput", 0, errorMessage) ||
        !ensureString(args[1], "guiAddInput", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddInput", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddInput", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddInput", 4, errorMessage) ||
        !ensureWholeNumber(args[5], "guiAddInput", 5, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddInput(asInt(args[0]), args[1].asString(),
                     asInt(args[2]), asInt(args[3]), asInt(args[4]), asInt(args[5]),
                     &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddTextAreaEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 6, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddTextArea", 0, errorMessage) ||
        !ensureString(args[1], "guiAddTextArea", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddTextArea", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddTextArea", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddTextArea", 4, errorMessage) ||
        !ensureWholeNumber(args[5], "guiAddTextArea", 5, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddTextArea(asInt(args[0]), args[1].asString(),
                        asInt(args[2]), asInt(args[3]), asInt(args[4]), asInt(args[5]),
                        &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddCheckboxEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 7, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddCheckbox", 0, errorMessage) ||
        !ensureString(args[1], "guiAddCheckbox", 1, errorMessage) ||
        !ensureBool(args[2], "guiAddCheckbox", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddCheckbox", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddCheckbox", 4, errorMessage) ||
        !ensureWholeNumber(args[5], "guiAddCheckbox", 5, errorMessage) ||
        !ensureWholeNumber(args[6], "guiAddCheckbox", 6, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddCheckbox(asInt(args[0]), args[1].asString(), args[2].asBool(),
                        asInt(args[3]), asInt(args[4]), asInt(args[5]), asInt(args[6]),
                        &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddListBoxEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 5, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddListBox", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiAddListBox", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddListBox", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddListBox", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddListBox", 4, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddListBox(asInt(args[0]), asInt(args[1]), asInt(args[2]),
                       asInt(args[3]), asInt(args[4]), &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiAddProgressBarEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 5, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddProgressBar", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiAddProgressBar", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiAddProgressBar", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiAddProgressBar", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiAddProgressBar", 4, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiAddProgressBar(asInt(args[0]), asInt(args[1]), asInt(args[2]),
                           asInt(args[3]), asInt(args[4]), &controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiSetTextEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetText", 0, errorMessage) ||
        !ensureString(args[1], "guiSetText", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetText(asInt(args[0]), args[1].asString(), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiGetTextEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiGetText", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string result;
    if (!guiGetText(asInt(args[0]), &result, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(result);
}

Value nativeGuiSetValueEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetValue", 0, errorMessage) ||
        !ensureNumber(args[1], "guiSetValue", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetValue(asInt(args[0]), args[1].asNumber(), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiGetValueEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiGetValue", 0, errorMessage)) {
        return Value::nilValue();
    }

    double value = 0.0;
    if (!guiGetValue(asInt(args[0]), &value, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(value);
}

Value nativeGuiSetCheckedEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetChecked", 0, errorMessage) ||
        !ensureBool(args[1], "guiSetChecked", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetChecked(asInt(args[0]), args[1].asBool(), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiGetCheckedEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiGetChecked", 0, errorMessage)) {
        return Value::nilValue();
    }

    bool checked = false;
    if (!guiGetChecked(asInt(args[0]), &checked, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(checked);
}

Value nativeGuiAddListItemEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiAddListItem", 0, errorMessage) ||
        !ensureString(args[1], "guiAddListItem", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiAddListItem(asInt(args[0]), args[1].asString(), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiClearListEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiClearList", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiClearList(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiGetSelectedIndexEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiGetSelectedIndex", 0, errorMessage)) {
        return Value::nilValue();
    }

    int index = -1;
    if (!guiGetSelectedIndex(asInt(args[0]), &index, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(index));
}

Value nativeGuiSetSelectedIndexEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetSelectedIndex", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiSetSelectedIndex", 1, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetSelectedIndex(asInt(args[0]), asInt(args[1]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiSetBoundsEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 5, errorMessage) ||
        !ensureWholeNumber(args[0], "guiSetBounds", 0, errorMessage) ||
        !ensureWholeNumber(args[1], "guiSetBounds", 1, errorMessage) ||
        !ensureWholeNumber(args[2], "guiSetBounds", 2, errorMessage) ||
        !ensureWholeNumber(args[3], "guiSetBounds", 3, errorMessage) ||
        !ensureWholeNumber(args[4], "guiSetBounds", 4, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetBounds(asInt(args[0]), asInt(args[1]), asInt(args[2]),
                      asInt(args[3]), asInt(args[4]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiShowControlEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiShowControl", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiShowControl(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiHideControlEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiHideControl", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiHideControl(asInt(args[0]), errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiEnableControlEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiEnableControl", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetControlEnabled(asInt(args[0]), true, errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiDisableControlEx(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "guiDisableControl", 0, errorMessage)) {
        return Value::nilValue();
    }

    if (!guiSetControlEnabled(asInt(args[0]), false, errorMessage)) {
        return Value::nilValue();
    }

    return Value::nilValue();
}

Value nativeGuiPollEventEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    bool hasEvent = false;
    if (!guiPollEvent(&hasEvent, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(hasEvent);
}

Value nativeGuiWaitEventEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    bool hasEvent = false;
    if (!guiWaitEvent(&hasEvent, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(hasEvent);
}

Value nativeGuiEventTypeEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string eventType;
    if (!guiGetEventType(&eventType, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(eventType);
}

Value nativeGuiEventWindowEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    int windowId = 0;
    if (!guiGetEventWindow(&windowId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(windowId));
}

Value nativeGuiEventControlEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    int controlId = 0;
    if (!guiGetEventControl(&controlId, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(controlId));
}

Value nativeGuiEventTextEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    std::string text;
    if (!guiGetEventText(&text, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(text);
}

Value nativeGuiEventCheckedEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    bool checked = false;
    if (!guiGetEventChecked(&checked, errorMessage)) {
        return Value::nilValue();
    }

    return Value::boolValue(checked);
}

Value nativeGuiEventIndexEx(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    int index = -1;
    if (!guiGetEventIndex(&index, errorMessage)) {
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(index));
}

bool getIndexedValue(const Value& receiver, const Value& indexValue,
                     Value* result, std::string* errorMessage) {
    if (receiver.isArray()) {
        const ArrayPtr& array = receiver.asArray();
        std::size_t index = 0;
        std::size_t size = array == nullptr ? 0 : array->elements.size();
        if (!valueToArrayIndex(indexValue, size, false, "Array access", &index, errorMessage)) {
            return false;
        }

        *result = array->elements[index];
        return true;
    }

    if (receiver.isMap()) {
        std::string key;
        if (!valueToMapKey(indexValue, "Map access", &key, errorMessage)) {
            return false;
        }

        const MapPtr& map = receiver.asMap();
        if (map == nullptr) {
            *result = Value::nilValue();
            return true;
        }

        auto it = map->entries.find(key);
        if (it == map->entries.end()) {
            *result = Value::nilValue();
            return true;
        }

        *result = it->second;
        return true;
    }

    if (receiver.isString()) {
        const std::string& text = receiver.asString();
        std::size_t index = 0;
        if (!valueToArrayIndex(indexValue, text.size(), false, "String access", &index, errorMessage)) {
            return false;
        }

        *result = Value::stringValue(std::string(1, text[index]));
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Only arrays, maps, and strings support indexed access.";
    }
    return false;
}

bool setIndexedValue(const Value& receiver, const Value& indexValue, const Value& assignedValue,
                     std::string* errorMessage) {
    if (receiver.isArray()) {
        const ArrayPtr& array = receiver.asArray();
        if (array == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "Cannot assign into an invalid array.";
            }
            return false;
        }

        std::size_t index = 0;
        if (!valueToArrayIndex(
                indexValue, array->elements.size(), true, "Array assignment", &index, errorMessage)) {
            return false;
        }

        if (index == array->elements.size()) {
            array->elements.push_back(assignedValue);
            writeBarrier(array, assignedValue);
        } else {
            array->elements[index] = assignedValue;
            writeBarrier(array, assignedValue);
        }
        return true;
    }

    if (receiver.isMap()) {
        std::string key;
        if (!valueToMapKey(indexValue, "Map assignment", &key, errorMessage)) {
            return false;
        }

        const MapPtr& map = receiver.asMap();
        if (map == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "Cannot assign into an invalid map.";
            }
            return false;
        }

        map->entries[key] = assignedValue;
        writeBarrier(map, assignedValue);
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Only arrays and maps support indexed assignment.";
    }
    return false;
}

ClosurePtr findMethod(const ClassPtr& klass, const std::string& name) {
    ClassPtr current = klass;
    while (current != nullptr) {
        auto method = current->methods.find(name);
        if (method != current->methods.end()) {
            return method->second;
        }

        current = current->superclass;
    }

    return nullptr;
}

bool getPropertyValue(const Value& receiver, const std::string& property,
                      Value* result, std::string* errorMessage) {
    if (receiver.isInstance()) {
        const InstancePtr& instance = receiver.asInstance();
        if (instance == nullptr) {
            *result = Value::nilValue();
            return true;
        }

        auto field = instance->fields.find(property);
        if (field != instance->fields.end()) {
            *result = field->second;
            return true;
        }

        if (instance->klass != nullptr) {
            ClosurePtr method = findMethod(instance->klass, property);
            if (method != nullptr) {
                *result = Value::boundMethodValue(
                    uraniumHeap().allocateBoundMethod(receiver, method));
                return true;
            }
        }

        *result = Value::nilValue();
        return true;
    }

    if (receiver.isClass()) {
        const ClassPtr& klass = receiver.asClass();
        if (klass == nullptr) {
            *result = Value::nilValue();
            return true;
        }

        if (property == "name") {
            *result = Value::stringValue(klass->name);
            return true;
        }

        if (property == "super") {
            if (klass->superclass == nullptr) {
                *result = Value::nilValue();
            } else {
                *result = Value::classValue(klass->superclass);
            }
            return true;
        }

        *result = Value::nilValue();
        return true;
    }

    if (receiver.isMap()) {
        const MapPtr& map = receiver.asMap();
        if (map == nullptr) {
            *result = Value::nilValue();
            return true;
        }

        auto it = map->entries.find(property);
        if (it == map->entries.end()) {
            *result = Value::nilValue();
            return true;
        }

        *result = it->second;
        return true;
    }

    if (receiver.isArray()) {
        if (property == "length") {
            const ArrayPtr& array = receiver.asArray();
            *result = Value::numberValue(
                static_cast<double>(array == nullptr ? 0 : array->elements.size()));
            return true;
        }

        if (errorMessage != nullptr) {
            *errorMessage = "Arrays only expose the 'length' property.";
        }
        return false;
    }

    if (receiver.isString()) {
        if (property == "length") {
            *result = Value::numberValue(static_cast<double>(receiver.asString().size()));
            return true;
        }

        if (errorMessage != nullptr) {
            *errorMessage = "Strings only expose the 'length' property.";
        }
        return false;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Only instances, maps, arrays, strings, and classes support property access.";
    }
    return false;
}

bool setPropertyValue(const Value& receiver, const std::string& property, const Value& assignedValue,
                      std::string* errorMessage) {
    if (receiver.isInstance()) {
        const InstancePtr& instance = receiver.asInstance();
        if (instance == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "Cannot assign into an invalid instance.";
            }
            return false;
        }

        instance->fields[property] = assignedValue;
        writeBarrier(instance, assignedValue);
        return true;
    }

    if (receiver.isMap()) {
        const MapPtr& map = receiver.asMap();
        if (map == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "Cannot assign into an invalid map.";
            }
            return false;
        }

        map->entries[property] = assignedValue;
        writeBarrier(map, assignedValue);
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Only instances and maps support property assignment.";
    }
    return false;
}

} // namespace

VM::VM()
    : currentTask(nullptr),
      rootTask(nullptr),
      nextTaskId(1),
      fastPathCompiledCount(0),
      fastPathUnsupportedCount(0),
      nativeJitCompiledCount(0),
      bytecodeFastPathCompiledCount(0),
      debugTraceEnabled(false) {
    resetScheduler();
    registerStandardLibrary();
}

VM::~VM() {
}

void VM::resetScheduler() {
    currentTask = nullptr;
    rootTask = nullptr;
    tasks.clear();
    readyQueue.clear();
    nextTaskId = 1;
}

bool VM::ensureTaskStackCapacity(TaskPtr task, std::size_t neededSlots) {
    if (task == nullptr) {
        runtimeError("Missing task stack.");
        return false;
    }

    std::size_t currentCapacity = task->stack.size();
    if (neededSlots <= currentCapacity) {
        return true;
    }

    Value* oldBase = task->stack.data();
    std::size_t oldTopIndex =
        static_cast<std::size_t>(task->stackTop - oldBase);

    std::size_t newCapacity = std::max(neededSlots, std::max<std::size_t>(64, currentCapacity * 2));
    task->stack.resize(newCapacity);
    Value* newBase = task->stack.data();
    task->stackTop = newBase + oldTopIndex;

    if (oldBase != newBase) {
        for (int index = 0; index < task->frameCount; ++index) {
            CallFrame& frame = task->frames[static_cast<std::size_t>(index)];
            if (frame.slots != nullptr) {
                frame.slots = newBase + (frame.slots - oldBase);
            }
            if (frame.base != nullptr) {
                frame.base = newBase + (frame.base - oldBase);
            }
        }

        for (int index = 0; index < task->handlerCount; ++index) {
            ExceptionHandler& handler = task->handlers[static_cast<std::size_t>(index)];
            if (handler.stackLevel != nullptr) {
                handler.stackLevel = newBase + (handler.stackLevel - oldBase);
            }
        }

        for (UpvaluePtr upvalue = task->openUpvalues; upvalue != nullptr; upvalue = upvalue->next) {
            if (upvalue->location != nullptr &&
                upvalue->location >= oldBase &&
                upvalue->location < oldBase + currentCapacity) {
                upvalue->location = newBase + (upvalue->location - oldBase);
            }
        }
    }

    return true;
}

bool VM::ensureFrameCapacity(TaskPtr task, int neededCount) {
    if (task == nullptr) {
        runtimeError("Missing task frame stack.");
        return false;
    }

    if (neededCount <= static_cast<int>(task->frames.size())) {
        return true;
    }

    std::size_t newCapacity =
        std::max<std::size_t>(static_cast<std::size_t>(neededCount),
                              std::max<std::size_t>(16, task->frames.size() * 2));
    task->frames.resize(newCapacity);
    return true;
}

bool VM::ensureHandlerCapacity(TaskPtr task, int neededCount) {
    if (task == nullptr) {
        runtimeError("Missing task handler stack.");
        return false;
    }

    if (neededCount <= static_cast<int>(task->handlers.size())) {
        return true;
    }

    std::size_t newCapacity =
        std::max<std::size_t>(static_cast<std::size_t>(neededCount),
                              std::max<std::size_t>(32, task->handlers.size() * 2));
    task->handlers.resize(newCapacity);
    return true;
}

void VM::markRoots() {
    for (const auto& global : globals) {
        uraniumHeap().markValue(global.second);
    }

    for (const std::unique_ptr<TaskHandle>& taskStorage : tasks) {
        const TaskPtr task = taskStorage.get();
        if (task == nullptr) {
            continue;
        }

        for (Value* slot = task->stack.data(); slot < task->stackTop; ++slot) {
            uraniumHeap().markValue(*slot);
        }

        uraniumHeap().markValue(task->resumeValue);
        uraniumHeap().markValue(task->result);
        uraniumHeap().markValue(task->failure);

        for (int index = 0; index < task->frameCount; ++index) {
            if (task->frames[index].closure != nullptr) {
                uraniumHeap().markObject(task->frames[index].closure);
            } else if (task->frames[index].function != nullptr) {
                uraniumHeap().markObject(task->frames[index].function);
            }
        }

        uraniumHeap().markObject(task->openUpvalues);
    }
}

void VM::collectGarbage(bool fullCollection) {
    markRoots();
    uraniumHeap().collectGarbage(fullCollection ? HEAP_COLLECT_FULL : HEAP_COLLECT_YOUNG);
}

void VM::maybeCollectGarbage() {
    if (uraniumHeap().shouldCollectFull()) {
        collectGarbage(true);
        return;
    }

    if (uraniumHeap().shouldCollectYoung()) {
        collectGarbage(false);
    }
}

void VM::defineNative(const std::string& name, int arity, NativeCallback callback) {
    globals[name] = Value::nativeFunctionValue(
        uraniumHeap().allocateNativeFunction(name, arity, callback));
    constantGlobals.insert(name);
}

void VM::defineNumberConstant(const std::string& name, double value) {
    globals[name] = Value::numberValue(value);
    constantGlobals.insert(name);
}

void VM::defineStringConstant(const std::string& name, const std::string& value) {
    globals[name] = Value::stringValue(value);
    constantGlobals.insert(name);
}

void VM::registerStandardLibrary() {
    defineNative("abs", 1, nativeAbs);
    defineNative("sqrt", 1, nativeSqrt);
    defineNative("pow", 2, nativePow);
    defineNative("min", 2, nativeMin);
    defineNative("max", 2, nativeMax);
    defineNative("clamp", 3, nativeClamp);
    defineNative("floor", 1, nativeFloor);
    defineNative("ceil", 1, nativeCeil);
    defineNative("round", 1, nativeRound);
    defineNative("sin", 1, nativeSin);
    defineNative("cos", 1, nativeCos);
    defineNative("tan", 1, nativeTan);
    defineNative("asin", 1, nativeAsin);
    defineNative("acos", 1, nativeAcos);
    defineNative("atan", 1, nativeAtan);
    defineNative("atan2", 2, nativeAtan2);
    defineNative("exp", 1, nativeExp);
    defineNative("log", 1, nativeLog);
    defineNative("log10", 1, nativeLog10);
    defineNative("mod", 2, nativeMod);
    defineNative("sign", 1, nativeSign);
    defineNative("radians", 1, nativeRadians);
    defineNative("degrees", 1, nativeDegrees);
    defineNative("random", 0, nativeRandom);
    defineNative("randRange", 2, nativeRandRange);
    defineNative("randInt", 2, nativeRandInt);
    defineNative("seedRandom", 1, nativeSeedRandom);

    defineNative("len", 1, nativeLen);
    defineNative("upper", 1, nativeUpper);
    defineNative("lower", 1, nativeLower);
    defineNative("trim", 1, nativeTrim);
    defineNative("repeat", 2, nativeRepeat);
    defineNative("contains", 2, nativeContains);
    defineNative("startsWith", 2, nativeStartsWith);
    defineNative("endsWith", 2, nativeEndsWith);
    defineNative("left", 2, nativeLeft);
    defineNative("right", 2, nativeRight);
    defineNative("slice", 3, nativeSlice);
    defineNative("replace", 3, nativeReplace);
    defineNative("toNumber", 1, nativeToNumber);
    defineNative("regexMatch", 2, nativeRegexMatch);
    defineNative("regexSearch", 2, nativeRegexSearch);
    defineNative("regexFindAll", 2, nativeRegexFindAll);
    defineNative("regexReplace", 3, nativeRegexReplace);
    defineNative("regexSplit", 2, nativeRegexSplit);
    defineNative("base64Encode", 1, nativeBase64Encode);
    defineNative("base64Decode", 1, nativeBase64Decode);
    defineNative("str", 1, nativeStr);
    defineNative("typeOf", 1, nativeTypeOf);
    defineNative("isNil", 1, nativeIsNil);
    defineNative("isBool", 1, nativeIsBool);
    defineNative("isNumber", 1, nativeIsNumber);
    defineNative("isString", 1, nativeIsString);
    defineNative("isFunction", 1, nativeIsFunction);
    defineNative("isNativeFunction", 1, nativeIsNativeFunction);
    defineNative("isArray", 1, nativeIsArray);
    defineNative("isMap", 1, nativeIsMap);
    defineNative("isClass", 1, nativeIsClass);
    defineNative("isInstance", 1, nativeIsInstance);
    defineNative("isTask", 1, nativeIsTask);
    defineNative("array", 0, nativeArray);
    defineNative("map", 0, nativeMap);
    defineNative("push", 2, nativePush);
    defineNative("pop", 1, nativePop);
    defineNative("hasKey", 2, nativeHasKey);
    defineNative("deleteKey", 2, nativeDeleteKey);
    defineNative("keys", 1, nativeKeys);
    defineNative("clock", 0, nativeClock);
    defineNative("unixMillis", 0, nativeUnixMillis);
    defineNative("println", 1, nativePrintln);
    defineNative("fsCwd", 0, nativeFsCwd);
    defineNative("fsChangeDir", 1, nativeFsChangeDir);
    defineNative("fsExists", 1, nativeFsExists);
    defineNative("fsIsFile", 1, nativeFsIsFile);
    defineNative("fsIsDir", 1, nativeFsIsDir);
    defineNative("fsNormalize", 1, nativeFsNormalize);
    defineNative("fsAbsolute", 1, nativeFsAbsolute);
    defineNative("fsParent", 1, nativeFsParent);
    defineNative("fsFileName", 1, nativeFsFileName);
    defineNative("fsStem", 1, nativeFsStem);
    defineNative("fsExtension", 1, nativeFsExtension);
    defineNative("fsJoin2", 2, nativeFsJoin2);
    defineNative("fsJoin3", 3, nativeFsJoin3);
    defineNative("fsJoin4", 4, nativeFsJoin4);
    defineNative("fsReadText", 1, nativeFsReadText);
    defineNative("fsReadLines", 1, nativeFsReadLines);
    defineNative("fsWriteText", 2, nativeFsWriteText);
    defineNative("fsAppendText", 2, nativeFsAppendText);
    defineNative("fsCreateDir", 1, nativeFsCreateDir);
    defineNative("fsCreateDirs", 1, nativeFsCreateDirs);
    defineNative("fsRemove", 1, nativeFsRemove);
    defineNative("fsRemoveTree", 1, nativeFsRemoveTree);
    defineNative("fsCopy", 2, nativeFsCopy);
    defineNative("fsMove", 2, nativeFsMove);
    defineNative("fsStat", 1, nativeFsStat);
    defineNative("fsListNames", 1, nativeFsListNames);
    defineNative("fsListEntries", 1, nativeFsListEntries);
    defineNative("fsWalk", 1, nativeFsWalk);
    defineNative("processArgs", 0, nativeProcessArgs);
    defineNative("processArgCount", 0, nativeProcessArgCount);
    defineNative("processExecutablePath", 0, nativeProcessExecutablePath);
    defineNative("processEntryPath", 0, nativeProcessEntryPath);
    defineNative("processCwd", 0, nativeProcessCwd);
    defineNative("processChangeDir", 1, nativeProcessChangeDir);
    defineNative("processGetEnv", 1, nativeProcessGetEnv);
    defineNative("processSetEnv", 2, nativeProcessSetEnv);
    defineNative("processPlatform", 0, nativeProcessPlatform);
    defineNative("processFeatures", 0, nativeRuntimeCapabilities);
    defineNative("runtimeCapabilities", 0, nativeRuntimeCapabilities);
    defineNative("processPid", 0, nativeProcessPid);
    defineNative("processSleep", 1, nativeProcessSleep);
    defineNative("processRun", 1, nativeProcessRun);
    defineNative("processSystem", 1, nativeProcessSystem);
    defineNative("processExit", 1, nativeProcessExit);
    defineNative("threadHardwareConcurrency", 0, nativeThreadHardwareConcurrency);
    defineNative("threadYield", 0, nativeThreadYield);
    defineNative("taskStatus", 1, nativeTaskStatus);
    defineNative("taskDone", 1, nativeTaskDone);
    defineNative("taskFailed", 1, nativeTaskFailed);
    defineNative("taskResult", 1, nativeTaskResult);
    defineNative("taskError", 1, nativeTaskError);
    defineNative("sleepAsync", 1, nativeSleepAsync);
    defineNative("yieldAsync", 0, nativeYieldAsync);
    defineNative("jitCompile", 1, nativeJitCompile);
    defineNative("jitStatus", 1, nativeJitStatus);
    defineNative("jitBackend", 1, nativeJitBackend);
    defineNative("jitStats", 0, nativeJitStats);
    defineNative("gcCollect", 0, nativeGcCollect);
    defineNative("gcStats", 0, nativeGcStats);
    defineNative("httpRequest", 4, nativeHttpRequest);
    defineNative("godotFindEditor", 1, nativeGodotFindEditor);
    defineNative("godotCreateProject", 2, nativeGodotCreateProject);
    defineNative("godotCreateScene", 2, nativeGodotCreateScene);
    defineNative("godotCreateScript", 2, nativeGodotCreateScript);
    defineNative("godotCreatePlugin", 2, nativeGodotCreatePlugin);
    defineNative("godotCreateGDExtension", 2, nativeGodotCreateGDExtension);
    defineNative("godotBuildCommand", 2, nativeGodotBuildCommand);
    defineNative("jsonParse", 1, nativeJsonParse);
    defineNative("jsonValid", 1, nativeJsonValid);
    defineNative("jsonStringify", 1, nativeJsonStringify);
    defineNative("jsonStringifyPretty", 2, nativeJsonStringifyPretty);
    defineNative("guiInfo", 2, nativeGuiInfo);
    defineNative("guiWarn", 2, nativeGuiWarn);
    defineNative("guiError", 2, nativeGuiError);
    defineNative("guiConfirm", 2, nativeGuiConfirm);
    defineNative("guiConfirmCancel", 2, nativeGuiConfirmCancel);
    defineNative("guiPrompt", 3, nativeGuiPrompt);
    defineNative("guiOpenFile", 1, nativeGuiOpenFile);
    defineNative("guiSaveFile", 2, nativeGuiSaveFile);
    defineNative("guiPickFolder", 1, nativeGuiPickFolder);
    defineNative("guiScreenWidth", 0, nativeGuiScreenWidth);
    defineNative("guiScreenHeight", 0, nativeGuiScreenHeight);
    defineNative("guiCreateWindow", 3, nativeGuiCreateWindow);
    defineNative("guiShowWindow", 1, nativeGuiShowWindowEx);
    defineNative("guiHideWindow", 1, nativeGuiHideWindowEx);
    defineNative("guiCloseWindow", 1, nativeGuiCloseWindowEx);
    defineNative("guiCenterWindow", 1, nativeGuiCenterWindowEx);
    defineNative("guiSetWindowTitle", 2, nativeGuiSetWindowTitleEx);
    defineNative("guiSetWindowSize", 3, nativeGuiSetWindowSizeEx);
    defineNative("guiAddLabel", 6, nativeGuiAddLabelEx);
    defineNative("guiAddButton", 6, nativeGuiAddButtonEx);
    defineNative("guiAddInput", 6, nativeGuiAddInputEx);
    defineNative("guiAddTextArea", 6, nativeGuiAddTextAreaEx);
    defineNative("guiAddCheckbox", 7, nativeGuiAddCheckboxEx);
    defineNative("guiAddListBox", 5, nativeGuiAddListBoxEx);
    defineNative("guiAddProgressBar", 5, nativeGuiAddProgressBarEx);
    defineNative("guiSetText", 2, nativeGuiSetTextEx);
    defineNative("guiGetText", 1, nativeGuiGetTextEx);
    defineNative("guiSetValue", 2, nativeGuiSetValueEx);
    defineNative("guiGetValue", 1, nativeGuiGetValueEx);
    defineNative("guiSetChecked", 2, nativeGuiSetCheckedEx);
    defineNative("guiGetChecked", 1, nativeGuiGetCheckedEx);
    defineNative("guiAddListItem", 2, nativeGuiAddListItemEx);
    defineNative("guiClearList", 1, nativeGuiClearListEx);
    defineNative("guiGetSelectedIndex", 1, nativeGuiGetSelectedIndexEx);
    defineNative("guiSetSelectedIndex", 2, nativeGuiSetSelectedIndexEx);
    defineNative("guiSetBounds", 5, nativeGuiSetBoundsEx);
    defineNative("guiShowControl", 1, nativeGuiShowControlEx);
    defineNative("guiHideControl", 1, nativeGuiHideControlEx);
    defineNative("guiEnableControl", 1, nativeGuiEnableControlEx);
    defineNative("guiDisableControl", 1, nativeGuiDisableControlEx);
    defineNative("guiPollEvent", 0, nativeGuiPollEventEx);
    defineNative("guiWaitEvent", 0, nativeGuiWaitEventEx);
    defineNative("guiEventType", 0, nativeGuiEventTypeEx);
    defineNative("guiEventWindow", 0, nativeGuiEventWindowEx);
    defineNative("guiEventControl", 0, nativeGuiEventControlEx);
    defineNative("guiEventText", 0, nativeGuiEventTextEx);
    defineNative("guiEventChecked", 0, nativeGuiEventCheckedEx);
    defineNative("guiEventIndex", 0, nativeGuiEventIndexEx);

    defineNative("netTcpConnect", 2, nativeNetTcpConnect);
    defineNative("netTcpListen", 2, nativeNetTcpListen);
    defineNative("netTcpAccept", 1, nativeNetTcpAccept);
    defineNative("netTcpReceive", 2, nativeNetTcpReceive);
    defineNative("netTcpSend", 2, nativeNetTcpSend);
    defineNative("netTcpClose", 1, nativeNetTcpClose);

    defineNative("cryptoHashSha256", 1, nativeCryptoHashSha256);
    defineNative("cryptoBase64Encode", 1, nativeCryptoBase64Encode);
    defineNative("cryptoBase64Decode", 1, nativeCryptoBase64Decode);
    defineNative("cryptoAesEncrypt", 2, nativeCryptoAesEncrypt);
    defineNative("cryptoAesDecrypt", 2, nativeCryptoAesDecrypt);

    defineNative("dbOpen", 1, nativeDbOpen);
    defineNative("dbExecute", 2, nativeDbExecute);
    defineNative("dbQuery", 2, nativeDbQuery);
    defineNative("dbClose", 1, nativeDbClose);

    defineNative("threadSpawn", 1, nativeThreadSpawn);
    defineNative("threadJoin", 1, nativeThreadJoin);
    defineNative("threadChannelCreate", 1, nativeThreadChannelCreate);
    defineNative("threadChannelSend", 2, nativeThreadChannelSend);
    defineNative("threadChannelReceive", 1, nativeThreadChannelReceive);
    defineNative("threadChannelTryReceive", 1, nativeThreadChannelTryReceive);
    defineNative("threadChannelPoll", 1, nativeThreadChannelPoll);
    defineNative("threadChannelSize", 1, nativeThreadChannelSize);
    defineNative("threadChannelClose", 1, nativeThreadChannelClose);
    defineNative("threadMutexCreate", 0, nativeThreadMutexCreate);
    defineNative("threadMutexLock", 1, nativeThreadMutexLock);
    defineNative("threadMutexTryLock", 1, nativeThreadMutexTryLock);
    defineNative("threadMutexUnlock", 1, nativeThreadMutexUnlock);
    defineNative("threadWorkerPoolCreate", 1, nativeThreadWorkerPoolCreate);
    defineNative("threadWorkerPoolDestroy", 1, nativeThreadWorkerPoolDestroy);
    defineNative("threadWorkerReadText", 2, nativeThreadWorkerReadText);
    defineNative("threadWorkerWriteText", 3, nativeThreadWorkerWriteText);
    defineNative("threadWorkerHttpGet", 2, nativeThreadWorkerHttpGet);
    defineNative("threadWorkerStatus", 1, nativeThreadWorkerStatus);
    defineNative("threadWorkerDone", 1, nativeThreadWorkerDone);
    defineNative("threadWorkerResult", 1, nativeThreadWorkerResult);
    defineNative("threadWorkerError", 1, nativeThreadWorkerError);
    defineNative("threadWorkerWait", 1, nativeThreadWorkerWait);

    defineNumberConstant("PI", 3.14159265358979323846);
    defineNumberConstant("TAU", 6.28318530717958647692);
    defineNumberConstant("E", 2.71828182845904523536);
    defineNumberConstant("PHI", 1.61803398874989484820);
    defineNumberConstant("DEG_TO_RAD", 3.14159265358979323846 / 180.0);
    defineNumberConstant("RAD_TO_DEG", 180.0 / 3.14159265358979323846);
    defineStringConstant("URANIUM_VERSION", "1.0");
}

TaskPtr VM::createTaskHandle(const std::string& name) {
    tasks.push_back(std::make_unique<TaskHandle>(nextTaskId++, name));
    return tasks.back().get();
}

TaskPtr VM::createTimerTask(long long wakeAtMillis,
                            const Value& result,
                            const std::string& name) {
    TaskPtr task = createTaskHandle(name);
    task->state = TASK_SLEEPING;
    task->wakeAtMillis = wakeAtMillis;
    task->result = result;
    maybeCollectGarbage();
    return task;
}

TaskPtr VM::createAsyncClosureTask(const ClosurePtr& closure,
                                   int argCount,
                                   const Value* args) {
    if (closure == nullptr || closure->function == nullptr) {
        runtimeError("Attempted to schedule an invalid async function.");
        return nullptr;
    }

    const FunctionPtr& function = closure->function;
    if (argCount != function->arity) {
        std::string functionName = function->name.empty() ? "<async>" : function->name;
        runtimeError(
            "Async function '" + functionName + "' expected " +
            std::to_string(function->arity) + " argument(s) but got " +
            std::to_string(argCount) + "."
        );
        return nullptr;
    }

    TaskPtr task = createTaskHandle(function->name.empty() ? "async" : function->name);
    if (!ensureTaskStackCapacity(task, static_cast<std::size_t>(argCount + 1)) ||
        !ensureFrameCapacity(task, 1)) {
        return nullptr;
    }
    task->stack[0] = Value::closureValue(closure);
    for (int index = 0; index < argCount; ++index) {
        task->stack[index + 1] = args[index];
    }
    task->stackTop = task->stack.data() + argCount + 1;

    CallFrame& frame = task->frames[static_cast<std::size_t>(task->frameCount++)];
    frame.closure = closure;
    frame.function = function;
    frame.ip = function->chunk.code.data();
    frame.slots = task->stack.data() + 1;
    frame.base = task->stack.data();

    enqueueReadyTask(task);
    maybeCollectGarbage();
    return task;
}

TaskPtr VM::createAsyncBoundMethodTask(const BoundMethodPtr& boundMethod,
                                       int argCount,
                                       const Value* args) {
    if (boundMethod == nullptr || boundMethod->method == nullptr ||
        boundMethod->method->function == nullptr) {
        runtimeError("Attempted to schedule an invalid async method.");
        return nullptr;
    }

    int expectedArgs = boundMethod->method->function->arity - 1;
    if (argCount != expectedArgs) {
        std::string methodName =
            boundMethod->method->function->name.empty()
                ? "<async method>"
                : boundMethod->method->function->name;
        runtimeError(
            "Async method '" + methodName + "' expected " +
            std::to_string(expectedArgs) + " argument(s) but got " +
            std::to_string(argCount) + "."
        );
        return nullptr;
    }

    TaskPtr task = createTaskHandle(
        boundMethod->method->function->name.empty() ? "async_method"
                                                    : boundMethod->method->function->name);
    if (!ensureTaskStackCapacity(task, static_cast<std::size_t>(argCount + 1)) ||
        !ensureFrameCapacity(task, 1)) {
        return nullptr;
    }
    task->stack[0] = boundMethod->receiver;
    for (int index = 0; index < argCount; ++index) {
        task->stack[index + 1] = args[index];
    }
    task->stackTop = task->stack.data() + argCount + 1;

    CallFrame& frame = task->frames[static_cast<std::size_t>(task->frameCount++)];
    frame.closure = boundMethod->method;
    frame.function = boundMethod->method->function;
    frame.ip = boundMethod->method->function->chunk.code.data();
    frame.slots = task->stack.data();
    frame.base = task->stack.data();

    enqueueReadyTask(task);
    maybeCollectGarbage();
    return task;
}

void VM::enqueueReadyTask(TaskPtr task) {
    if (task == nullptr) {
        return;
    }

    task->state = TASK_READY;
    readyQueue.push_back(task);
}

void VM::resetLoopBroker(TaskPtr task) {
    if (task == nullptr) {
        return;
    }

    task->loopBrokerWindowStartMillis = currentUnixMillis();
    task->loopBrokerInstructionCount = 0;
    task->loopBrokerLoopCount = 0;
}

bool VM::noteLoopBrokerInstruction(TaskPtr task, std::string* errorMessage) {
    if (task == nullptr) {
        return true;
    }

    if (task->loopBrokerWindowStartMillis == 0) {
        resetLoopBroker(task);
    }

    task->loopBrokerInstructionCount++;
    long long elapsed = currentUnixMillis() - task->loopBrokerWindowStartMillis;
    if (elapsed < kLoopBrokerWallClockLimitMillis) {
        return true;
    }

    if (task->loopBrokerInstructionCount < kLoopBrokerInstructionLimit) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage =
            "Loop broker stopped task '" + task->name +
            "' after " + std::to_string(task->loopBrokerInstructionCount) +
            " instructions in " + std::to_string(elapsed) +
            "ms. Probable infinite loop or non-yielding hot path detected.";
    }
    return false;
}

bool VM::noteLoopBrokerLoop(TaskPtr task, std::string* errorMessage) {
    if (task == nullptr) {
        return true;
    }

    if (task->loopBrokerWindowStartMillis == 0) {
        resetLoopBroker(task);
    }

    task->loopBrokerLoopCount++;
    long long elapsed = currentUnixMillis() - task->loopBrokerWindowStartMillis;
    if (elapsed < kLoopBrokerWallClockLimitMillis) {
        return true;
    }

    if (task->loopBrokerLoopCount < kLoopBrokerLoopLimit) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage =
            "Loop broker detected a probable infinite loop in task '" + task->name +
            "' after " + std::to_string(task->loopBrokerLoopCount) +
            " back-edges in " + std::to_string(elapsed) + "ms.";
    }
    return false;
}

void VM::wakeAwaiters(TaskPtr task, const Value& value, bool isException) {
    if (task == nullptr) {
        return;
    }

    for (TaskPtr awaiter : task->awaiters) {
        if (awaiter == nullptr) {
            continue;
        }

        awaiter->waitingOn = nullptr;
        awaiter->hasResumeValue = true;
        awaiter->resumeIsException = isException;
        awaiter->resumeValue = value;
        enqueueReadyTask(awaiter);
    }

    task->awaiters.clear();
}

void VM::resolveTask(TaskPtr task, const Value& value) {
    if (task == nullptr ||
        task->state == TASK_COMPLETED ||
        task->state == TASK_FAILED) {
        return;
    }

    task->state = TASK_COMPLETED;
    task->result = value;
    task->stackTop = task->stack.data();
    task->frameCount = 0;
    task->handlerCount = 0;
    task->openUpvalues = nullptr;
    resetLoopBroker(task);
    wakeAwaiters(task, value, false);
}

void VM::failTask(TaskPtr task, const Value& error) {
    if (task == nullptr ||
        task->state == TASK_COMPLETED ||
        task->state == TASK_FAILED) {
        return;
    }

    task->state = TASK_FAILED;
    task->failure = error;
    task->stackTop = task->stack.data();
    task->frameCount = 0;
    task->handlerCount = 0;
    task->openUpvalues = nullptr;
    resetLoopBroker(task);
    wakeAwaiters(task, error, true);
}

bool VM::applyPendingResume(TaskPtr task) {
    if (task == nullptr || !task->hasResumeValue) {
        return true;
    }

    Value resumeValue = task->resumeValue;
    bool isException = task->resumeIsException;
    task->hasResumeValue = false;
    task->resumeIsException = false;
    task->resumeValue = Value::nilValue();
    resetLoopBroker(task);

    if (!isException) {
        push(resumeValue);
        return true;
    }

    if (propagateException(resumeValue)) {
        return true;
    }

    failTask(task, resumeValue);
    return false;
}

void VM::pollSleepingTasks() {
    long long now = currentUnixMillis();
    for (const std::unique_ptr<TaskHandle>& taskStorage : tasks) {
        TaskPtr task = taskStorage.get();
        if (task == nullptr || task->state != TASK_SLEEPING) {
            continue;
        }

        if (task->wakeAtMillis <= now) {
            resolveTask(task, task->result);
        }
    }
}

bool VM::hasReadyTask() const {
    for (TaskPtr task : readyQueue) {
        if (task != nullptr && task->state == TASK_READY) {
            return true;
        }
    }

    return false;
}

bool VM::hasPendingLiveTasks() const {
    for (const std::unique_ptr<TaskHandle>& taskStorage : tasks) {
        TaskPtr task = taskStorage.get();
        if (task == nullptr) {
            continue;
        }

        if (task->state == TASK_READY ||
            task->state == TASK_RUNNING ||
            task->state == TASK_WAITING ||
            task->state == TASK_SLEEPING) {
            return true;
        }
    }

    return false;
}

long long VM::nextWakeDelayMillis() const {
    long long now = currentUnixMillis();
    long long best = std::numeric_limits<long long>::max();

    for (const std::unique_ptr<TaskHandle>& taskStorage : tasks) {
        TaskPtr task = taskStorage.get();
        if (task == nullptr || task->state != TASK_SLEEPING) {
            continue;
        }

        long long delay = task->wakeAtMillis - now;
        if (delay < 0) {
            delay = 0;
        }
        if (delay < best) {
            best = delay;
        }
    }

    if (best == std::numeric_limits<long long>::max()) {
        return -1;
    }

    return best;
}

std::string VM::buildTaskTrace(TaskPtr task, const std::string& message) const {
    std::string trace = message;
    if (task == nullptr) {
        return trace;
    }

    for (int index = task->frameCount - 1; index >= 0; --index) {
        const CallFrame& frame = task->frames[index];
        if (frame.function == nullptr || frame.function->chunk.lines.empty()) {
            continue;
        }

        std::size_t instruction =
            static_cast<std::size_t>(frame.ip - frame.function->chunk.code.data());
        if (instruction > 0) {
            instruction--;
        }

        if (instruction >= frame.function->chunk.lines.size()) {
            continue;
        }

        trace += "\n[line " + std::to_string(frame.function->chunk.lines[instruction]) + "] in ";
        if (frame.function->name.empty()) {
            trace += "script";
        } else {
            trace += frame.function->name + "()";
        }
    }

    return trace;
}

void VM::push(const Value& value) {
    if (currentTask == nullptr) {
        throw std::runtime_error("No active task.");
    }

    std::size_t usedSlots =
        static_cast<std::size_t>(currentTask->stackTop - currentTask->stack.data());
    if (!ensureTaskStackCapacity(currentTask, usedSlots + 1)) {
        throw std::runtime_error("VM stack growth failed.");
    }

    *currentTask->stackTop = value;
    currentTask->stackTop++;
}

Value VM::pop() {
    if (currentTask == nullptr || currentTask->stackTop == currentTask->stack.data()) {
        throw std::runtime_error("VM stack underflow.");
    }

    currentTask->stackTop--;
    return *currentTask->stackTop;
}

Value VM::peek(int distance) const {
    if (currentTask == nullptr ||
        distance < 0 ||
        currentTask->stackTop - currentTask->stack.data() <= distance) {
        throw std::runtime_error("VM stack underflow.");
    }

    return currentTask->stackTop[-1 - distance];
}

InterpretResult VM::interpret(const FunctionPtr& function) {
    if (function == nullptr) {
        return INTERPRET_COMPILE_ERROR;
    }

    optimizeFunctionTree(function);

    resetScheduler();
    ClosurePtr closure = uraniumHeap().allocateClosure(function);
    rootTask = createTaskHandle(function->name.empty() ? "script" : function->name);
    if (!ensureFrameCapacity(rootTask, 1)) {
        return INTERPRET_RUNTIME_ERROR;
    }
    rootTask->observed = true;
    rootTask->frames[0].closure = closure;
    rootTask->frames[0].function = function;
    rootTask->frames[0].ip = function->chunk.code.data();
    rootTask->frames[0].slots = rootTask->stack.data();
    rootTask->frames[0].base = rootTask->stack.data();
    rootTask->frameCount = 1;
    enqueueReadyTask(rootTask);
    maybeCollectGarbage();

    return run();
}

InterpretResult VM::interpret(const char* source) {
    FunctionPtr function;
    if (!compile(source, &function)) {
        return INTERPRET_COMPILE_ERROR;
    }

    return interpret(function);
}

void VM::forceGarbageCollection() {
    collectGarbage(true);
}

bool VM::jitCompile(FunctionPtr function) {
    return maybeCompileFastPath(function, true);
}

int VM::compiledFastPathCount() const {
    return fastPathCompiledCount;
}

int VM::unsupportedFastPathCount() const {
    return fastPathUnsupportedCount;
}

int VM::nativeCompiledCount() const {
    return nativeJitCompiledCount;
}

int VM::fastPathCompiledOnlyCount() const {
    return bytecodeFastPathCompiledCount;
}

void VM::setDebugTraceEnabled(bool enabled) {
    debugTraceEnabled = enabled;
}

bool VM::propagateException(const Value& exception) {
    if (currentTask == nullptr) {
        return false;
    }

    int& handlerCount = currentTask->handlerCount;
    int& frameCount = currentTask->frameCount;
    ExceptionHandler* handlers = currentTask->handlers.data();
    CallFrame* frames = currentTask->frames.data();
    Value*& stackTop = currentTask->stackTop;

    while (handlerCount > 0) {
        ExceptionHandler handler = handlers[--handlerCount];
        if (handler.frameIndex < 0) {
            continue;
        }

        while (frameCount - 1 > handler.frameIndex) {
            closeUpvalues(frames[frameCount - 1].slots);
            frameCount--;
            while (handlerCount > 0 && handlers[handlerCount - 1].frameIndex >= frameCount) {
                handlerCount--;
            }
        }

        if (handler.frameIndex >= frameCount) {
            continue;
        }

        CallFrame& frame = frames[handler.frameIndex];
        if (frame.function == nullptr ||
            handler.catchOffset >= frame.function->chunk.code.size()) {
            continue;
        }

        closeUpvalues(handler.stackLevel);
        stackTop = handler.stackLevel;
        frame.ip = frame.function->chunk.code.data() + handler.catchOffset;
        push(exception);
        return true;
    }

    return false;
}

UpvaluePtr VM::captureUpvalue(Value* local) {
    if (currentTask == nullptr) {
        return nullptr;
    }

    UpvaluePtr previous = nullptr;
    UpvaluePtr upvalue = currentTask->openUpvalues;

    while (upvalue != nullptr && upvalue->location > local) {
        previous = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != nullptr && upvalue->location == local) {
        return upvalue;
    }

    UpvaluePtr created = uraniumHeap().allocateUpvalue(local);
    created->next = upvalue;

    if (previous == nullptr) {
        currentTask->openUpvalues = created;
    } else {
        previous->next = created;
    }

    return created;
}

void VM::closeUpvalues(Value* last) {
    if (currentTask == nullptr) {
        return;
    }

    while (currentTask->openUpvalues != nullptr && currentTask->openUpvalues->location >= last) {
        currentTask->openUpvalues->closed = *currentTask->openUpvalues->location;
        writeBarrier(currentTask->openUpvalues, currentTask->openUpvalues->closed);
        currentTask->openUpvalues->location = &currentTask->openUpvalues->closed;
        currentTask->openUpvalues = currentTask->openUpvalues->next;
    }
}

bool VM::maybeCompileFastPath(FunctionPtr function, bool force) {
    if (function == nullptr) {
        return false;
    }

    optimizeFunctionTree(function);

    if (function->fastPathStatus == FASTPATH_COMPILED && function->fastPath != nullptr) {
        return true;
    }

    if (function->fastPathStatus == FASTPATH_COMPILED && function->nativeJitEntry != nullptr) {
        return true;
    }

    if (function->fastPathStatus == FASTPATH_UNSUPPORTED) {
        return false;
    }

    if (!force) {
        function->callCount++;
        if (function->callCount < kFastPathHotThreshold) {
            return false;
        }
    }

    FastPathPlan plan;
    std::string reason;
    if (!buildFastPathPlan(function, &plan, &reason)) {
        function->jitBackend = JIT_BACKEND_NONE;
        function->fastPathStatus = FASTPATH_UNSUPPORTED;
        fastPathUnsupportedCount++;
        return false;
    }

    NativeJitArtifact nativeArtifact;
    std::string nativeReason;
    if (compileNativeJit(function, plan, &nativeArtifact, &nativeReason)) {
        function->fastPath.reset();
        function->nativeJitEntry = nativeArtifact.entry;
        function->nativeJitSize = nativeArtifact.size;
        function->nativeJitRegion = std::move(nativeArtifact.region);
        function->jitBackend = JIT_BACKEND_NATIVE;
        function->fastPathStatus = FASTPATH_COMPILED;
        fastPathCompiledCount++;
        nativeJitCompiledCount++;
        return true;
    }

    function->fastPath = std::make_unique<FastPathPlan>(std::move(plan));
    function->nativeJitEntry = nullptr;
    function->nativeJitSize = 0;
    function->nativeJitRegion.reset();
    function->jitBackend = JIT_BACKEND_FASTPATH;
    function->fastPathStatus = FASTPATH_COMPILED;
    fastPathCompiledCount++;
    bytecodeFastPathCompiledCount++;
    return true;
}

bool VM::executeNativeJit(FunctionPtr function,
                          const Value* initialSlots,
                          int initialSlotCount,
                          Value* result) {
    if (function == nullptr || function->nativeJitEntry == nullptr || result == nullptr) {
        return false;
    }

    if (initialSlotCount < function->arity) {
        return false;
    }

    std::vector<double> numericArgs(static_cast<std::size_t>(function->arity));
    for (int index = 0; index < function->arity; ++index) {
        if (!initialSlots[index].isNumber()) {
            return false;
        }
        numericArgs[static_cast<std::size_t>(index)] = initialSlots[index].asNumber();
    }

    using NativeEntry = int (*)(const double* args, double* resultNumber);
    auto entry = reinterpret_cast<NativeEntry>(function->nativeJitEntry);
    double numericResult = 0.0;
    int code = entry(numericArgs.data(), &numericResult);

    switch (code) {
        case 1:
            *result = Value::numberValue(numericResult);
            return true;
        case 2:
            *result = Value::boolValue(false);
            return true;
        case 3:
            *result = Value::boolValue(true);
            return true;
        default:
            return false;
    }
}

bool VM::executeFastPath(FunctionPtr function,
                         const Value* initialSlots,
                         int initialSlotCount,
                         Value* result) {
    if (function == nullptr || function->fastPath == nullptr || result == nullptr) {
        return false;
    }

    const FastPathPlan& plan = *function->fastPath;
    std::size_t slotCapacity =
        std::max<std::size_t>(
            static_cast<std::size_t>(std::max<int>(plan.localCount, initialSlotCount)),
            static_cast<std::size_t>(initialSlotCount)) +
        static_cast<std::size_t>(plan.maxStack) + 8;
    std::vector<Value> slotStack(slotCapacity);
    int stackTop = 0;
    for (int index = 0; index < initialSlotCount; ++index) {
        slotStack[stackTop++] = initialSlots[index];
    }

    auto pushFast = [&](const Value& value) -> bool {
        if (stackTop >= static_cast<int>(slotStack.size())) {
            slotStack.resize(slotStack.size() * 2 + 8);
        }

        slotStack[stackTop++] = value;
        return true;
    };

    auto popFast = [&](Value* value) -> bool {
        if (stackTop <= 0) {
            runtimeError("Fast-path stack underflow.");
            return false;
        }

        *value = slotStack[--stackTop];
        return true;
    };

    for (std::size_t pc = 0; pc < plan.instructions.size();) {
        const FastPathInstruction& instruction = plan.instructions[pc];
        switch (instruction.op) {
            case FASTPATH_CONSTANT:
                if (instruction.operand >= function->chunk.constants.values.size()) {
                    runtimeError("Fast-path constant index out of range.");
                    return false;
                }
                if (!pushFast(function->chunk.constants.values[instruction.operand])) {
                    return false;
                }
                break;
            case FASTPATH_NIL:
                if (!pushFast(Value::nilValue())) {
                    return false;
                }
                break;
            case FASTPATH_TRUE:
                if (!pushFast(Value::boolValue(true))) {
                    return false;
                }
                break;
            case FASTPATH_FALSE:
                if (!pushFast(Value::boolValue(false))) {
                    return false;
                }
                break;
            case FASTPATH_GET_LOCAL:
                if (instruction.operand >= stackTop) {
                    runtimeError("Fast-path local read out of range.");
                    return false;
                }
                if (!pushFast(slotStack[instruction.operand])) {
                    return false;
                }
                break;
            case FASTPATH_SET_LOCAL:
                if (instruction.operand >= stackTop) {
                    runtimeError("Fast-path local write out of range.");
                    return false;
                }
                if (stackTop <= 0) {
                    runtimeError("Fast-path local write requires a value.");
                    return false;
                }
                slotStack[instruction.operand] = slotStack[stackTop - 1];
                break;
            case FASTPATH_ADD: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }

                if (left.isNumber() && right.isNumber()) {
                    if (!pushFast(Value::numberValue(left.asNumber() + right.asNumber()))) {
                        return false;
                    }
                    break;
                }

                if (left.isString() && right.isString()) {
                    if (!pushFast(Value::stringValue(left.asString() + right.asString()))) {
                        return false;
                    }
                    break;
                }

                runtimeError("Operands to '+' must both be numbers or both be strings.");
                return false;
            }
            case FASTPATH_SUBTRACT: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!left.isNumber() || !right.isNumber()) {
                    runtimeError("Operands to '-' must be numbers.");
                    return false;
                }
                if (!pushFast(Value::numberValue(left.asNumber() - right.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_MULTIPLY: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!left.isNumber() || !right.isNumber()) {
                    runtimeError("Operands to '*' must be numbers.");
                    return false;
                }
                if (!pushFast(Value::numberValue(left.asNumber() * right.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_DIVIDE: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!left.isNumber() || !right.isNumber()) {
                    runtimeError("Operands to '/' must be numbers.");
                    return false;
                }
                if (!pushFast(Value::numberValue(left.asNumber() / right.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_NOT: {
                Value value;
                if (!popFast(&value)) {
                    return false;
                }
                if (!pushFast(Value::boolValue(isFalsey(value)))) {
                    return false;
                }
                break;
            }
            case FASTPATH_EQUAL: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!pushFast(Value::boolValue(valuesEqual(left, right)))) {
                    return false;
                }
                break;
            }
            case FASTPATH_GREATER: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!left.isNumber() || !right.isNumber()) {
                    runtimeError("Operands to comparison must be numbers.");
                    return false;
                }
                if (!pushFast(Value::boolValue(left.asNumber() > right.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_LESS: {
                Value right;
                Value left;
                if (!popFast(&right) || !popFast(&left)) {
                    return false;
                }
                if (!left.isNumber() || !right.isNumber()) {
                    runtimeError("Operands to comparison must be numbers.");
                    return false;
                }
                if (!pushFast(Value::boolValue(left.asNumber() < right.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_NEGATE: {
                Value value;
                if (!popFast(&value)) {
                    return false;
                }
                if (!value.isNumber()) {
                    runtimeError("Operand to unary '-' must be a number.");
                    return false;
                }
                if (!pushFast(Value::numberValue(-value.asNumber()))) {
                    return false;
                }
                break;
            }
            case FASTPATH_POP:
                if (stackTop <= 0) {
                    runtimeError("Fast-path pop underflow.");
                    return false;
                }
                stackTop--;
                break;
            case FASTPATH_JUMP:
            case FASTPATH_LOOP:
                if (instruction.operand >= plan.instructions.size()) {
                    runtimeError("Fast-path jump target out of range.");
                    return false;
                }
                pc = instruction.operand;
                continue;
            case FASTPATH_JUMP_IF_FALSE:
                if (stackTop <= 0) {
                    runtimeError("Fast-path conditional jump requires a value.");
                    return false;
                }
                if (instruction.operand >= plan.instructions.size()) {
                    runtimeError("Fast-path conditional jump target out of range.");
                    return false;
                }
                if (isFalsey(slotStack[stackTop - 1])) {
                    pc = instruction.operand;
                    continue;
                }
                break;
            case FASTPATH_RETURN:
                return popFast(result);
        }
        pc++;
    }

    runtimeError("Fast-path function exited without returning a value.");
    return false;
}

bool VM::call(const ClosurePtr& closure, int argCount) {
    if (currentTask == nullptr) {
        runtimeError("No active task to receive a function call.");
        return false;
    }

    if (closure == nullptr || closure->function == nullptr) {
        runtimeError("Attempted to call an invalid function.");
        return false;
    }

    ClosurePtr executableClosure = closure;
    FunctionPtr function = closure->function;
    if (function != nullptr) {
        FunctionPtr specialized =
            maybeSpecializeFunction(function, argCount, currentTask->stackTop - argCount);
        if (specialized != nullptr && specialized != function) {
            executableClosure = uraniumHeap().allocateClosure(specialized);
            executableClosure->upvalues = closure->upvalues;
            function = specialized;
        }
    }

    std::string functionName =
        function == nullptr || function->name.empty() ? "<script>" : function->name;
    if (!validateFunctionArguments(function, argCount, currentTask->stackTop - argCount,
                                   functionName)) {
        return false;
    }

    maybeCompileFastPath(function, false);
    if (executableClosure->upvalues.empty() &&
        function->fastPathStatus == FASTPATH_COMPILED &&
        function->jitBackend == JIT_BACKEND_NATIVE &&
        function->nativeJitEntry != nullptr) {
        Value result;
        if (executeNativeJit(function, currentTask->stackTop - argCount, argCount, &result)) {
            if (!validateReturnValue(function, result, functionName)) {
                return false;
            }
            currentTask->stackTop -= (argCount + 1);
            push(result);
            maybeCollectGarbage();
            return true;
        }
    }

    if (executableClosure->upvalues.empty() &&
        function->fastPathStatus == FASTPATH_COMPILED &&
        function->jitBackend == JIT_BACKEND_FASTPATH &&
        function->fastPath != nullptr) {
        Value result;
        if (!executeFastPath(function, currentTask->stackTop - argCount, argCount, &result)) {
            return false;
        }
        if (!validateReturnValue(function, result, functionName)) {
            return false;
        }

        currentTask->stackTop -= (argCount + 1);
        push(result);
        maybeCollectGarbage();
        return true;
    }

    if (!ensureFrameCapacity(currentTask, currentTask->frameCount + 1)) {
        return false;
    }

    CallFrame& frame = currentTask->frames[static_cast<std::size_t>(currentTask->frameCount++)];
    frame.closure = executableClosure;
    frame.function = function;
    frame.ip = function->chunk.code.data();
    frame.slots = currentTask->stackTop - argCount;
    frame.base = frame.slots - 1;
    return true;
}

bool VM::callNative(const NativeFunctionPtr& function, int argCount) {
    if (function == nullptr) {
        runtimeError("Attempted to call an invalid native function.");
        return false;
    }

    if (argCount != function->arity) {
        runtimeError(
            "Native function '" + function->name + "' expected " +
            std::to_string(function->arity) + " argument(s) but got " +
            std::to_string(argCount) + "."
        );
        return false;
    }

    std::string errorMessage;
    Value result = function->callback(argCount, currentTask->stackTop - argCount, &errorMessage);
    if (!errorMessage.empty()) {
        runtimeError(errorMessage);
        return false;
    }

    currentTask->stackTop -= (argCount + 1);
    push(result);
    maybeCollectGarbage();
    return true;
}

bool VM::rewriteCallSlice(int providedArgCount,
                          int finalArgCount,
                          int minArgCount,
                          const std::vector<std::string>& parameterNames,
                          const std::vector<std::string>* providedArgNames,
                          const std::string& callableName) {
    if (currentTask == nullptr) {
        runtimeError("No active task for argument rewriting.");
        return false;
    }

    if (providedArgCount < 0 || finalArgCount < 0 || minArgCount < 0) {
        runtimeError("Invalid argument counts encountered during call preparation.");
        return false;
    }

    if (providedArgCount > finalArgCount) {
        runtimeError(
            "Callable '" + callableName + "' expected at most " +
            std::to_string(finalArgCount) + " argument(s) but got " +
            std::to_string(providedArgCount) + "."
        );
        return false;
    }

    if (providedArgCount < minArgCount && providedArgNames == nullptr) {
        runtimeError(
            "Callable '" + callableName + "' expected at least " +
            std::to_string(minArgCount) + " argument(s) but got " +
            std::to_string(providedArgCount) + "."
        );
        return false;
    }

    std::size_t usedSlots =
        static_cast<std::size_t>(currentTask->stackTop - currentTask->stack.data());
    if (usedSlots < static_cast<std::size_t>(providedArgCount + 1)) {
        runtimeError("Call stack underflow while preparing arguments.");
        return false;
    }

    std::size_t baseIndex = usedSlots - static_cast<std::size_t>(providedArgCount + 1);
    std::vector<Value> providedValues(static_cast<std::size_t>(providedArgCount));
    for (int index = 0; index < providedArgCount; ++index) {
        providedValues[static_cast<std::size_t>(index)] =
            currentTask->stack[baseIndex + 1 + static_cast<std::size_t>(index)];
    }

    std::vector<Value> finalValues(static_cast<std::size_t>(finalArgCount), Value::unsetValue());
    int nextPositional = 0;
    for (int index = 0; index < providedArgCount; ++index) {
        const std::string providedName =
            providedArgNames == nullptr ? std::string() : (*providedArgNames)[static_cast<std::size_t>(index)];

        if (providedName.empty()) {
            while (nextPositional < finalArgCount &&
                   !finalValues[static_cast<std::size_t>(nextPositional)].isUnset()) {
                nextPositional++;
            }

            if (nextPositional >= finalArgCount) {
                runtimeError(
                    "Callable '" + callableName + "' received too many positional arguments.");
                return false;
            }

            finalValues[static_cast<std::size_t>(nextPositional)] =
                providedValues[static_cast<std::size_t>(index)];
            nextPositional++;
            continue;
        }

        bool found = false;
        for (int paramIndex = 0; paramIndex < finalArgCount; ++paramIndex) {
            if (paramIndex >= static_cast<int>(parameterNames.size()) ||
                parameterNames[static_cast<std::size_t>(paramIndex)] != providedName) {
                continue;
            }

            if (!finalValues[static_cast<std::size_t>(paramIndex)].isUnset()) {
                runtimeError(
                    "Callable '" + callableName + "' received duplicate argument '" +
                    providedName + "'.");
                return false;
            }

            finalValues[static_cast<std::size_t>(paramIndex)] =
                providedValues[static_cast<std::size_t>(index)];
            found = true;
            break;
        }

        if (!found) {
            runtimeError(
                "Callable '" + callableName + "' has no parameter named '" +
                providedName + "'.");
            return false;
        }
    }

    for (int index = 0; index < minArgCount; ++index) {
        if (!finalValues[static_cast<std::size_t>(index)].isUnset()) {
            continue;
        }

        std::string missingName =
            index < static_cast<int>(parameterNames.size())
                ? parameterNames[static_cast<std::size_t>(index)]
                : ("#" + std::to_string(index + 1));
        runtimeError(
            "Callable '" + callableName + "' is missing required argument '" +
            missingName + "'.");
        return false;
    }

    if (!ensureTaskStackCapacity(currentTask, baseIndex + 1 + static_cast<std::size_t>(finalArgCount))) {
        return false;
    }

    Value* stackBase = currentTask->stack.data();
    for (int index = 0; index < finalArgCount; ++index) {
        stackBase[baseIndex + 1 + static_cast<std::size_t>(index)] =
            finalValues[static_cast<std::size_t>(index)];
    }

    currentTask->stackTop = stackBase + baseIndex + 1 + finalArgCount;
    return true;
}

bool VM::prepareFunctionArguments(const FunctionPtr& function,
                                  int providedArgCount,
                                  const std::vector<std::string>* providedArgNames,
                                  const std::string& callableName,
                                  int* actualArgCount) {
    if (function == nullptr) {
        runtimeError("Attempted to prepare arguments for an invalid function.");
        return false;
    }

    int visibleArity = function->arity - (function->hasReceiverSlot ? 1 : 0);
    int visibleMinArity = function->minArity - (function->hasReceiverSlot ? 1 : 0);
    if (visibleArity < 0 || visibleMinArity < 0) {
        runtimeError("Function metadata is inconsistent.");
        return false;
    }

    if (!rewriteCallSlice(providedArgCount, visibleArity, visibleMinArity,
                          function->parameterNames, providedArgNames, callableName)) {
        return false;
    }

    if (actualArgCount != nullptr) {
        *actualArgCount = visibleArity;
    }
    return true;
}

bool VM::validateFunctionArguments(const FunctionPtr& function,
                                   int visibleArgCount,
                                   const Value* visibleArgs,
                                   const std::string& callableName) {
    if (function == nullptr || visibleArgs == nullptr) {
        return false;
    }

    std::size_t count =
        std::min<std::size_t>(static_cast<std::size_t>(visibleArgCount),
                              function->parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        std::string expected = normalizeTypeAnnotation(function->parameterTypes[index]);
        if (!isConcreteTypeAnnotation(expected)) {
            continue;
        }

        if (!valueMatchesTypeAnnotation(visibleArgs[index], expected)) {
            runtimeError(
                "Function '" + callableName + "' expected argument " +
                std::to_string(index + 1) + " to be '" + expected + "' but got '" +
                runtimeTypeName(visibleArgs[index]) + "'.");
            return false;
        }
    }

    return true;
}

bool VM::validateReturnValue(const FunctionPtr& function,
                             const Value& value,
                             const std::string& callableName) {
    if (function == nullptr) {
        return false;
    }

    std::string expected = normalizeTypeAnnotation(function->returnType);
    if (!isConcreteTypeAnnotation(expected)) {
        return true;
    }

    if (valueMatchesTypeAnnotation(value, expected)) {
        return true;
    }

    runtimeError(
        "Function '" + callableName + "' declared return type '" + expected +
        "' but returned '" + runtimeTypeName(value) + "'.");
    return false;
}

FunctionPtr VM::maybeSpecializeFunction(const FunctionPtr& function,
                                        int visibleArgCount,
                                        const Value* visibleArgs) {
    if (function == nullptr || visibleArgs == nullptr ||
        function->genericParameters.empty() ||
        visibleArgCount < 0 ||
        static_cast<std::size_t>(visibleArgCount) != function->parameterTypes.size()) {
        return function;
    }

    std::vector<std::string> argumentTypes(static_cast<std::size_t>(visibleArgCount));
    for (int index = 0; index < visibleArgCount; ++index) {
        argumentTypes[static_cast<std::size_t>(index)] = runtimeTypeName(visibleArgs[index]);
    }

    std::unordered_map<std::string, std::string> bindings;
    if (!inferTypeBindings(function->genericParameters, function->parameterTypes,
                           argumentTypes, &bindings) ||
        bindings.empty()) {
        return function;
    }

    std::string specializationKey;
    std::vector<std::string> specializationTypes;
    specializationTypes.reserve(function->genericParameters.size());
    for (const std::string& generic : function->genericParameters) {
        std::string concrete = "Any";
        auto it = bindings.find(generic);
        if (it != bindings.end()) {
            concrete = normalizeTypeAnnotation(it->second);
        }

        if (!specializationKey.empty()) {
            specializationKey.push_back('|');
        }
        specializationKey += generic;
        specializationKey.push_back('=');
        specializationKey += concrete;
        specializationTypes.push_back(concrete);
    }

    for (std::size_t index = 0; index < function->specializationKeys.size(); ++index) {
        if (function->specializationKeys[index] == specializationKey &&
            index < function->specializations.size() &&
            function->specializations[index] != nullptr) {
            return function->specializations[index];
        }
    }

    std::string specializedName = function->name;
    specializedName += "<";
    for (std::size_t index = 0; index < specializationTypes.size(); ++index) {
        if (index > 0) {
            specializedName += ",";
        }
        specializedName += specializationTypes[index];
    }
    specializedName += ">";

    FunctionPtr specialized = uraniumHeap().allocateFunction(specializedName, function->arity);
    specialized->minArity = function->minArity;
    specialized->upvalueCount = function->upvalueCount;
    specialized->isAsync = function->isAsync;
    specialized->hasReceiverSlot = function->hasReceiverSlot;
    specialized->parameterNames = function->parameterNames;
    specialized->parameterTypes.clear();
    specialized->parameterTypes.reserve(function->parameterTypes.size());
    for (const std::string& parameterType : function->parameterTypes) {
        specialized->parameterTypes.push_back(applyTypeBindings(parameterType, bindings));
    }
    specialized->returnType = applyTypeBindings(function->returnType, bindings);
    specialized->genericParameters.clear();
    specialized->chunk = function->chunk;
    specialized->optimized = false;
    specialized->callCount = 0;
    specialized->fastPathStatus = FASTPATH_UNCHECKED;
    specialized->jitBackend = JIT_BACKEND_NONE;
    specialized->fastPath.reset();
    specialized->nativeJitEntry = nullptr;
    specialized->nativeJitSize = 0;
    specialized->nativeJitRegion.reset();
    specialized->isGenericSpecialization = true;
    specialized->genericSource = function;
    specialized->specializationTypes = specializationTypes;

    function->specializationKeys.push_back(specializationKey);
    function->specializations.push_back(specialized);
    writeBarrier(function, Value::functionValue(specialized));
    return specialized;
}

bool VM::callNamedValue(const Value& callee,
                        int argCount,
                        const std::vector<std::string>& argNames) {
    return callValue(callee, argCount, &argNames);
}

bool VM::callValue(const Value& callee,
                   int argCount,
                   const std::vector<std::string>* providedArgNames) {
    if (callee.isClosure()) {
        const FunctionPtr& function = callee.asClosure() == nullptr
                                          ? nullptr
                                          : callee.asClosure()->function;
        int actualArgCount = argCount;
        std::string functionName =
            function == nullptr || function->name.empty() ? "<script>" : function->name;
        if (!prepareFunctionArguments(function, argCount, providedArgNames,
                                      functionName, &actualArgCount)) {
            return false;
        }

        ClosurePtr closure = callee.asClosure();
        FunctionPtr executableFunction =
            maybeSpecializeFunction(function, actualArgCount, currentTask->stackTop - actualArgCount);
        if (closure != nullptr && executableFunction != nullptr &&
            executableFunction != function) {
            ClosurePtr specializedClosure = uraniumHeap().allocateClosure(executableFunction);
            specializedClosure->upvalues = closure->upvalues;
            closure = specializedClosure;
            functionName =
                executableFunction->name.empty() ? functionName : executableFunction->name;
        }

        if (!validateFunctionArguments(executableFunction, actualArgCount,
                                       currentTask->stackTop - actualArgCount,
                                       functionName)) {
            return false;
        }

        if (closure != nullptr &&
            executableFunction != nullptr &&
            executableFunction->isAsync) {
            TaskPtr task = createAsyncClosureTask(closure, actualArgCount,
                                                  currentTask->stackTop - actualArgCount);
            if (task == nullptr) {
                return false;
            }

            currentTask->stackTop -= (actualArgCount + 1);
            push(Value::taskValue(task));
            return true;
        }
        return call(closure, actualArgCount);
    }

    if (callee.isFunction()) {
        const FunctionPtr& function = callee.asFunction();
        if (function == nullptr) {
            runtimeError("Attempted to call an invalid function.");
            return false;
        }

        int actualArgCount = argCount;
        std::string functionName = function->name.empty() ? "<script>" : function->name;
        if (!prepareFunctionArguments(function, argCount, providedArgNames,
                                      functionName, &actualArgCount)) {
            return false;
        }

        FunctionPtr executableFunction =
            maybeSpecializeFunction(function, actualArgCount, currentTask->stackTop - actualArgCount);
        if (!validateFunctionArguments(executableFunction, actualArgCount,
                                       currentTask->stackTop - actualArgCount,
                                       functionName)) {
            return false;
        }

        if (executableFunction->upvalueCount != 0) {
            runtimeError("Cannot call a raw function object that still expects closures.");
            return false;
        }

        ClosurePtr closure = uraniumHeap().allocateClosure(executableFunction);
        if (executableFunction->isAsync) {
            TaskPtr task = createAsyncClosureTask(closure, actualArgCount,
                                                  currentTask->stackTop - actualArgCount);
            if (task == nullptr) {
                return false;
            }

            currentTask->stackTop -= (actualArgCount + 1);
            push(Value::taskValue(task));
            maybeCollectGarbage();
            return true;
        }

        bool called = call(closure, actualArgCount);
        if (called) {
            maybeCollectGarbage();
        }
        return called;
    }

    if (callee.isNativeFunction()) {
        if (providedArgNames != nullptr) {
            runtimeError("Named arguments are not supported for native functions.");
            return false;
        }
        return callNative(callee.asNativeFunction(), argCount);
    }

    if (callee.isBoundMethod()) {
        const BoundMethodPtr& boundMethod = callee.asBoundMethod();
        if (boundMethod == nullptr || boundMethod->method == nullptr) {
            runtimeError("Attempted to call an invalid bound method.");
            return false;
        }

        FunctionPtr methodFunction =
            boundMethod->method == nullptr ? nullptr : boundMethod->method->function;
        int actualArgCount = argCount;
        std::string methodName =
            methodFunction == nullptr || methodFunction->name.empty()
                ? "<method>"
                : methodFunction->name;
        if (!prepareFunctionArguments(methodFunction, argCount, providedArgNames,
                                      methodName, &actualArgCount)) {
            return false;
        }

        ClosurePtr executableMethod = boundMethod->method;
        FunctionPtr executableFunction =
            maybeSpecializeFunction(methodFunction, actualArgCount, currentTask->stackTop - actualArgCount);
        if (executableMethod != nullptr && executableFunction != nullptr &&
            executableFunction != methodFunction) {
            ClosurePtr specializedMethod = uraniumHeap().allocateClosure(executableFunction);
            specializedMethod->upvalues = executableMethod->upvalues;
            executableMethod = specializedMethod;
            methodName =
                executableFunction->name.empty() ? methodName : executableFunction->name;
        }

        if (!validateFunctionArguments(executableFunction, actualArgCount,
                                       currentTask->stackTop - actualArgCount,
                                       methodName)) {
            return false;
        }

        if (executableFunction != nullptr &&
            executableFunction->isAsync) {
            BoundMethodPtr specializedBoundMethod =
                uraniumHeap().allocateBoundMethod(boundMethod->receiver, executableMethod);
            TaskPtr task = createAsyncBoundMethodTask(
                specializedBoundMethod, actualArgCount, currentTask->stackTop - actualArgCount);
            if (task == nullptr) {
                return false;
            }

            currentTask->stackTop -= (actualArgCount + 1);
            push(Value::taskValue(task));
            return true;
        }

        Value* calleeSlot = currentTask->stackTop - actualArgCount - 1;
        *calleeSlot = boundMethod->receiver;

        maybeCompileFastPath(executableFunction, false);
        if (executableMethod->upvalues.empty() &&
            executableFunction != nullptr &&
            executableFunction->fastPathStatus == FASTPATH_COMPILED &&
            executableFunction->jitBackend == JIT_BACKEND_NATIVE &&
            executableFunction->nativeJitEntry != nullptr) {
            Value result;
            if (executeNativeJit(executableFunction, calleeSlot, actualArgCount + 1, &result)) {
                if (!validateReturnValue(executableFunction, result, methodName)) {
                    return false;
                }
                currentTask->stackTop = calleeSlot;
                push(result);
                maybeCollectGarbage();
                return true;
            }
        }

        if (executableMethod->upvalues.empty() &&
            executableFunction != nullptr &&
            executableFunction->fastPathStatus == FASTPATH_COMPILED &&
            executableFunction->jitBackend == JIT_BACKEND_FASTPATH &&
            executableFunction->fastPath != nullptr) {
            Value result;
            if (!executeFastPath(executableFunction, calleeSlot, actualArgCount + 1, &result)) {
                return false;
            }
            if (!validateReturnValue(executableFunction, result, methodName)) {
                return false;
            }

            currentTask->stackTop = calleeSlot;
            push(result);
            maybeCollectGarbage();
            return true;
        }

        if (!ensureFrameCapacity(currentTask, currentTask->frameCount + 1)) {
            return false;
        }

        CallFrame& frame =
            currentTask->frames[static_cast<std::size_t>(currentTask->frameCount++)];
        frame.closure = executableMethod;
        frame.function = executableFunction;
        frame.ip = executableFunction->chunk.code.data();
        frame.slots = calleeSlot;
        frame.base = calleeSlot;
        return true;
    }

    if (callee.isClass()) {
        const ClassPtr& klass = callee.asClass();
        if (klass == nullptr) {
            runtimeError("Attempted to instantiate an invalid class.");
            return false;
        }

        Value instanceValue =
            Value::instanceValue(uraniumHeap().allocateInstance(klass));
        Value* calleeSlot = currentTask->stackTop - argCount - 1;
        *calleeSlot = instanceValue;

        ClosurePtr initializer = findMethod(klass, "init");
        if (initializer == nullptr) {
            if (providedArgNames != nullptr) {
                runtimeError("Named arguments require an init() method on the target class.");
                return false;
            }

            if (argCount != 0) {
                runtimeError(
                    "Class '" + klass->name + "' expected 0 constructor argument(s) but got " +
                    std::to_string(argCount) + "."
                );
                return false;
            }

            currentTask->stackTop = calleeSlot + 1;
            maybeCollectGarbage();
            return true;
        }

        if (initializer->function != nullptr && initializer->function->isAsync) {
            runtimeError("Async init() is not supported.");
            return false;
        }

        FunctionPtr initializerFunction = initializer->function;
        int actualArgCount = argCount;
        if (!prepareFunctionArguments(initializerFunction, argCount, providedArgNames,
                                      klass->name + ".init", &actualArgCount)) {
            return false;
        }

        calleeSlot = currentTask->stackTop - actualArgCount - 1;
        if (!validateFunctionArguments(initializerFunction, actualArgCount,
                                       currentTask->stackTop - actualArgCount,
                                       klass->name + ".init")) {
            return false;
        }

        maybeCompileFastPath(initializerFunction, false);
        if (initializer->upvalues.empty() &&
            initializerFunction != nullptr &&
            initializerFunction->fastPathStatus == FASTPATH_COMPILED &&
            initializerFunction->jitBackend == JIT_BACKEND_NATIVE &&
            initializerFunction->nativeJitEntry != nullptr) {
            Value result;
            if (executeNativeJit(initializerFunction, calleeSlot, actualArgCount + 1, &result)) {
                currentTask->stackTop = calleeSlot;
                push(result);
                maybeCollectGarbage();
                return true;
            }
        }

        if (initializer->upvalues.empty() &&
            initializerFunction != nullptr &&
            initializerFunction->fastPathStatus == FASTPATH_COMPILED &&
            initializerFunction->jitBackend == JIT_BACKEND_FASTPATH &&
            initializerFunction->fastPath != nullptr) {
            Value result;
            if (!executeFastPath(initializerFunction, calleeSlot, actualArgCount + 1, &result)) {
                return false;
            }

            currentTask->stackTop = calleeSlot;
            push(result);
            maybeCollectGarbage();
            return true;
        }

        if (!ensureFrameCapacity(currentTask, currentTask->frameCount + 1)) {
            return false;
        }

        CallFrame& frame =
            currentTask->frames[static_cast<std::size_t>(currentTask->frameCount++)];
        frame.closure = initializer;
        frame.function = initializer->function;
        frame.ip = initializer->function->chunk.code.data();
        frame.slots = calleeSlot;
        frame.base = calleeSlot;
        maybeCollectGarbage();
        return true;
    }

    runtimeError("Only functions and classes can be called.");
    return false;
}

InterpretResult VM::runtimeError(const std::string& message) {
    std::string trace = buildTaskTrace(currentTask, message);
    if (currentTask == rootTask) {
        std::cerr << trace << std::endl;
    }

    failTask(currentTask, Value::stringValue(trace));
    return INTERPRET_RUNTIME_ERROR;
}

InterpretResult VM::runTaskSlice() {
#define READ_BYTE() (*frame->ip++)
#define READ_CONSTANT() (frame->function->chunk.constants.values[READ_BYTE()])
#define READ_SHORT() \
    static_cast<uint16_t>((static_cast<uint16_t>(READ_BYTE()) << 8) | READ_BYTE())
#define READ_CONSTANT_LONG() (frame->function->chunk.constants.values[READ_SHORT()])
#define NUMERIC_BINARY_OP(op, message) \
    do { \
        Value b = pop(); \
        Value a = pop(); \
        if (!a.isNumber() || !b.isNumber()) { \
            return runtimeError(message); \
        } \
        push(Value::numberValue(a.asNumber() op b.asNumber())); \
    } while (false)
#define NUMERIC_COMPARE_OP(op, message) \
    do { \
        Value b = pop(); \
        Value a = pop(); \
        if (!a.isNumber() || !b.isNumber()) { \
            return runtimeError(message); \
        } \
        push(Value::boolValue(a.asNumber() op b.asNumber())); \
    } while (false)

    try {
        Value*& stackTop = currentTask->stackTop;
        int& frameCount = currentTask->frameCount;
        int& handlerCount = currentTask->handlerCount;

        int instructionBudget = 512;
        while (instructionBudget-- > 0) {
            std::string loopBrokerError;
            if (!noteLoopBrokerInstruction(currentTask, &loopBrokerError)) {
                return runtimeError(loopBrokerError);
            }

            if (frameCount <= 0) {
                resolveTask(currentTask, Value::nilValue());
                return INTERPRET_OK;
            }

            ExceptionHandler* handlers = currentTask->handlers.data();
            CallFrame* frame = &currentTask->frames[static_cast<std::size_t>(frameCount - 1)];
            bool shouldTrace = debugTraceEnabled;
#ifdef DEBUG_TRACE_EXECUTION
            shouldTrace = true;
#endif
            if (shouldTrace) {
            std::cout << "          ";
            for (Value* slot = currentTask->stack.data(); slot < stackTop; slot++) {
                std::cout << "[ ";
                printValue(*slot);
                std::cout << " ]";
            }
            std::cout << std::endl;
            disassembleInstruction(&frame->function->chunk,
                static_cast<int>(frame->ip - frame->function->chunk.code.data()));
            }
            uint8_t instruction;
            switch (instruction = READ_BYTE()) {
                case OP_NOP:
                    break;
                case OP_CONSTANT: {
                    Value constant = READ_CONSTANT();
                    push(constant);
                    break;
                }
                case OP_CONSTANT_LONG: {
                    Value constant = READ_CONSTANT_LONG();
                    push(constant);
                    break;
                }
                case OP_UNSET:
                    push(Value::unsetValue());
                    break;
                case OP_NIL:
                    push(Value::nilValue());
                    break;
                case OP_TRUE:
                    push(Value::boolValue(true));
                    break;
                case OP_FALSE:
                    push(Value::boolValue(false));
                    break;
                case OP_ARRAY: {
                    uint8_t elementCount = READ_BYTE();
                    ArrayPtr array = uraniumHeap().allocateArray();
                    array->elements.resize(elementCount);

                    for (int index = static_cast<int>(elementCount) - 1; index >= 0; --index) {
                        array->elements[static_cast<std::size_t>(index)] = pop();
                    }

                    push(Value::arrayValue(array));
                    maybeCollectGarbage();
                    break;
                }
                case OP_MAP: {
                    uint8_t entryCount = READ_BYTE();
                    MapPtr map = uraniumHeap().allocateMap();
                    std::vector<std::pair<std::string, Value>> entries(entryCount);

                    for (int index = static_cast<int>(entryCount) - 1; index >= 0; --index) {
                        Value entryValue = pop();
                        Value keyValue = pop();
                        std::string key;
                        std::string errorMessage;
                        if (!valueToMapKey(keyValue, "Map literal", &key, &errorMessage)) {
                            return runtimeError(errorMessage);
                        }

                        entries[static_cast<std::size_t>(index)] =
                            std::make_pair(std::move(key), entryValue);
                    }

                    for (const auto& entry : entries) {
                        map->entries[entry.first] = entry.second;
                    }

                    push(Value::mapValue(map));
                    maybeCollectGarbage();
                    break;
                }
                case OP_CLASS: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string class name.");
                    }

                    push(Value::classValue(uraniumHeap().allocateClass(name.asString())));
                    maybeCollectGarbage();
                    break;
                }
                case OP_CLASS_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string class name.");
                    }

                    push(Value::classValue(uraniumHeap().allocateClass(name.asString())));
                    maybeCollectGarbage();
                    break;
                }
                case OP_INHERIT: {
                    Value superclassValue = pop();
                    Value subclassValue = peek(0);

                    if (!superclassValue.isClass() || superclassValue.asClass() == nullptr) {
                        return runtimeError("Superclass must be a class.");
                    }

                    if (!subclassValue.isClass() || subclassValue.asClass() == nullptr) {
                        return runtimeError("Inheritance target must be a class.");
                    }

                    subclassValue.asClass()->superclass = superclassValue.asClass();
                    writeBarrier(subclassValue.asClass(),
                                 Value::classValue(superclassValue.asClass()));
                    break;
                }
                case OP_METHOD: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string method name.");
                    }

                    Value methodValue = pop();
                    if (!methodValue.isClosure() || methodValue.asClosure() == nullptr) {
                        return runtimeError("Class methods must compile to closures.");
                    }

                    Value klassValue = peek(0);
                    if (!klassValue.isClass() || klassValue.asClass() == nullptr) {
                        return runtimeError("Method definition target is not a class.");
                    }

                    klassValue.asClass()->methods[name.asString()] = methodValue.asClosure();
                    writeBarrier(klassValue.asClass(), methodValue);
                    break;
                }
                case OP_METHOD_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string method name.");
                    }

                    Value methodValue = pop();
                    if (!methodValue.isClosure() || methodValue.asClosure() == nullptr) {
                        return runtimeError("Class methods must compile to closures.");
                    }

                    Value klassValue = peek(0);
                    if (!klassValue.isClass() || klassValue.asClass() == nullptr) {
                        return runtimeError("Method definition target is not a class.");
                    }

                    klassValue.asClass()->methods[name.asString()] = methodValue.asClosure();
                    writeBarrier(klassValue.asClass(), methodValue);
                    break;
                }
                case OP_CLOSURE: {
                    Value functionValue = READ_CONSTANT();
                    if (!functionValue.isFunction() || functionValue.asFunction() == nullptr) {
                        return runtimeError("Closure constant must be a function.");
                    }

                    ClosurePtr closure = uraniumHeap().allocateClosure(functionValue.asFunction());
                    for (int index = 0; index < functionValue.asFunction()->upvalueCount; ++index) {
                        uint8_t isLocal = READ_BYTE();
                        uint8_t slot = READ_BYTE();
                        if (isLocal != 0) {
                            closure->upvalues[static_cast<std::size_t>(index)] =
                                captureUpvalue(frame->slots + slot);
                        } else {
                            if (frame->closure == nullptr ||
                                slot >= frame->closure->upvalues.size()) {
                                return runtimeError("Invalid upvalue capture.");
                            }

                            closure->upvalues[static_cast<std::size_t>(index)] =
                                frame->closure->upvalues[slot];
                        }
                    }

                    push(Value::closureValue(closure));
                    maybeCollectGarbage();
                    break;
                }
                case OP_CLOSURE_LONG: {
                    Value functionValue = READ_CONSTANT_LONG();
                    if (!functionValue.isFunction() || functionValue.asFunction() == nullptr) {
                        return runtimeError("Closure constant must be a function.");
                    }

                    ClosurePtr closure = uraniumHeap().allocateClosure(functionValue.asFunction());
                    for (int index = 0; index < functionValue.asFunction()->upvalueCount; ++index) {
                        uint8_t isLocal = READ_BYTE();
                        uint8_t slot = READ_BYTE();
                        if (isLocal != 0) {
                            closure->upvalues[static_cast<std::size_t>(index)] =
                                captureUpvalue(frame->slots + slot);
                        } else {
                            if (frame->closure == nullptr ||
                                slot >= frame->closure->upvalues.size()) {
                                return runtimeError("Invalid upvalue capture.");
                            }

                            closure->upvalues[static_cast<std::size_t>(index)] =
                                frame->closure->upvalues[slot];
                        }
                    }

                    push(Value::closureValue(closure));
                    maybeCollectGarbage();
                    break;
                }
                case OP_PUSH_EXCEPTION_HANDLER: {
                    uint16_t catchOffset = READ_SHORT();
                    if (!ensureHandlerCapacity(currentTask, handlerCount + 1)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    currentTask->handlers[static_cast<std::size_t>(handlerCount)].frameIndex =
                        frameCount - 1;
                    currentTask->handlers[static_cast<std::size_t>(handlerCount)].catchOffset =
                        catchOffset;
                    currentTask->handlers[static_cast<std::size_t>(handlerCount)].stackLevel =
                        stackTop;
                    handlerCount++;
                    break;
                }
                case OP_POP_EXCEPTION_HANDLER:
                    if (handlerCount == 0) {
                        return runtimeError("Exception handler stack underflow.");
                    }
                    handlerCount--;
                    break;
                case OP_THROW: {
                    Value exception = pop();
                    if (!propagateException(exception)) {
                        failTask(currentTask, exception);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    break;
                }
                case OP_AWAIT: {
                    Value awaitedValue = pop();
                    if (!awaitedValue.isTask() || awaitedValue.asTask() == nullptr) {
                        push(awaitedValue);
                        break;
                    }

                    TaskPtr awaitedTask = awaitedValue.asTask();
                    awaitedTask->observed = true;

                    if (awaitedTask == currentTask) {
                        return runtimeError("A task cannot await itself.");
                    }

                    if (awaitedTask->state == TASK_COMPLETED) {
                        push(awaitedTask->result);
                        break;
                    }

                    if (awaitedTask->state == TASK_FAILED) {
                        if (!propagateException(awaitedTask->failure)) {
                            failTask(currentTask, awaitedTask->failure);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        break;
                    }

                    currentTask->state = TASK_WAITING;
                    currentTask->waitingOn = awaitedTask;
                    awaitedTask->awaiters.push_back(currentTask);
                    return INTERPRET_OK;
                }
                case OP_POP:
                    pop();
                    break;
                case OP_CLOSE_UPVALUE:
                    closeUpvalues(stackTop - 1);
                    pop();
                    break;
                case OP_DEFINE_GLOBAL: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    globals[name.asString()] = pop();
                    break;
                }
                case OP_DEFINE_CONST_GLOBAL: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    globals[name.asString()] = pop();
                    constantGlobals.insert(name.asString());
                    break;
                }
                case OP_DEFINE_GLOBAL_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    globals[name.asString()] = pop();
                    break;
                }
                case OP_DEFINE_CONST_GLOBAL_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    globals[name.asString()] = pop();
                    constantGlobals.insert(name.asString());
                    break;
                }
                case OP_GET_GLOBAL: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    auto it = globals.find(name.asString());
                    if (it == globals.end()) {
                        return runtimeError("Undefined variable '" + name.asString() + "'.");
                    }

                    push(it->second);
                    break;
                }
                case OP_GET_GLOBAL_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    auto it = globals.find(name.asString());
                    if (it == globals.end()) {
                        return runtimeError("Undefined variable '" + name.asString() + "'.");
                    }

                    push(it->second);
                    break;
                }
                case OP_SET_GLOBAL: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    auto it = globals.find(name.asString());
                    if (it == globals.end()) {
                        return runtimeError("Undefined variable '" + name.asString() + "'.");
                    }

                    if (constantGlobals.find(name.asString()) != constantGlobals.end()) {
                        return runtimeError("Cannot assign to const global '" + name.asString() + "'.");
                    }

                    it->second = peek(0);
                    break;
                }
                case OP_SET_GLOBAL_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string global name.");
                    }

                    auto it = globals.find(name.asString());
                    if (it == globals.end()) {
                        return runtimeError("Undefined variable '" + name.asString() + "'.");
                    }

                    if (constantGlobals.find(name.asString()) != constantGlobals.end()) {
                        return runtimeError("Cannot assign to const global '" + name.asString() + "'.");
                    }

                    it->second = peek(0);
                    break;
                }
                case OP_GET_LOCAL: {
                    uint8_t slot = READ_BYTE();
                    push(frame->slots[slot]);
                    break;
                }
                case OP_SET_LOCAL: {
                    uint8_t slot = READ_BYTE();
                    frame->slots[slot] = peek(0);
                    break;
                }
                case OP_GET_UPVALUE: {
                    uint8_t slot = READ_BYTE();
                    if (frame->closure == nullptr ||
                        slot >= frame->closure->upvalues.size() ||
                        frame->closure->upvalues[slot] == nullptr ||
                        frame->closure->upvalues[slot]->location == nullptr) {
                        return runtimeError("Invalid upvalue read.");
                    }

                    push(*frame->closure->upvalues[slot]->location);
                    break;
                }
                case OP_SET_UPVALUE: {
                    uint8_t slot = READ_BYTE();
                    if (frame->closure == nullptr ||
                        slot >= frame->closure->upvalues.size() ||
                        frame->closure->upvalues[slot] == nullptr ||
                        frame->closure->upvalues[slot]->location == nullptr) {
                        return runtimeError("Invalid upvalue write.");
                    }

                    *frame->closure->upvalues[slot]->location = peek(0);
                    writeBarrier(frame->closure->upvalues[slot], peek(0));
                    break;
                }
                case OP_GET_INDEX: {
                    Value indexValue = pop();
                    Value receiver = pop();
                    Value result;
                    std::string errorMessage;
                    if (!getIndexedValue(receiver, indexValue, &result, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(result);
                    break;
                }
                case OP_SET_INDEX: {
                    Value assignedValue = pop();
                    Value indexValue = pop();
                    Value receiver = pop();
                    std::string errorMessage;
                    if (!setIndexedValue(receiver, indexValue, assignedValue, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(assignedValue);
                    break;
                }
                case OP_GET_PROPERTY: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string property name.");
                    }

                    Value receiver = pop();
                    Value result;
                    std::string errorMessage;
                    if (!getPropertyValue(receiver, name.asString(), &result, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(result);
                    maybeCollectGarbage();
                    break;
                }
                case OP_GET_PROPERTY_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string property name.");
                    }

                    Value receiver = pop();
                    Value result;
                    std::string errorMessage;
                    if (!getPropertyValue(receiver, name.asString(), &result, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(result);
                    maybeCollectGarbage();
                    break;
                }
                case OP_GET_SUPER: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string superclass method name.");
                    }

                    Value receiver = pop();
                    Value superclassValue = pop();

                    if (!superclassValue.isClass() || superclassValue.asClass() == nullptr) {
                        return runtimeError("super lookup requires a superclass.");
                    }

                    ClosurePtr method = findMethod(superclassValue.asClass(), name.asString());
                    if (method == nullptr) {
                        return runtimeError(
                            "Undefined superclass method '" + name.asString() + "'.");
                    }

                    push(Value::boundMethodValue(
                        uraniumHeap().allocateBoundMethod(receiver, method)));
                    maybeCollectGarbage();
                    break;
                }
                case OP_GET_SUPER_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string superclass method name.");
                    }

                    Value receiver = pop();
                    Value superclassValue = pop();

                    if (!superclassValue.isClass() || superclassValue.asClass() == nullptr) {
                        return runtimeError("super lookup requires a superclass.");
                    }

                    ClosurePtr method = findMethod(superclassValue.asClass(), name.asString());
                    if (method == nullptr) {
                        return runtimeError(
                            "Undefined superclass method '" + name.asString() + "'.");
                    }

                    push(Value::boundMethodValue(
                        uraniumHeap().allocateBoundMethod(receiver, method)));
                    maybeCollectGarbage();
                    break;
                }
                case OP_SET_PROPERTY: {
                    Value name = READ_CONSTANT();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string property name.");
                    }

                    Value assignedValue = pop();
                    Value receiver = pop();
                    std::string errorMessage;
                    if (!setPropertyValue(receiver, name.asString(), assignedValue, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(assignedValue);
                    break;
                }
                case OP_SET_PROPERTY_LONG: {
                    Value name = READ_CONSTANT_LONG();
                    if (!name.isString()) {
                        return runtimeError("Compiler emitted a non-string property name.");
                    }

                    Value assignedValue = pop();
                    Value receiver = pop();
                    std::string errorMessage;
                    if (!setPropertyValue(receiver, name.asString(), assignedValue, &errorMessage)) {
                        return runtimeError(errorMessage);
                    }

                    push(assignedValue);
                    break;
                }
                case OP_CALL: {
                    uint8_t argCount = READ_BYTE();
                    if (!callValue(peek(argCount), argCount)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    break;
                }
                case OP_CALL_NAMED: {
                    uint8_t argCount = READ_BYTE();
                    std::vector<std::string> argNames(static_cast<std::size_t>(argCount));
                    for (uint8_t index = 0; index < argCount; ++index) {
                        uint16_t encoded = READ_SHORT();
                        if (encoded == 0) {
                            continue;
                        }

                        std::size_t constantIndex = static_cast<std::size_t>(encoded - 1);
                        if (constantIndex >= frame->function->chunk.constants.values.size()) {
                            return runtimeError("Named call operand referenced an invalid constant.");
                        }

                        Value nameValue =
                            frame->function->chunk.constants.values[constantIndex];
                        if (!nameValue.isString()) {
                            return runtimeError("Named call operands must be strings.");
                        }

                        argNames[static_cast<std::size_t>(index)] = nameValue.asString();
                    }

                    if (!callNamedValue(peek(argCount), argCount, argNames)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    break;
                }
                case OP_ADD: {
                    Value b = pop();
                    Value a = pop();

                    if (a.isNumber() && b.isNumber()) {
                        push(Value::numberValue(a.asNumber() + b.asNumber()));
                        break;
                    }

                    if (a.isString() && b.isString()) {
                        push(Value::stringValue(a.asString() + b.asString()));
                        break;
                    }

                    return runtimeError("Operands to '+' must both be numbers or both be strings.");
                }
                case OP_SUBTRACT:
                    NUMERIC_BINARY_OP(-, "Operands to '-' must be numbers.");
                    break;
                case OP_MULTIPLY:
                    NUMERIC_BINARY_OP(*, "Operands to '*' must be numbers.");
                    break;
                case OP_DIVIDE:
                    NUMERIC_BINARY_OP(/, "Operands to '/' must be numbers.");
                    break;
                case OP_NOT:
                    push(Value::boolValue(isFalsey(pop())));
                    break;
                case OP_EQUAL: {
                    Value b = pop();
                    Value a = pop();
                    push(Value::boolValue(valuesEqual(a, b)));
                    break;
                }
                case OP_GREATER:
                    NUMERIC_COMPARE_OP(>, "Operands to comparison must be numbers.");
                    break;
                case OP_LESS:
                    NUMERIC_COMPARE_OP(<, "Operands to comparison must be numbers.");
                    break;
                case OP_JUMP: {
                    uint16_t offset = READ_SHORT();
                    frame->ip += offset;
                    break;
                }
                case OP_JUMP_IF_FALSE: {
                    uint16_t offset = READ_SHORT();
                    if (isFalsey(peek(0))) {
                        frame->ip += offset;
                    }
                    break;
                }
                case OP_LOOP: {
                    uint16_t offset = READ_SHORT();
                    std::string loopBrokerError;
                    if (!noteLoopBrokerLoop(currentTask, &loopBrokerError)) {
                        return runtimeError(loopBrokerError);
                    }
                    frame->ip -= offset;
                    break;
                }
                case OP_NEGATE: {
                    Value value = pop();
                    if (!value.isNumber()) {
                        return runtimeError("Operand to unary '-' must be a number.");
                    }

                    push(Value::numberValue(-value.asNumber()));
                    break;
                }
                case OP_PRINT: {
                    printValue(pop());
                    std::cout << std::endl;
                    break;
                }
                case OP_RETURN: {
                    Value result = pop();
                    if (frame->function != nullptr) {
                        std::string callableName =
                            frame->function->name.empty() ? "<script>" : frame->function->name;
                        if (!validateReturnValue(frame->function, result, callableName)) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    closeUpvalues(frame->slots);
                    frameCount--;
                    while (handlerCount > 0 && handlers[handlerCount - 1].frameIndex >= frameCount) {
                        handlerCount--;
                    }

                    if (frameCount == 0) {
                        resolveTask(currentTask, result);
                        return INTERPRET_OK;
                    }

                    stackTop = frame->base;
                    push(result);
                    break;
                }
            }
        }

        if (currentTask != nullptr && currentTask->state == TASK_RUNNING) {
            currentTask->state = TASK_READY;
        }
        return INTERPRET_OK;
    } catch (const std::runtime_error& error) {
        return runtimeError(error.what());
    }

#undef READ_BYTE
#undef READ_CONSTANT
#undef READ_SHORT
#undef READ_CONSTANT_LONG
#undef NUMERIC_BINARY_OP
#undef NUMERIC_COMPARE_OP
}

InterpretResult VM::run() {
    VM* previousVm = activeVm;
    activeVm = this;

    try {
        for (;;) {
            pollSleepingTasks();

            if (rootTask != nullptr && rootTask->state == TASK_FAILED) {
                activeVm = previousVm;
                return INTERPRET_RUNTIME_ERROR;
            }

            TaskPtr nextTask = nullptr;
            while (!readyQueue.empty()) {
                TaskPtr candidate = readyQueue.front();
                readyQueue.pop_front();
                if (candidate != nullptr && candidate->state == TASK_READY) {
                    nextTask = candidate;
                    break;
                }
            }

            if (nextTask == nullptr) {
                if (!hasPendingLiveTasks()) {
                    activeVm = previousVm;
                    if (rootTask != nullptr && rootTask->state == TASK_FAILED) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    return INTERPRET_OK;
                }

                long long wakeDelay = nextWakeDelayMillis();
                if (wakeDelay < 0) {
                    currentTask = rootTask;
                    activeVm = previousVm;
                    return runtimeError("Scheduler deadlock: no runnable tasks remain.");
                }

                if (wakeDelay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(wakeDelay));
                } else {
                    std::this_thread::yield();
                }
                continue;
            }

            currentTask = nextTask;
            currentTask->state = TASK_RUNNING;

            if (!applyPendingResume(currentTask)) {
                if (rootTask != nullptr && rootTask->state == TASK_FAILED) {
                    activeVm = previousVm;
                    return INTERPRET_RUNTIME_ERROR;
                }
                continue;
            }

            InterpretResult sliceResult = runTaskSlice();
            if (sliceResult == INTERPRET_RUNTIME_ERROR &&
                rootTask != nullptr &&
                rootTask->state == TASK_FAILED) {
                activeVm = previousVm;
                return INTERPRET_RUNTIME_ERROR;
            }

            if (currentTask != nullptr && currentTask->state == TASK_READY) {
                enqueueReadyTask(currentTask);
            }
        }
    } catch (const std::runtime_error& error) {
        currentTask = rootTask;
        InterpretResult result = runtimeError(error.what());
        activeVm = previousVm;
        return result;
    }
}
