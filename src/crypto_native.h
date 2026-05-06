#ifndef uranium_crypto_native_h
#define uranium_crypto_native_h

#include "value.h"
#include <string>

Value nativeCryptoBase64Encode(int argCount, const Value* args, std::string* errorMessage);
Value nativeCryptoBase64Decode(int argCount, const Value* args, std::string* errorMessage);
Value nativeCryptoHashSha256(int argCount, const Value* args, std::string* errorMessage);
Value nativeCryptoAesEncrypt(int argCount, const Value* args, std::string* errorMessage);
Value nativeCryptoAesDecrypt(int argCount, const Value* args, std::string* errorMessage);

#endif
