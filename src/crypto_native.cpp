#include "crypto_native.h"
#include <vector>
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "advapi32.lib")

static const std::string base64_chars = 
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

static std::string encodeBase64(const unsigned char* bytes_to_encode, size_t in_len) {
    std::string ret;
    int i = 0, j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; (i <4) ; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}

static std::string decodeBase64(const std::string& encoded_string) {
    int in_len = encoded_string.size();
    int i = 0, j = 0, in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    std::string ret;
    auto is_base64 = [](unsigned char c) { return (isalnum(c) || (c == '+') || (c == '/')); };

    while (in_len-- && ( encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i ==4) {
            for (i = 0; i <4; i++)
                char_array_4[i] = (unsigned char)base64_chars.find(char_array_4[i]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; (i < 3); i++) ret += char_array_3[i];
            i = 0;
        }
    }
    if (i) {
        for (j = i; j <4; j++) char_array_4[j] = 0;
        for (j = 0; j <4; j++) char_array_4[j] = (unsigned char)base64_chars.find(char_array_4[j]);
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        for (j = 0; (j < i - 1); j++) ret += char_array_3[j];
    }
    return ret;
}


Value nativeCryptoBase64Encode(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage) *errorMessage = "Expected a string to encode";
        return Value::nilValue();
    }
    const std::string& input = args[0].asString();
    return Value::stringValue(encodeBase64((const unsigned char*)input.data(), input.length()));
}

Value nativeCryptoBase64Decode(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage) *errorMessage = "Expected a string to decode";
        return Value::nilValue();
    }
    const std::string& input = args[0].asString();
    return Value::stringValue(decodeBase64(input));
}

Value nativeCryptoHashSha256(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage) *errorMessage = "Expected a string to hash";
        return Value::nilValue();
    }
    const std::string& input = args[0].asString();

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (errorMessage) *errorMessage = "CryptAcquireContext failed";
        return Value::nilValue();
    }
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptCreateHash failed";
        return Value::nilValue();
    }
    if(!CryptHashData(hHash, (const BYTE*)input.data(), (DWORD)input.length(), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptHashData failed";
        return Value::nilValue();
    }
    
    DWORD cbHashSize = 0, dwCount = sizeof(DWORD);
    CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&cbHashSize, &dwCount, 0);
    
    std::vector<BYTE> rgbHash(cbHashSize);
    if (!CryptGetHashParam(hHash, HP_HASHVAL, rgbHash.data(), &cbHashSize, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptGetHashParam failed";
        return Value::nilValue();
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    const char hex_chars[] = "0123456789abcdef";
    std::string sha256_hex;
    for (DWORD i = 0; i < cbHashSize; i++) {
        sha256_hex += hex_chars[(rgbHash[i] >> 4) & 0xF];
        sha256_hex += hex_chars[rgbHash[i] & 0xF];
    }

    return Value::stringValue(sha256_hex);
}

Value nativeCryptoAesEncrypt(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isString()) {
        if (errorMessage) *errorMessage = "Expected key and data to encrypt";
        return Value::nilValue();
    }
    const std::string& key = args[0].asString();
    const std::string& data = args[1].asString();

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (errorMessage) *errorMessage = "CryptAcquireContext failed";
        return Value::nilValue();
    }

    HCRYPTHASH hHash = 0;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return Value::nilValue();
    }
    CryptHashData(hHash, (const BYTE*)key.data(), (DWORD)key.length(), 0);

    HCRYPTKEY hKey = 0;
    if (!CryptDeriveKey(hProv, CALG_AES_256, hHash, 0, &hKey)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptDeriveKey failed";
        return Value::nilValue();
    }
    CryptDestroyHash(hHash);

    DWORD dataLen = (DWORD)data.length();
    DWORD bufferLen = dataLen + 32;
    std::vector<BYTE> buffer(bufferLen, 0);
    memcpy(buffer.data(), data.data(), dataLen);

    if (!CryptEncrypt(hKey, 0, TRUE, 0, buffer.data(), &dataLen, bufferLen)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptEncrypt failed";
        return Value::nilValue();
    }

    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);

    std::string encoded = encodeBase64(buffer.data(), dataLen);
    return Value::stringValue(encoded);
}

Value nativeCryptoAesDecrypt(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isString()) {
        if (errorMessage) *errorMessage = "Expected key and base64 string to decrypt";
        return Value::nilValue();
    }
    const std::string& key = args[0].asString();
    std::string decodedData = decodeBase64(args[1].asString());

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (errorMessage) *errorMessage = "CryptAcquireContext failed";
        return Value::nilValue();
    }

    HCRYPTHASH hHash = 0;
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (const BYTE*)key.data(), (DWORD)key.length(), 0);

    HCRYPTKEY hKey = 0;
    if (!CryptDeriveKey(hProv, CALG_AES_256, hHash, 0, &hKey)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptDeriveKey failed";
        return Value::nilValue();
    }
    CryptDestroyHash(hHash);

    DWORD dataLen = (DWORD)decodedData.length();
    std::vector<BYTE> buffer(dataLen, 0);
    memcpy(buffer.data(), decodedData.data(), dataLen);

    if (!CryptDecrypt(hKey, 0, TRUE, 0, buffer.data(), &dataLen)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        if (errorMessage) *errorMessage = "CryptDecrypt failed";
        return Value::nilValue();
    }

    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);

    return Value::stringValue(std::string((char*)buffer.data(), dataLen));
}
