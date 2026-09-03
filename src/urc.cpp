#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif
#include "urc.h"
#include "common.h"
#include "heap.h"
#include "object.h"
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr char kUrcMagicV1[] = {'U', 'R', 'C', '1'};
constexpr char kUrcMagicV2[] = {'U', 'R', 'C', '2'};
constexpr char kUrcMagicV3[] = {'U', 'R', 'C', '3'};
constexpr char kUrcMagicV4[] = {'U', 'R', 'C', '4'};
constexpr char kUraMagicV1[] = {'U', 'R', 'A', '1'};
constexpr char kAotFooterMagic[] = {'U', 'A', 'O', 'T', 'B', 'I', 'N', '1'};

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

uint16_t readLe16(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1] << 8);
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void writeLe16(std::vector<uint8_t>* bytes, std::size_t offset, uint16_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value & 0xff);
    (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void writeLe32(std::vector<uint8_t>* bytes, std::size_t offset, uint32_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value & 0xff);
    (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    (*bytes)[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    (*bytes)[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

struct IconImage {
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t colorCount = 0;
    uint16_t planes = 0;
    uint16_t bitCount = 0;
    std::vector<uint8_t> data;
};

bool readIconFile(const std::filesystem::path& iconPath,
                  std::vector<IconImage>* images,
                  std::string* errorMessage) {
    std::ifstream stream(iconPath, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open icon file '" + iconPath.string() + "'.");
    }

    std::streamoff fileSize = stream.tellg();
    if (fileSize < 6 || fileSize > 32 * 1024 * 1024) {
        return setError(errorMessage, "Invalid .ico file size: " + iconPath.string());
    }

    std::vector<uint8_t> bytes(static_cast<std::size_t>(fileSize));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    if (!stream.good()) {
        return setError(errorMessage, "Could not read icon file '" + iconPath.string() + "'.");
    }

    uint16_t reserved = readLe16(bytes, 0);
    uint16_t type = readLe16(bytes, 2);
    uint16_t count = readLe16(bytes, 4);
    if (reserved != 0 || type != 1 || count == 0) {
        return setError(errorMessage, "Expected a Windows .ico icon file: " + iconPath.string());
    }

    std::size_t directorySize = 6 + static_cast<std::size_t>(count) * 16;
    if (directorySize > bytes.size()) {
        return setError(errorMessage, "Corrupt .ico directory: " + iconPath.string());
    }

    images->clear();
    images->reserve(count);
    for (uint16_t index = 0; index < count; ++index) {
        std::size_t entry = 6 + static_cast<std::size_t>(index) * 16;
        uint32_t imageSize = readLe32(bytes, entry + 8);
        uint32_t imageOffset = readLe32(bytes, entry + 12);
        if (imageSize == 0 || imageOffset > bytes.size() || imageSize > bytes.size() - imageOffset) {
            return setError(errorMessage, "Corrupt .ico image data: " + iconPath.string());
        }

        IconImage image;
        image.width = bytes[entry];
        image.height = bytes[entry + 1];
        image.colorCount = bytes[entry + 2];
        image.planes = readLe16(bytes, entry + 4);
        image.bitCount = readLe16(bytes, entry + 6);
        image.data.assign(bytes.begin() + imageOffset, bytes.begin() + imageOffset + imageSize);
        images->push_back(std::move(image));
    }

    return true;
}

bool applyExecutableIcon(const std::filesystem::path& executablePath,
                         const std::filesystem::path& iconPath,
                         std::string* errorMessage) {
#ifdef _WIN32
    std::vector<IconImage> images;
    if (!readIconFile(iconPath, &images, errorMessage)) {
        return false;
    }

    HANDLE update = BeginUpdateResourceW(executablePath.wstring().c_str(), FALSE);
    if (update == nullptr) {
        return setError(errorMessage, "Could not open executable resources for icon update. Windows error " +
                                    std::to_string(GetLastError()) + ".");
    }

    WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    for (std::size_t index = 0; index < images.size(); ++index) {
        const IconImage& image = images[index];
        WORD resourceId = static_cast<WORD>(index + 1);
        if (!UpdateResourceW(update, MAKEINTRESOURCEW(3) /* RT_ICON */, MAKEINTRESOURCEW(resourceId), language,
                             const_cast<uint8_t*>(image.data.data()),
                             static_cast<DWORD>(image.data.size()))) {
            DWORD error = GetLastError();
            EndUpdateResourceW(update, TRUE);
            return setError(errorMessage, "Could not write icon image resource. Windows error " +
                                        std::to_string(error) + ".");
        }
    }

    std::vector<uint8_t> group(6 + images.size() * 14, 0);
    writeLe16(&group, 0, 0);
    writeLe16(&group, 2, 1);
    writeLe16(&group, 4, static_cast<uint16_t>(images.size()));
    for (std::size_t index = 0; index < images.size(); ++index) {
        const IconImage& image = images[index];
        std::size_t entry = 6 + index * 14;
        group[entry] = image.width;
        group[entry + 1] = image.height;
        group[entry + 2] = image.colorCount;
        group[entry + 3] = 0;
        writeLe16(&group, entry + 4, image.planes);
        writeLe16(&group, entry + 6, image.bitCount);
        writeLe32(&group, entry + 8, static_cast<uint32_t>(image.data.size()));
        writeLe16(&group, entry + 12, static_cast<uint16_t>(index + 1));
    }

    if (!UpdateResourceW(update, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, MAKEINTRESOURCEW(1), language,
                         group.data(), static_cast<DWORD>(group.size()))) {
        DWORD error = GetLastError();
        EndUpdateResourceW(update, TRUE);
        return setError(errorMessage, "Could not write icon group resource. Windows error " +
                                    std::to_string(error) + ".");
    }

    if (!EndUpdateResourceW(update, FALSE)) {
        return setError(errorMessage, "Could not commit executable icon resources. Windows error " +
                                    std::to_string(GetLastError()) + ".");
    }
    return true;
#endif
}

struct ResourceNode {
    std::wstring key;
    uint16_t type = 1; // 1 = text, 0 = binary
    std::vector<uint8_t> valueBytes;
    std::vector<ResourceNode> children;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> data;
        // wLength placeholder (2 bytes)
        data.push_back(0); data.push_back(0);
        
        // wValueLength (2 bytes)
        uint16_t valLen = static_cast<uint16_t>(valueBytes.size());
        if (type == 1 && valLen > 0) {
            // for text type, wValueLength is in WCHARs, including null terminator
            valLen = valLen / 2;
        }
        data.push_back(static_cast<uint8_t>(valLen & 0xFF));
        data.push_back(static_cast<uint8_t>((valLen >> 8) & 0xFF));

        // wType (2 bytes)
        data.push_back(static_cast<uint8_t>(type & 0xFF));
        data.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));

        // szKey (null-terminated WCHAR string)
        for (wchar_t c : key) {
            data.push_back(static_cast<uint8_t>(c & 0xFF));
            data.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        }
        data.push_back(0); data.push_back(0); // L'\0'

        // align to 32-bit boundary
        while (data.size() % 4 != 0) {
            data.push_back(0);
        }

        // Value
        if (!valueBytes.empty()) {
            data.insert(data.end(), valueBytes.begin(), valueBytes.end());
            // align to 32-bit boundary
            while (data.size() % 4 != 0) {
                data.push_back(0);
            }
        }

        // Children
        for (const auto& child : children) {
            std::vector<uint8_t> childBytes = child.serialize();
            data.insert(data.end(), childBytes.begin(), childBytes.end());
        }

        // Write total length
        uint16_t totalLen = static_cast<uint16_t>(data.size());
        data[0] = static_cast<uint8_t>(totalLen & 0xFF);
        data[1] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);

        return data;
    }
};

