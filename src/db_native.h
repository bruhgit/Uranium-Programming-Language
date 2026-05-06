#ifndef uranium_db_native_h
#define uranium_db_native_h

#include "value.h"
#include <string>

Value nativeDbOpen(int argCount, const Value* args, std::string* errorMessage);
Value nativeDbExecute(int argCount, const Value* args, std::string* errorMessage);
Value nativeDbQuery(int argCount, const Value* args, std::string* errorMessage);
Value nativeDbClose(int argCount, const Value* args, std::string* errorMessage);

#endif
