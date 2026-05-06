#include "urc.h"
#include "heap.h"
#include "object.h"
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {

constexpr char kUrcMagicV1[] = {'U', 'R', 'C', '1'};
constexpr char kUrcMagicV2[] = {'U', 'R', 'C', '2'};
constexpr char kUrcMagicV3[] = {'U', 'R', 'C', '3'};
constexpr char kUrcMagicV4[] = {'U', 'R', 'C', '4'};
constexpr char kUraMagicV1[] = {'U', 'R', 'A', '1'};

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

template <typename T>
bool writePod(std::ostream& stream, const T& value, std::string* errorMessage) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!stream.good()) {
        return setError(errorMessage, "Failed while writing .urc data.");
    }
    return true;
}

template <typename T>
bool readPod(std::istream& stream, T* value, std::string* errorMessage) {
    stream.read(reinterpret_cast<char*>(value), sizeof(T));
    if (!stream.good()) {
        return setError(errorMessage, "Failed while reading .urc data.");
    }
    return true;
}

bool writeString(std::ostream& stream, const std::string& value, std::string* errorMessage) {
    uint32_t size = static_cast<uint32_t>(value.size());
    if (!writePod(stream, size, errorMessage)) {
        return false;
    }

    if (size > 0) {
        stream.write(value.data(), static_cast<std::streamsize>(size));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while writing string data to .urc.");
        }
    }
    return true;
}

bool readString(std::istream& stream, std::string* value, std::string* errorMessage) {
    uint32_t size = 0;
    if (!readPod(stream, &size, errorMessage)) {
        return false;
    }

    value->resize(size);
    if (size > 0) {
        stream.read(value->data(), static_cast<std::streamsize>(size));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while reading string data from .urc.");
        }
    }
    return true;
}

bool writeBlob(std::ostream& stream, const std::string& value, std::string* errorMessage) {
    uint32_t size = static_cast<uint32_t>(value.size());
    if (!writePod(stream, size, errorMessage)) {
        return false;
    }

    if (size > 0) {
        stream.write(value.data(), static_cast<std::streamsize>(size));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while writing binary data.");
        }
    }
    return true;
}

bool readBlob(std::istream& stream, std::string* value, std::string* errorMessage) {
    uint32_t size = 0;
    if (!readPod(stream, &size, errorMessage)) {
        return false;
    }

    value->resize(size);
    if (size > 0) {
        stream.read(value->data(), static_cast<std::streamsize>(size));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while reading binary data.");
        }
    }
    return true;
}

bool writeFunction(std::ostream& stream, const FunctionPtr& function, std::string* errorMessage);
bool readFunction(std::istream& stream,
                  bool legacyFormat,
                  bool supportsAsyncMetadata,
                  bool supportsSignatureMetadata,
                  FunctionPtr* function,
                  std::string* errorMessage);