static void parseVersionString(const std::wstring& verStr, uint32_t* ms, uint32_t* ls) {
    uint32_t major = 1, minor = 0, patch = 0, build = 0;
    if (std::swscanf(verStr.c_str(), L"%u.%u.%u.%u", &major, &minor, &patch, &build) < 1) {
        major = 1; minor = 0; patch = 0; build = 0;
        std::swscanf(verStr.c_str(), L"%u.%u.%u", &major, &minor, &patch);
    }
    *ms = (major << 16) | minor;
    *ls = (patch << 16) | build;
}

static ResourceNode createStringNode(const std::wstring& name, const std::wstring& val) {
    ResourceNode node;
    node.key = name;
    node.type = 1;
    for (wchar_t c : val) {
        node.valueBytes.push_back(static_cast<uint8_t>(c & 0xFF));
        node.valueBytes.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    }
    node.valueBytes.push_back(0);
    node.valueBytes.push_back(0);
    return node;
}

bool applyExecutableVersionInfo(const std::filesystem::path& executablePath,
                                const std::wstring& companyName,
                                const std::wstring& fileDescription,
                                const std::wstring& fileVersion,
                                const std::wstring& productName,
                                const std::wstring& productVersion,
                                const std::wstring& originalFilename,
                                std::string* errorMessage) {
#ifdef _WIN32
    ResourceNode root;
    root.key = L"VS_VERSION_INFO";
    root.type = 0;

    VS_FIXEDFILEINFO fixed = {};
    fixed.dwSignature = 0xFEEF04BD;
    fixed.dwStrucVersion = 0x00010000;
    
    uint32_t fileMS = 0, fileLS = 0;
    parseVersionString(fileVersion, &fileMS, &fileLS);
    fixed.dwFileVersionMS = fileMS;
    fixed.dwFileVersionLS = fileLS;

    uint32_t prodMS = 0, prodLS = 0;
    parseVersionString(productVersion, &prodMS, &prodLS);
    fixed.dwProductVersionMS = prodMS;
    fixed.dwProductVersionLS = prodLS;

    fixed.dwFileFlagsMask = 0x3F;
    fixed.dwFileFlags = 0;
    fixed.dwFileOS = 0x00040004;
    fixed.dwFileType = 0x00000001;
    fixed.dwFileSubtype = 0;

    uint8_t* fixedPtr = reinterpret_cast<uint8_t*>(&fixed);
    root.valueBytes.assign(fixedPtr, fixedPtr + sizeof(fixed));

    ResourceNode stringFileInfo;
    stringFileInfo.key = L"StringFileInfo";
    stringFileInfo.type = 1;

    ResourceNode langBlock;
    langBlock.key = L"040904B0";
    langBlock.type = 1;

    langBlock.children.push_back(createStringNode(L"CompanyName", companyName));
    langBlock.children.push_back(createStringNode(L"FileDescription", fileDescription));
    langBlock.children.push_back(createStringNode(L"FileVersion", fileVersion));
    langBlock.children.push_back(createStringNode(L"ProductName", productName));
    langBlock.children.push_back(createStringNode(L"ProductVersion", productVersion));
    langBlock.children.push_back(createStringNode(L"OriginalFilename", originalFilename));

    stringFileInfo.children.push_back(langBlock);
    root.children.push_back(stringFileInfo);

    ResourceNode varFileInfo;
    varFileInfo.key = L"VarFileInfo";
    varFileInfo.type = 1;

    ResourceNode translation;
    translation.key = L"Translation";
    translation.type = 0;
    translation.valueBytes.push_back(0x09);
    translation.valueBytes.push_back(0x04);
    translation.valueBytes.push_back(0xB0);
    translation.valueBytes.push_back(0x04);

    varFileInfo.children.push_back(translation);
    root.children.push_back(varFileInfo);

    std::vector<uint8_t> serialized = root.serialize();

    HANDLE update = BeginUpdateResourceW(executablePath.wstring().c_str(), FALSE);
    if (update == nullptr) {
        return setError(errorMessage, "Could not open executable resources for version update. Windows error " +
                                    std::to_string(GetLastError()) + ".");
    }

    WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    if (!UpdateResourceW(update, MAKEINTRESOURCEW(16) /* RT_VERSION */, MAKEINTRESOURCEW(1), language,
                         serialized.data(), static_cast<DWORD>(serialized.size()))) {
        DWORD error = GetLastError();
        EndUpdateResourceW(update, TRUE);
        return setError(errorMessage, "Could not write version resource. Windows error " +
                                    std::to_string(error) + ".");
    }

    if (!EndUpdateResourceW(update, FALSE)) {
        return setError(errorMessage, "Could not commit executable version resources. Windows error " +
                                    std::to_string(GetLastError()) + ".");
    }
    return true;
#else
    (void)executablePath;
    (void)companyName;
    (void)fileDescription;
    (void)fileVersion;
    (void)productName;
    (void)productVersion;
    (void)originalFilename;
    return setError(errorMessage, "Executable version editing is only supported on Windows builds.");
#endif
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
        case VAL_INT:
            return writePod(stream, value.integer, errorMessage);
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
        case VAL_INT: {
            int64_t integer;
            if (!readPod(stream, &integer, errorMessage)) {
                return false;
            }
            *value = Value::intValue(integer);
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
                  const std::string& sourceText,
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

    return writeUraStream(function, stream, manifestText, entryPath, sourceText, errorMessage);
}

bool writeUraStream(const FunctionPtr& function,
                    std::ostream& stream,
                    const std::string& manifestText,
                    const std::string& entryPath,
                    const std::string& sourceText,
                    std::string* errorMessage) {
    std::ostringstream payloadStream(std::ios::binary | std::ios::out);
    if (!writeUrcStream(function, payloadStream, errorMessage)) {
        return false;
    }
    std::string payload = payloadStream.str();

    stream.write(kUraMagicV1, sizeof(kUraMagicV1));
    if (!stream.good()) {
        return setError(errorMessage, "Failed while writing the .ura header.");
    }

    if (!writeString(stream, manifestText, errorMessage) ||
        !writeString(stream, entryPath, errorMessage) ||
        !writeString(stream, sourceText, errorMessage) ||
        !writeBlob(stream, payload, errorMessage)) {
        return false;
    }

    return true;
}

bool readUraFile(const std::filesystem::path& path,
                 FunctionPtr* function,
                 std::string* manifestText,
                 std::string* entryPath,
                 std::string* sourceText,
                 std::string* errorMessage) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + path.string() + "'.");
    }

    return readUraStream(stream, function, manifestText, entryPath, sourceText, errorMessage);
}

