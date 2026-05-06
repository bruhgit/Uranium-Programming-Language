#ifndef uranium_godot_native_h
#define uranium_godot_native_h

#include "value.h"
#include <string>

Value nativeGodotFindEditor(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotCreateProject(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotCreateScene(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotCreateScript(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotCreatePlugin(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotCreateGDExtension(int argCount, const Value* args, std::string* errorMessage);
Value nativeGodotBuildCommand(int argCount, const Value* args, std::string* errorMessage);

#endif