bool writeStringVector(std::ostream& stream,
                       const std::vector<std::string>& values,
                       std::string* errorMessage) {
    uint32_t count = static_cast<uint32_t>(values.size());
    if (!writePod(stream, count, errorMessage)) {
        return false;
    }

    for (const std::string& value : values) {
        if (!writeString(stream, value, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool readStringVector(std::istream& stream,
                      std::vector<std::string>* values,
                      std::string* errorMessage) {
    uint32_t count = 0;
    if (!readPod(stream, &count, errorMessage)) {
        return false;
    }

    values->clear();
    values->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        std::string value;
        if (!readString(stream, &value, errorMessage)) {
            return false;
        }
        values->push_back(std::move(value));
    }

    return true;
}

bool writeValue(std::ostream& stream, const Value& value, std::string* errorMessage) {
    uint8_t type = static_cast<uint8_t>(value.type);
    if (!writePod(stream, type, errorMessage)) {
        return false;
    }

    switch (value.type) {
        case VAL_NIL:
            return true;
        case VAL_BOOL: {
            uint8_t rawBool = value.asBool() ? 1 : 0;
            return writePod(stream, rawBool, errorMessage);
        }
        case VAL_NUMBER:
            return writePod(stream, value.number, errorMessage);
        case VAL_STRING:
            return writeString(stream, value.string, errorMessage);
        case VAL_FUNCTION:
            return writeFunction(stream, value.function, errorMessage);
        case VAL_CLOSURE:
            return setError(errorMessage,
                            "Closures are runtime values and cannot be serialized into .urc constants.");
        case VAL_NATIVE_FUNCTION:
            return setError(errorMessage,
                            "Native functions are runtime-provided and cannot be serialized to .urc.");
        case VAL_ARRAY:
            return setError(errorMessage,
                            "Arrays are runtime values and cannot be serialized into .urc constants.");
        case VAL_MAP:
            return setError(errorMessage,
                            "Maps are runtime values and cannot be serialized into .urc constants.");
        case VAL_CLASS:
            return setError(errorMessage,
                            "Classes are runtime values and cannot be serialized into .urc constants.");
        case VAL_INSTANCE:
            return setError(errorMessage,
                            "Instances are runtime values and cannot be serialized into .urc constants.");
        case VAL_BOUND_METHOD:
            return setError(errorMessage,
                            "Bound methods are runtime values and cannot be serialized into .urc constants.");
        default:
            return setError(errorMessage, "Encountered an unknown value type while writing .urc.");
    }
}

bool readValue(std::istream& stream,
               bool legacyFormat,
               bool supportsAsyncMetadata,
               bool supportsSignatureMetadata,
               Value* value,
               std::string* errorMessage) {
    uint8_t rawType = 0;
    if (!readPod(stream, &rawType, errorMessage)) {
        return false;
    }

    ValueType type = VAL_NIL;
    if (legacyFormat) {
        switch (rawType) {
            case 0:
                type = VAL_NIL;
                break;
            case 1:
                type = VAL_BOOL;
                break;
            case 2:
                type = VAL_NUMBER;
                break;
            case 3:
                type = VAL_STRING;
                break;
            case 4:
                type = VAL_FUNCTION;
                break;
            case 5:
                type = VAL_NATIVE_FUNCTION;
                break;
            case 6:
                type = VAL_ARRAY;
                break;
            case 7:
                type = VAL_MAP;
                break;
            case 8:
                type = VAL_CLASS;
                break;
            case 9:
                type = VAL_INSTANCE;
                break;
            case 10:
                type = VAL_BOUND_METHOD;
                break;
            default:
                return setError(errorMessage,
                                "Encountered an unknown legacy value type while reading .urc.");
        }
    } else {
        type = static_cast<ValueType>(rawType);
    }

    switch (type) {
        case VAL_NIL:
            *value = Value::nilValue();
            return true;
        case VAL_BOOL: {
            uint8_t rawBool = 0;
            if (!readPod(stream, &rawBool, errorMessage)) {
                return false;
            }
            *value = Value::boolValue(rawBool != 0);
            return true;
        }
        case VAL_NUMBER: {
            double number = 0.0;
            if (!readPod(stream, &number, errorMessage)) {
                return false;
            }
            *value = Value::numberValue(number);
            return true;
        }
        case VAL_STRING: {
            std::string stringValue;
            if (!readString(stream, &stringValue, errorMessage)) {
                return false;
            }
            *value = Value::stringValue(stringValue);
            return true;
        }
        case VAL_FUNCTION: {
            FunctionPtr nestedFunction;
            if (!readFunction(stream, legacyFormat, supportsAsyncMetadata, supportsSignatureMetadata,
                              &nestedFunction, errorMessage)) {
                return false;
            }
            *value = Value::functionValue(nestedFunction);
            return true;
        }
        case VAL_CLOSURE:
            return setError(errorMessage,
                            "Closures cannot be loaded from .urc constants.");
        case VAL_NATIVE_FUNCTION:
            return setError(errorMessage,
                            "Native functions must come from the runtime and cannot be loaded from .urc.");
        case VAL_ARRAY:
            return setError(errorMessage,
                            "Arrays cannot be loaded from .urc constants.");
        case VAL_MAP:
            return setError(errorMessage,
                            "Maps cannot be loaded from .urc constants.");
        case VAL_CLASS:
            return setError(errorMessage,
                            "Classes cannot be loaded from .urc constants.");
        case VAL_INSTANCE:
            return setError(errorMessage,
                            "Instances cannot be loaded from .urc constants.");
        case VAL_BOUND_METHOD:
            return setError(errorMessage,
                            "Bound methods cannot be loaded from .urc constants.");
        default:
            return setError(errorMessage, "Encountered an unknown value type while reading .urc.");
    }
}

bool writeFunction(std::ostream& stream, const FunctionPtr& function, std::string* errorMessage) {
    if (function == nullptr) {
        return setError(errorMessage, "Cannot write a null function to .urc.");
    }

    if (!writeString(stream, function->name, errorMessage)) {
        return false;
    }

    int32_t arity = static_cast<int32_t>(function->arity);
    if (!writePod(stream, arity, errorMessage)) {
        return false;
    }

    uint8_t isAsync = function->isAsync ? 1 : 0;
    if (!writePod(stream, isAsync, errorMessage)) {
        return false;
    }

    int32_t minArity = static_cast<int32_t>(function->minArity);
    if (!writePod(stream, minArity, errorMessage)) {
        return false;
    }

    uint8_t hasReceiverSlot = function->hasReceiverSlot ? 1 : 0;
    if (!writePod(stream, hasReceiverSlot, errorMessage)) {
        return false;
    }

    if (!writeStringVector(stream, function->parameterNames, errorMessage) ||
        !writeStringVector(stream, function->parameterTypes, errorMessage) ||
        !writeStringVector(stream, function->genericParameters, errorMessage) ||
        !writeString(stream, function->returnType, errorMessage)) {
        return false;
    }

    int32_t upvalueCount = static_cast<int32_t>(function->upvalueCount);
    if (!writePod(stream, upvalueCount, errorMessage)) {
        return false;
    }

    uint32_t codeSize = static_cast<uint32_t>(function->chunk.code.size());
    if (!writePod(stream, codeSize, errorMessage)) {
        return false;
    }

    if (codeSize > 0) {
        stream.write(reinterpret_cast<const char*>(function->chunk.code.data()),
                     static_cast<std::streamsize>(codeSize));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while writing bytecode to .urc.");
        }
    }

    uint32_t lineCount = static_cast<uint32_t>(function->chunk.lines.size());
    if (!writePod(stream, lineCount, errorMessage)) {
        return false;
    }

    for (int line : function->chunk.lines) {
        int32_t storedLine = static_cast<int32_t>(line);
        if (!writePod(stream, storedLine, errorMessage)) {
            return false;
        }
    }

    uint32_t constantCount = static_cast<uint32_t>(function->chunk.constants.values.size());
    if (!writePod(stream, constantCount, errorMessage)) {
        return false;
    }

    for (const Value& constant : function->chunk.constants.values) {
        if (!writeValue(stream, constant, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool readFunction(std::istream& stream,
                  bool legacyFormat,
                  bool supportsAsyncMetadata,
                  bool supportsSignatureMetadata,
                  FunctionPtr* function,
                  std::string* errorMessage) {
    std::string name;
    if (!readString(stream, &name, errorMessage)) {
        return false;
    }

    int32_t arity = 0;
    if (!readPod(stream, &arity, errorMessage)) {
        return false;
    }

    FunctionPtr result = uraniumHeap().allocateFunction(name, arity);
    result->isAsync = false;
    result->minArity = arity;
    result->hasReceiverSlot = false;
    result->parameterNames.clear();
    result->parameterTypes.clear();
    result->genericParameters.clear();
    result->returnType.clear();
    if (supportsAsyncMetadata) {
        uint8_t isAsync = 0;
        if (!readPod(stream, &isAsync, errorMessage)) {
            return false;
        }
        result->isAsync = (isAsync != 0);
    }

    if (supportsSignatureMetadata) {
        int32_t minArity = 0;
        if (!readPod(stream, &minArity, errorMessage)) {
            return false;
        }
        result->minArity = minArity;

        uint8_t hasReceiverSlot = 0;
        if (!readPod(stream, &hasReceiverSlot, errorMessage)) {
            return false;
        }
        result->hasReceiverSlot = (hasReceiverSlot != 0);

        if (!readStringVector(stream, &result->parameterNames, errorMessage) ||
            !readStringVector(stream, &result->parameterTypes, errorMessage) ||
            !readStringVector(stream, &result->genericParameters, errorMessage) ||
            !readString(stream, &result->returnType, errorMessage)) {
            return false;
        }
    }

    if (legacyFormat) {
        result->upvalueCount = 0;
    } else {
        int32_t upvalueCount = 0;
        if (!readPod(stream, &upvalueCount, errorMessage)) {
            return false;
        }
        result->upvalueCount = upvalueCount;
    }

    uint32_t codeSize = 0;
    if (!readPod(stream, &codeSize, errorMessage)) {
        return false;
    }

    result->chunk.code.resize(codeSize);
    if (codeSize > 0) {
        stream.read(reinterpret_cast<char*>(result->chunk.code.data()),
                    static_cast<std::streamsize>(codeSize));
        if (!stream.good()) {
            return setError(errorMessage, "Failed while reading bytecode from .urc.");
        }
    }

    uint32_t lineCount = 0;
    if (!readPod(stream, &lineCount, errorMessage)) {
        return false;
    }

    result->chunk.lines.resize(lineCount);
    for (uint32_t i = 0; i < lineCount; ++i) {
        int32_t storedLine = 0;
        if (!readPod(stream, &storedLine, errorMessage)) {
            return false;
        }
        result->chunk.lines[i] = storedLine;
    }

    if (result->chunk.code.size() != result->chunk.lines.size()) {
        return setError(errorMessage, "Corrupt .urc: code and line tables do not match.");
    }

    uint32_t constantCount = 0;
    if (!readPod(stream, &constantCount, errorMessage)) {
        return false;
    }

    result->chunk.constants.values.clear();
    result->chunk.constants.values.reserve(constantCount);
    for (uint32_t i = 0; i < constantCount; ++i) {
        Value constant;
        if (!readValue(stream, legacyFormat, supportsAsyncMetadata,
                       supportsSignatureMetadata, &constant, errorMessage)) {
            return false;
        }
        result->chunk.constants.write(constant);
    }

    *function = result;
    return true;
}

bool isOutsideWorkingDirectory(const std::filesystem::path& relativePath) {
    for (const auto& component : relativePath) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

} // namespace

bool writeUrcStream(const FunctionPtr& function,
                    std::ostream& stream,
                    std::string* errorMessage) {
    stream.write(kUrcMagicV4, sizeof(kUrcMagicV4));
    if (!stream.good()) {
        return setError(errorMessage, "Failed while writing the .urc header.");
    }

    return writeFunction(stream, function, errorMessage);
}

bool readUrcStream(std::istream& stream,
                   FunctionPtr* function,
                   std::string* errorMessage) {
    char magic[sizeof(kUrcMagicV4)] = {};
    stream.read(magic, sizeof(magic));
    if (!stream.good()) {
        return setError(errorMessage, "Could not read the .urc header.");
    }

    bool legacyFormat = true;
    bool matchesV1 = true;
    bool matchesV2 = true;
    bool matchesV3 = true;
    bool matchesV4 = true;
    for (std::size_t i = 0; i < sizeof(kUrcMagicV4); ++i) {
        if (magic[i] != kUrcMagicV1[i]) {
            matchesV1 = false;
        }
        if (magic[i] != kUrcMagicV2[i]) {
            matchesV2 = false;
        }
        if (magic[i] != kUrcMagicV3[i]) {
            matchesV3 = false;
        }
        if (magic[i] != kUrcMagicV4[i]) {
            matchesV4 = false;
        }
    }

    if (!matchesV1 && !matchesV2 && !matchesV3 && !matchesV4) {
        return setError(errorMessage, "Invalid .urc file header.");
    }

    legacyFormat = matchesV1 && !matchesV2;
    return readFunction(stream, legacyFormat, matchesV3 || matchesV4, matchesV4,
                        function, errorMessage);
}

bool writeUrcFile(const FunctionPtr& function,
                  const std::filesystem::path& path,
                  std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create output directory '" + path.parent_path().string() + "'.");
    }

    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + path.string() + "' for writing.");
    }

    return writeUrcStream(function, stream, errorMessage);
}

bool readUrcFile(const std::filesystem::path& path,
                 FunctionPtr* function,
                 std::string* errorMessage) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + path.string() + "'.");
    }

    return readUrcStream(stream, function, errorMessage);
}

