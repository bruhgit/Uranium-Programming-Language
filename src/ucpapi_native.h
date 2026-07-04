#ifndef uranium_ucpapi_native_h
#define uranium_ucpapi_native_h

#include "value.h"
#include <string>

// UCPAPI Native function bindings
Value nativeUcpLoad(int argCount, const Value* args, std::string* errorMessage);
Value nativeUcpUnload(int argCount, const Value* args, std::string* errorMessage);
Value nativeUcpRun(int argCount, const Value* args, std::string* errorMessage);
Value nativeUcpCreateType(int argCount, const Value* args, std::string* errorMessage);

#endif
