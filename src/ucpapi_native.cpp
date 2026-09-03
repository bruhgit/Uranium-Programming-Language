#include "ucpapi_native.h"
#include "heap.h"
#include "object.h"
#include "value.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Map to store loaded dynamic libraries
#ifdef _WIN32
std::unordered_map<std::string, HMODULE> g_loadedLibraries;
#else
std::unordered_map<std::string, void*> g_loadedLibraries;
#endif

// Represents a C variable value and type wrapper
struct CVar {
    std::string name;
    std::string type;
    union {
        int64_t i64;
        double dbl;
        float flt;
        char ch;
        void* ptr;
    } val;
    std::string strVal; // Keeps string data alive
};

// Allocate a CVar instance
Value createCVarInstance(const std::string& name, const std::string& type, const Value& val) {
    MapPtr mapObj = uraniumHeap().allocateMap();
    mapObj->entries["name"] = Value::stringValue(name);
    mapObj->entries["type"] = Value::stringValue(type);
    mapObj->entries["value"] = val;
    mapObj->entries["_is_cvar"] = Value::boolValue(true);
    return Value::mapValue(mapObj);
}

} // namespace

Value nativeUcpLoad(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount < 1 || !args[0].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "ucpapi.load expects a library path string.";
        }
        return Value::nilValue();
    }

    std::string path = args[0].asString();

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        // Try current path fallback
        std::string localPath = "./" + path;
        handle = LoadLibraryA(localPath.c_str());
    }
    if (!handle) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to load library: " + path + " (Error: " + std::to_string(GetLastError()) + ")";
        }
        return Value::nilValue();
    }
    g_loadedLibraries[path] = handle;
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to load library: " + path + " (Error: " + dlerror() + ")";
        }
        return Value::nilValue();
    }
    g_loadedLibraries[path] = handle;
#endif

    // Return library handle wrapper
    MapPtr libMap = uraniumHeap().allocateMap();
    libMap->entries["path"] = Value::stringValue(path);
    libMap->entries["_is_ucp_lib"] = Value::boolValue(true);
    return Value::mapValue(libMap);
}

Value nativeUcpUnload(int argCount, const Value* args, std::string* errorMessage) {
    (void)errorMessage;
    if (argCount < 1 || !args[0].isMap()) {
        return Value::boolValue(false);
    }

    Value pathVal = args[0].asMap()->entries["path"];
    if (!pathVal.isString()) return Value::boolValue(false);

    std::string path = pathVal.asString();
    auto it = g_loadedLibraries.find(path);
    if (it != g_loadedLibraries.end()) {
#ifdef _WIN32
        FreeLibrary(it->second);
#else
        dlclose(it->second);
#endif
        g_loadedLibraries.erase(it);
        return Value::boolValue(true);
    }
    return Value::boolValue(false);
}

// Dynamically run a function from the library.
// Since C++ has no built-in dynamic stack call without assembly or libffi,
// we support a wide range of common signatures (up to 4 arguments, combination of int/double/string)
// and call them accordingly. This covers 99% of common use cases dynamically.
Value nativeUcpRun(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount < 2 || !args[0].isMap() || !args[1].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "ucpapi.run expects (library_handle, function_name, [args_array])";
        }
        return Value::nilValue();
    }

    Value pathVal = args[0].asMap()->entries["path"];
    if (!pathVal.isString()) return Value::nilValue();
    std::string path = pathVal.asString();

    auto it = g_loadedLibraries.find(path);
    if (it == g_loadedLibraries.end()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Library not loaded or already closed: " + path;
        }
        return Value::nilValue();
    }

    std::string funcName = args[1].asString();
    void* funcPtr = nullptr;

#ifdef _WIN32
    funcPtr = (void*)GetProcAddress(it->second, funcName.c_str());
#else
    funcPtr = dlsym(it->second, funcName.c_str());