bool readUraStream(std::istream& stream,
                   FunctionPtr* function,
                   std::string* manifestText,
                   std::string* entryPath,
                   std::string* sourceText,
                   std::string* errorMessage) {
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
    std::string source;
    std::string payload;
    if (!readString(stream, &manifest, errorMessage) ||
        !readString(stream, &entry, errorMessage) ||
        !readString(stream, &source, errorMessage) ||
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

    if (sourceText != nullptr) {
        *sourceText = source;
    }

    return true;
}

bool writeEmbeddedAotBinary(const std::filesystem::path& runtimeExecutablePath,
                            const std::string& payload,
                            const std::filesystem::path& outputPath,
                            const std::filesystem::path& iconPath,
                            std::string* errorMessage) {
    std::ifstream runtimeStream(runtimeExecutablePath, std::ios::binary);
    if (!runtimeStream.is_open()) {
        return setError(errorMessage,
                        "Could not open runtime executable '" +
                            runtimeExecutablePath.string() + "'.");
    }

    std::error_code errorCode;
    std::filesystem::create_directories(outputPath.parent_path(), errorCode);
    if (errorCode) {
        return setError(errorMessage,
                        "Could not create output directory '" +
                            outputPath.parent_path().string() + "'.");
    }

    std::ofstream outputStream(outputPath, std::ios::binary | std::ios::trunc);
    if (!outputStream.is_open()) {
        return setError(errorMessage,
                        "Could not open '" + outputPath.string() + "' for writing.");
    }

    outputStream << runtimeStream.rdbuf();
    if (!outputStream.good()) {
        return setError(errorMessage, "Failed while copying the Uranium runtime executable.");
    }

    if (!payload.empty()) {
        outputStream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!outputStream.good()) {
            return setError(errorMessage, "Failed while writing embedded AOT payload.");
        }
    }

    outputStream.write(kAotFooterMagic, sizeof(kAotFooterMagic));
    if (!outputStream.good()) {
        return setError(errorMessage, "Failed while writing embedded AOT footer.");
    }

    uint64_t payloadSize = static_cast<uint64_t>(payload.size());
    if (!writePod(outputStream, payloadSize, errorMessage)) {
        return false;
    }

    outputStream.close();
    if (!outputStream.good()) {
        return setError(errorMessage, "Failed while closing embedded AOT output file.");
    }

    if (!iconPath.empty() && !applyExecutableIcon(outputPath, iconPath, errorMessage)) {
        return false;
    }

    if (!g_compileCompanyName.empty() || !g_compileFileDescription.empty() || !g_compileFileVersion.empty() || !g_compileProductName.empty()) {
        std::wstring origFilename = outputPath.filename().wstring();
        if (!applyExecutableVersionInfo(outputPath,
                                        g_compileCompanyName,
                                        g_compileFileDescription,
                                        g_compileFileVersion.empty() ? L"1.0.0.0" : g_compileFileVersion,
                                        g_compileProductName,
                                        g_compileProductVersion.empty() ? (g_compileFileVersion.empty() ? L"1.0.0.0" : g_compileFileVersion) : g_compileProductVersion,
                                        origFilename,
                                        errorMessage)) {
            return false;
        }
    }

    return true;
}

