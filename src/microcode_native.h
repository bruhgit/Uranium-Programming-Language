#ifndef uranium_microcode_native_h
#define uranium_microcode_native_h

#include "value.h"
#include <string>

// Hardware Port Enumeration & Connection
Value nativeMicrocodeListPorts(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeOpen(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeClose(int argCount, const Value* args, std::string* errorMessage);

// Serial I/O
Value nativeMicrocodeWrite(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeRead(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeReadLine(int argCount, const Value* args, std::string* errorMessage);

// Hardware Control Lines (ESP32 / Arduino reset and bootloader)
Value nativeMicrocodeSetDTR(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeSetRTS(int argCount, const Value* args, std::string* errorMessage);
Value nativeMicrocodeResetEsp32(int argCount, const Value* args, std::string* errorMessage);

// Interactive Command Execution on Chip
Value nativeMicrocodeExecute(int argCount, const Value* args, std::string* errorMessage);

// Compilation & Flashing via Arduino toolchain / CLI
Value nativeMicrocodeCompileAndFlash(int argCount, const Value* args, std::string* errorMessage);

#endif
