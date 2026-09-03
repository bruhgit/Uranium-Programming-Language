#ifndef uranium_system_native_h
#define uranium_system_native_h

#include "value.h"
#include <string>
#include <vector>

void configureRuntimeProcessContext(const std::string& executablePath,
                                    const std::string& entryPath,
                                    const std::vector<std::string>& scriptArgs);
const std::vector<std::string>& getRuntimeScriptArgs();

Value nativeFsCwd(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsChangeDir(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsExists(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsIsFile(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsIsDir(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsNormalize(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsAbsolute(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsParent(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsFileName(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsStem(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsExtension(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsJoin2(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsJoin3(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsJoin4(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsReadText(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsReadLines(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsWriteText(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsAppendText(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsCreateDir(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsCreateDirs(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsRemove(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsRemoveTree(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsCopy(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsMove(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsStat(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsListNames(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsListEntries(int argCount, const Value* args, std::string* errorMessage);
Value nativeFsWalk(int argCount, const Value* args, std::string* errorMessage);

Value nativeProcessArgs(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessArgCount(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessExecutablePath(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessEntryPath(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessCwd(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessChangeDir(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessGetEnv(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessSetEnv(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessPlatform(int argCount, const Value* args, std::string* errorMessage);
Value nativeRuntimeCapabilities(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessPid(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessSleep(int argCount, const Value* args, std::string* errorMessage);
Value nativeInput(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessRun(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessSystem(int argCount, const Value* args, std::string* errorMessage);
Value nativeProcessExit(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadHardwareConcurrency(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadYield(int argCount, const Value* args, std::string* errorMessage);

Value nativeJsonParse(int argCount, const Value* args, std::string* errorMessage);
Value nativeJsonValid(int argCount, const Value* args, std::string* errorMessage);
Value nativeJsonStringify(int argCount, const Value* args, std::string* errorMessage);
Value nativeJsonStringifyPretty(int argCount, const Value* args, std::string* errorMessage);

#endif