bool readEmbeddedAotPayload(const std::filesystem::path& executablePath,
                            std::string* payload,
                            bool* found,
                            std::string* errorMessage) {
    if (found != nullptr) {
        *found = false;
    }
    if (payload != nullptr) {
        payload->clear();
    }

    std::ifstream stream(executablePath, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return setError(errorMessage, "Could not open '" + executablePath.string() + "'.");
    }

    std::streamoff fileSize = stream.tellg();
    std::streamoff footerSize =
        static_cast<std::streamoff>(sizeof(kAotFooterMagic) + sizeof(uint64_t));
    if (fileSize < footerSize) {
        return true;
    }

    stream.seekg(fileSize - footerSize);
    char magic[sizeof(kAotFooterMagic)] = {};
    stream.read(magic, sizeof(magic));
    if (!stream.good()) {
        return setError(errorMessage, "Could not read the embedded AOT footer.");
    }

    bool matches = true;
    for (std::size_t index = 0; index < sizeof(kAotFooterMagic); ++index) {
        if (magic[index] != kAotFooterMagic[index]) {
            matches = false;
            break;
        }
    }

    if (!matches) {
        return true;
    }

    uint64_t payloadSize = 0;
    if (!readPod(stream, &payloadSize, errorMessage)) {
        return false;
    }

    if (payloadSize > static_cast<uint64_t>(fileSize - footerSize)) {
        return setError(errorMessage, "Embedded AOT payload is corrupt.");
    }

    std::streamoff payloadOffset =
        fileSize - footerSize - static_cast<std::streamoff>(payloadSize);
    stream.seekg(payloadOffset);
    if (!stream.good()) {
        return setError(errorMessage, "Could not seek to the embedded AOT payload.");
    }

    std::string loadedPayload;
    loadedPayload.resize(static_cast<std::size_t>(payloadSize));
    if (payloadSize > 0) {
        stream.read(loadedPayload.data(), static_cast<std::streamsize>(payloadSize));
        if (!stream.good()) {
            return setError(errorMessage, "Could not read the embedded AOT payload.");
        }
    }

    if (payload != nullptr) {
        *payload = std::move(loadedPayload);
    }
    if (found != nullptr) {
        *found = true;
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
