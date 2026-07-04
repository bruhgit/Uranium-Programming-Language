#ifndef uranium_compiler_h
#define uranium_compiler_h

#include "value.h"
#include <string>

bool compile(const char* source, FunctionPtr* function, const std::string& filename = "script.ur");

extern void (*g_compileErrorCallback)(const std::string& message, int line, int column, int length);

#endif
