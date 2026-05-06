#ifndef uranium_net_native_h
#define uranium_net_native_h

#include "value.h"
#include <string>

Value nativeNetTcpListen(int argCount, const Value* args, std::string* errorMessage);
Value nativeNetTcpAccept(int argCount, const Value* args, std::string* errorMessage);
Value nativeNetTcpReceive(int argCount, const Value* args, std::string* errorMessage);
Value nativeNetTcpSend(int argCount, const Value* args, std::string* errorMessage);
Value nativeNetTcpClose(int argCount, const Value* args, std::string* errorMessage);

#endif
