#ifndef uranium_thread_native_h
#define uranium_thread_native_h

#include "value.h"
#include <string>

Value nativeThreadSpawn(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadJoin(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelCreate(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelSend(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelReceive(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelTryReceive(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelPoll(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelSize(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadChannelClose(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadMutexCreate(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadMutexLock(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadMutexTryLock(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadMutexUnlock(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerPoolCreate(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerPoolDestroy(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerReadText(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerWriteText(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerHttpGet(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerStatus(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerDone(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerResult(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerError(int argCount, const Value* args, std::string* errorMessage);
Value nativeThreadWorkerWait(int argCount, const Value* args, std::string* errorMessage);

#endif
