#ifndef uranium_ffi_native_h
#define uranium_ffi_native_h

#include "value.h"
#include <string>

// FFI Native Bindings
Value nativeFfiLoad(int argCount, const Value* args, std::string* errorMessage);
Value nativeFfiCall(int argCount, const Value* args, std::string* errorMessage);
Value nativeFfiUnload(int argCount, const Value* args, std::string* errorMessage);

#endif // uranium_ffi_native_h