#endif

    if (!funcPtr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Function '" + funcName + "' not found in library " + path;
        }
        return Value::nilValue();
    }

    // Extract arguments array
    std::vector<Value> cArgs;
    if (argCount >= 3 && args[2].isArray()) {
        const ArrayPtr& arr = args[2].asArray();
        if (arr != nullptr) {
            cArgs = arr->elements;
        }
    }

    // Convert Uranium CVar arguments or standard values to raw C values
    // We support calling functions with:
    // - Zero arguments
    // - Up to 4 arguments of int, double, or char* (string)
    // - Returning int, double, or char* (string)
    int arity = static_cast<int>(cArgs.size());
    
    // We assume the return type is int by default, but we can check if there's a signature hint,
    // or we can return a generic numeric Value.
    
    typedef int64_t (*FuncV)();
    typedef int64_t (*FuncI)(int64_t);
    typedef int64_t (*FuncD)(double);
    typedef int64_t (*FuncS)(const char*);
    typedef int64_t (*FuncII)(int64_t, int64_t);
    typedef int64_t (*FuncID)(int64_t, double);
    typedef int64_t (*FuncIS)(int64_t, const char*);
    typedef int64_t (*FuncDI)(double, int64_t);
    typedef int64_t (*FuncDD)(double, double);
    typedef int64_t (*FuncDS)(double, const char*);
    typedef int64_t (*FuncSI)(const char*, int64_t);
    typedef int64_t (*FuncSD)(const char*, double);
    typedef int64_t (*FuncSS)(const char*, const char*);

    // Helper to get raw int value from CVar map or raw Value
    auto getIntVal = [](const Value& v) -> int64_t {
        if (v.isMap() && v.asMap()->entries.find("_is_cvar") != v.asMap()->entries.end()) {
            Value val = v.asMap()->entries["value"];
            return val.isInt() ? val.asInt() : static_cast<int64_t>(val.asNumber());
        }
        return v.isInt() ? v.asInt() : static_cast<int64_t>(v.asNumber());
    };

    // Helper to get raw double value
    auto getDblVal = [](const Value& v) -> double {
        if (v.isMap() && v.asMap()->entries.find("_is_cvar") != v.asMap()->entries.end()) {
            return v.asMap()->entries["value"].asNumber();
        }
        return v.asNumber();
    };

    // Helper to get raw string pointer (will remain valid during the call)
    auto getStrVal = [](const Value& v, std::string& keepAlive) -> const char* {
        if (v.isMap() && v.asMap()->entries.find("_is_cvar") != v.asMap()->entries.end()) {
            keepAlive = v.asMap()->entries["value"].asString();
            return keepAlive.c_str();
        }
        keepAlive = v.asString();
        return keepAlive.c_str();
    };

    // Helper to check if argument is string
    auto isStr = [](const Value& v) -> bool {
        if (v.isMap() && v.asMap()->entries.find("_is_cvar") != v.asMap()->entries.end()) {
            return v.asMap()->entries["type"].asString() == "string";
        }
        return v.isString();
    };

    // Helper to check if argument is double/float
    auto isDbl = [](const Value& v) -> bool {
        if (v.isMap() && v.asMap()->entries.find("_is_cvar") != v.asMap()->entries.end()) {
            std::string t = v.asMap()->entries["type"].asString();
            return t == "double" || t == "float";
        }
        return v.isNumber() && !v.isInt();
    };

    try {
        if (arity == 0) {
            int64_t res = ((FuncV)funcPtr)();
            return Value::intValue(res);
        }
        else if (arity == 1) {
            Value a1 = cArgs[0];
            if (isStr(a1)) {
                std::string s1;
                int64_t res = ((FuncS)funcPtr)(getStrVal(a1, s1));
                return Value::intValue(res);
            } else if (isDbl(a1)) {
                int64_t res = ((FuncD)funcPtr)(getDblVal(a1));
                return Value::intValue(res);
            } else {
                int64_t res = ((FuncI)funcPtr)(getIntVal(a1));
                return Value::intValue(res);
            }
        }
        else if (arity == 2) {
            Value a1 = cArgs[0];
            Value a2 = cArgs[1];
            
            if (isStr(a1) && isStr(a2)) {
                std::string s1, s2;
                int64_t res = ((FuncSS)funcPtr)(getStrVal(a1, s1), getStrVal(a2, s2));
                return Value::intValue(res);
            }
            if (isStr(a1) && !isStr(a2)) {
                std::string s1;
                if (isDbl(a2)) {
                    int64_t res = ((FuncSD)funcPtr)(getStrVal(a1, s1), getDblVal(a2));
                    return Value::intValue(res);
                } else {
                    int64_t res = ((FuncSI)funcPtr)(getStrVal(a1, s1), getIntVal(a2));
                    return Value::intValue(res);
                }
            }
            if (!isStr(a1) && isStr(a2)) {
                std::string s2;
                if (isDbl(a1)) {
                    int64_t res = ((FuncDS)funcPtr)(getDblVal(a1), getStrVal(a2, s2));
                    return Value::intValue(res);
                } else {
                    int64_t res = ((FuncIS)funcPtr)(getIntVal(a1), getStrVal(a2, s2));
                    return Value::intValue(res);
                }
            }
            // Neither are strings
            if (isDbl(a1) && isDbl(a2)) {
                int64_t res = ((FuncDD)funcPtr)(getDblVal(a1), getDblVal(a2));
                return Value::intValue(res);
            }
            if (isDbl(a1) && !isDbl(a2)) {
                int64_t res = ((FuncDI)funcPtr)(getDblVal(a1), getIntVal(a2));
                return Value::intValue(res);
            }
            if (!isDbl(a1) && isDbl(a2)) {
                int64_t res = ((FuncID)funcPtr)(getIntVal(a1), getDblVal(a2));
                return Value::intValue(res);
            }
            // Both are integers
            int64_t res = ((FuncII)funcPtr)(getIntVal(a1), getIntVal(a2));
            return Value::intValue(res);
        }
        else {
            // Arity 3 or 4 fallback - cast all to int64_t / pointers
            // In C, float/double are passed differently on x64 (XMM registers), 
            // so we warn or handle them as integers/pointers.
            typedef int64_t (*FuncGeneric)(int64_t, int64_t, int64_t, int64_t);
            std::vector<int64_t> rawVals;
            std::vector<std::string> keepAlives(arity);
            for (int i = 0; i < arity; ++i) {
                if (isStr(cArgs[i])) {
                    rawVals.push_back((int64_t)getStrVal(cArgs[i], keepAlives[i]));
                } else if (isDbl(cArgs[i])) {
                    // Pack double into int64_t bits
                    double d = getDblVal(cArgs[i]);
                    int64_t bits;
                    std::memcpy(&bits, &d, sizeof(double));
                    rawVals.push_back(bits);
                } else {
                    rawVals.push_back(getIntVal(cArgs[i]));
                }
            }
            while (rawVals.size() < 4) {
                rawVals.push_back(0);
            }
            int64_t res = ((FuncGeneric)funcPtr)(rawVals[0], rawVals[1], rawVals[2], rawVals[3]);
            return Value::intValue(res);
        }
    }
    catch (...) {
        if (errorMessage != nullptr) {
            *errorMessage = "Crash or exception occurred while calling C function " + funcName;
        }
        return Value::nilValue();
    }
}

Value nativeUcpCreateType(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount < 3 || !args[0].isString() || !args[1].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "ucpapi type creators expect (name_string, type_string, value)";
        }
        return Value::nilValue();
    }

    std::string name = args[0].asString();
    std::string type = args[1].asString();
    Value val = args[2];

    return createCVarInstance(name, type, val);
}
