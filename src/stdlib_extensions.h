#ifndef STDLIB_EXTENSIONS_H
#define STDLIB_EXTENSIONS_H

#include "value.h"
#include <string>

void registerStdlibExtensions();
bool bindStringMethod(const Value& receiver, const std::string& property, Value* result);
bool bindMapMethod(const Value& receiver, const std::string& property, Value* result);

#endif