bool writeUraFile(const FunctionPtr& function,
                  const std::filesystem::path& path,
                  const std::string& manifestText,
                  const std::string& entryPath,
                  std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create output directory '" + path.parent_path().string() + "'.");
    }

    std::ostringstream payloadStream(std::ios::binary | std::ios::out);
    if (!writeUrcStream(function, payloadStream, errorMessage)) {
        return false;
    }
    std::string payload = payloadStream.str();

    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + path.string() + "' for writing.");
    }

    stream.write(kUraMagicV1, sizeof(kUraMagicV1));
    if (!stream.good()) {
        return setError(errorMessage, "Failed while writing the .ura header.");
    }

    if (!writeString(stream, manifestText, errorMessage) ||
        !writeString(stream, entryPath, errorMessage) ||
        !writeBlob(stream, payload, errorMessage)) {
        return false;
    }

    return true;
}

bool readUraFile(const std::filesystem::path& path,
                 FunctionPtr* function,
                 std::string* manifestText,
                 std::string* entryPath,
                 std::string* errorMessage) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + path.string() + "'.");
    }

    char magic[sizeof(kUraMagicV1)] = {};
    stream.read(magic, sizeof(magic));
    if (!stream.good()) {
        return setError(errorMessage, "Could not read the .ura header.");
    }

    for (std::size_t index = 0; index < sizeof(kUraMagicV1); ++index) {
        if (magic[index] != kUraMagicV1[index]) {
            return setError(errorMessage, "Invalid .ura file header.");
        }
    }

    std::string manifest;
    std::string entry;
    std::string payload;
    if (!readString(stream, &manifest, errorMessage) ||
        !readString(stream, &entry, errorMessage) ||
        !readBlob(stream, &payload, errorMessage)) {
        return false;
    }

    std::istringstream payloadStream(payload, std::ios::binary | std::ios::in);
    if (!readUrcStream(payloadStream, function, errorMessage)) {
        return false;
    }

    if (manifestText != nullptr) {
        *manifestText = manifest;
    }

    if (entryPath != nullptr) {
        *entryPath = entry;
    }

    return true;
}

