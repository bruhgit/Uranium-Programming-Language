#include "ffi_native.h"
#include "heap.h"
#include "object.h"
#include "value.h"
#include "native_jit_mem.h"

#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

#ifdef _WIN32
std::unordered_map<std::string, HMODULE> g_ffiLibraries;
#else
std::unordered_map<std::string, void*> g_ffiLibraries;
#endif

// A simple X64 emitter for the Trampoline
class FfiAssembler {
public:
    std::vector<uint8_t> code;

    void emitByte(uint8_t b) { code.push_back(b); }
    void emitBytes(std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes.begin(), bytes.end()); }
    void emitInt32(int32_t val) {
        uint8_t bytes[4];
        std::memcpy(bytes, &val, 4);
        code.insert(code.end(), bytes, bytes + 4);
    }
    void emitInt64(int64_t val) {
        uint8_t bytes[8];
        std::memcpy(bytes, &val, 8);
        code.insert(code.end(), bytes, bytes + 8);
    }
    
    // mov rcx, val
    void emitMovRcx(int64_t val) { emitBytes({0x48, 0xB9}); emitInt64(val); }
    // mov rdx, val
    void emitMovRdx(int64_t val) { emitBytes({0x48, 0xBA}); emitInt64(val); }
    // mov r8, val
    void emitMovR8(int64_t val) { emitBytes({0x49, 0xB8}); emitInt64(val); }
    // mov r9, val
    void emitMovR9(int64_t val) { emitBytes({0x49, 0xB9}); emitInt64(val); }
    
    // mov rdi, val (System V)
    void emitMovRdi(int64_t val) { emitBytes({0x48, 0xBF}); emitInt64(val); }
    // mov rsi, val (System V)
    void emitMovRsi(int64_t val) { emitBytes({0x48, 0xBE}); emitInt64(val); }

    // mov rax, funcPtr; call rax
    void emitCall(void* funcPtr) {
        emitBytes({0x48, 0xB8});
        emitInt64((int64_t)funcPtr);
        emitBytes({0xFF, 0xD0}); // call rax
    }
    
    // ret
    void emitRet() { emitByte(0xC3); }
    
    // sub rsp, 40 (Shadow space for Windows x64 ABI + alignment)
    void emitPrologue() {
        emitBytes({0x48, 0x83, 0xEC, 0x28});
    }
    // add rsp, 40
    void emitEpilogue() {
        emitBytes({0x48, 0x83, 0xC4, 0x28});
    }
};

} // namespace

Value nativeFfiLoad(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount < 1 || !args[0].isString()) {
        if (errorMessage) *errorMessage = "ffi.load expects a string path.";
        return Value::nilValue();
    }
    std::string path = args[0].asString();
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        if (errorMessage) *errorMessage = "Failed to load DLL: " + path;
        return Value::nilValue();
    }
    g_ffiLibraries[path] = handle;
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        if (errorMessage) *errorMessage = "Failed to load shared library: " + path;
        return Value::nilValue();
    }
    g_ffiLibraries[path] = handle;
#endif

    MapPtr map = uraniumHeap().allocateMap();
    map->entries["path"] = Value::stringValue(path);
    map->entries["_is_ffi_lib"] = Value::boolValue(true);
    return Value::mapValue(map);
}

Value nativeFfiCall(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount < 4 || !args[0].isMap() || !args[1].isString() || !args[2].isString() || !args[3].isArray()) {
        if (errorMessage) *errorMessage = "ffi.call expects (lib, funcName, returnType, [args])";
        return Value::nilValue();
    }

    std::string path = args[0].asMap()->entries["path"].asString();
    if (g_ffiLibraries.find(path) == g_ffiLibraries.end()) {
        if (errorMessage) *errorMessage = "Library not loaded.";
        return Value::nilValue();
    }

    std::string funcName = args[1].asString();
    std::string retType = args[2].asString();
    ArrayPtr ffiArgs = args[3].asArray();

#ifdef _WIN32
    void* funcPtr = (void*)GetProcAddress(g_ffiLibraries[path], funcName.c_str());
#else
    void* funcPtr = dlsym(g_ffiLibraries[path], funcName.c_str());
#endif

    if (!funcPtr) {
        if (errorMessage) *errorMessage = "Function not found: " + funcName;
        return Value::nilValue();
    }

    FfiAssembler asmGen;
    asmGen.emitPrologue();

    // Prepare arguments (up to 4 supported in this MVP)
    int argLen = static_cast<int>(ffiArgs->elements.size());
    if (argLen > 4) argLen = 4;

    for (int i = 0; i < argLen; ++i) {
        Value val = ffiArgs->elements[i];
        int64_t rawValue = 0;
        
        if (val.isNumber()) {
            rawValue = (int64_t)val.asNumber(); // cast double to int
        } else if (val.isString()) {
            rawValue = (int64_t)val.asString().c_str(); // pointer to string
        } else if (val.isBool()) {
            rawValue = val.asBool() ? 1 : 0;
        }

#ifdef _WIN32
        // Windows ABI
        if (i == 0) asmGen.emitMovRcx(rawValue);
        else if (i == 1) asmGen.emitMovRdx(rawValue);
        else if (i == 2) asmGen.emitMovR8(rawValue);
        else if (i == 3) asmGen.emitMovR9(rawValue);
#else
        // System V ABI (Linux/Mac)
        if (i == 0) asmGen.emitMovRdi(rawValue);
        else if (i == 1) asmGen.emitMovRsi(rawValue);
        else if (i == 2) asmGen.emitMovRdx(rawValue);
        else if (i == 3) asmGen.emitMovRcx(rawValue);
#endif
    }

    asmGen.emitCall(funcPtr);
    asmGen.emitEpilogue();
    asmGen.emitRet();

    // Allocate executable memory
    void* execMem = jit_alloc_executable(asmGen.code.size());
    std::memcpy(execMem, asmGen.code.data(), asmGen.code.size());
    jit_make_executable(execMem, asmGen.code.size());

    // Call the Trampoline!
    typedef int64_t (*TrampolineFunc)();
    TrampolineFunc trampoline = (TrampolineFunc)execMem;
    
    int64_t result = trampoline();

    // Free the memory
    jit_free_executable(execMem, asmGen.code.size());

    if (retType == "int" || retType == "long" || retType == "bool") {
        return Value::numberValue((double)result);
    } else if (retType == "string" && result != 0) {
        return Value::stringValue(std::string((char*)result));
    }

    return Value::nilValue();
}

Value nativeFfiUnload(int argCount, const Value* args, std::string* errorMessage) {
    (void)errorMessage;
    if (argCount < 1 || !args[0].isMap()) return Value::boolValue(false);
    std::string path = args[0].asMap()->entries["path"].asString();
    auto it = g_ffiLibraries.find(path);
    if (it != g_ffiLibraries.end()) {
#ifdef _WIN32
        FreeLibrary(it->second);
#else
        dlclose(it->second);
#endif
        g_ffiLibraries.erase(it);
        return Value::boolValue(true);
    }
    return Value::boolValue(false);
}
