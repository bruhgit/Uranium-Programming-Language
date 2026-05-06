#ifndef uranium_http_native_h
#define uranium_http_native_h

#include "value.h"
#include <string>

Value nativeHttpRequest(int argCount, const Value* args, std::string* errorMessage);

#endif