std::filesystem::path compiledPathForSource(const std::filesystem::path& sourcePath,
                                            const std::filesystem::path& workingDirectory) {
    std::error_code errorCode;
    std::filesystem::path absoluteSource = std::filesystem::absolute(sourcePath, errorCode);
    if (errorCode) {
        absoluteSource = sourcePath;
        errorCode.clear();
    }

    std::filesystem::path absoluteWorkingDirectory =
        std::filesystem::absolute(workingDirectory, errorCode);
    if (errorCode) {
        absoluteWorkingDirectory = workingDirectory;
        errorCode.clear();
    }

    std::filesystem::path relativeSource =
        std::filesystem::relative(absoluteSource, absoluteWorkingDirectory, errorCode);

    if (errorCode || relativeSource.empty() || isOutsideWorkingDirectory(relativeSource)) {
        relativeSource = sourcePath.filename();
    }

    relativeSource.replace_extension(".urc");
    return absoluteWorkingDirectory / "compiled" / relativeSource;
}

std::filesystem::path archivePathForPackage(const std::filesystem::path& packageRoot,
                                            const std::string& packageName) {
    std::filesystem::path archiveName = packageName.empty()
                                            ? std::filesystem::path("app.ura")
                                            : std::filesystem::path(packageName).replace_extension(".ura");
    return std::filesystem::absolute(packageRoot) / "compiled" / archiveName;
}
