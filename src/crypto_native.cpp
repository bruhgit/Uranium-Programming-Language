#include "crypto_native.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#elif defined(URANIUM_HAS_OPENSSL)
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

namespace {

const std::string kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

std::string encodeBase64(const unsigned char* bytesToEncode, std::size_t inputLength) {
    std::string result;
    int index = 0;
    unsigned char group3[3];
    unsigned char group4[4];

    while (inputLength--) {
        group3[index++] = *(bytesToEncode++);
        if (index == 3) {
            group4[0] = (group3[0] & 0xfc) >> 2;
            group4[1] = ((group3[0] & 0x03) << 4) + ((group3[1] & 0xf0) >> 4);
            group4[2] = ((group3[1] & 0x0f) << 2) + ((group3[2] & 0xc0) >> 6);
            group4[3] = group3[2] & 0x3f;

            for (index = 0; index < 4; ++index) {
                result += kBase64Chars[group4[index]];
            }
            index = 0;
        }
    }

    if (index > 0) {
        for (int fill = index; fill < 3; ++fill) {
            group3[fill] = '\0';
        }

        group4[0] = (group3[0] & 0xfc) >> 2;
        group4[1] = ((group3[0] & 0x03) << 4) + ((group3[1] & 0xf0) >> 4);
        group4[2] = ((group3[1] & 0x0f) << 2) + ((group3[2] & 0xc0) >> 6);
        group4[3] = group3[2] & 0x3f;

        for (int output = 0; output < index + 1; ++output) {
            result += kBase64Chars[group4[output]];
        }

        while (index++ < 3) {
            result += '=';
        }
    }

    return result;
}

std::string decodeBase64(const std::string& encoded) {
    auto isBase64 = [](unsigned char character) {
        return std::isalnum(character) || character == '+' || character == '/';
    };

    int inputLength = static_cast<int>(encoded.size());
    int index = 0;
    int cursor = 0;
    unsigned char group4[4];
    unsigned char group3[3];
    std::string result;

    while (inputLength-- && encoded[static_cast<std::size_t>(cursor)] != '=' &&
           isBase64(static_cast<unsigned char>(encoded[static_cast<std::size_t>(cursor)]))) {
        group4[index++] = static_cast<unsigned char>(encoded[static_cast<std::size_t>(cursor)]);
        cursor++;
        if (index == 4) {
            for (index = 0; index < 4; ++index) {
                group4[index] = static_cast<unsigned char>(kBase64Chars.find(group4[index]));
            }

            group3[0] = (group4[0] << 2) + ((group4[1] & 0x30) >> 4);
            group3[1] = ((group4[1] & 0x0f) << 4) + ((group4[2] & 0x3c) >> 2);
            group3[2] = ((group4[2] & 0x03) << 6) + group4[3];

            for (index = 0; index < 3; ++index) {
                result += static_cast<char>(group3[index]);
            }
            index = 0;
        }
    }

    if (index > 0) {
        for (int fill = index; fill < 4; ++fill) {
            group4[fill] = 0;
        }
        for (int position = 0; position < 4; ++position) {
            group4[position] =
                static_cast<unsigned char>(kBase64Chars.find(group4[position]));
        }

        group3[0] = (group4[0] << 2) + ((group4[1] & 0x30) >> 4);
        group3[1] = ((group4[1] & 0x0f) << 4) + ((group4[2] & 0x3c) >> 2);
        group3[2] = ((group4[2] & 0x03) << 6) + group4[3];

        for (int output = 0; output < index - 1; ++output) {
            result += static_cast<char>(group3[output]);
        }
    }

    return result;
}

std::string hexEncode(const std::vector<unsigned char>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0F]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

bool sha256Bytes(const std::string& input,
                 std::vector<unsigned char>* digest,
                 std::string* errorMessage) {
#ifdef _WIN32
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContext(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return setError(errorMessage, "CryptAcquireContext failed");
    }
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptCreateHash failed");
    }
    if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()),
                       static_cast<DWORD>(input.size()), 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptHashData failed");
    }

    DWORD hashSize = 0;
    DWORD sizeSize = sizeof(hashSize);
    CryptGetHashParam(hash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hashSize), &sizeSize, 0);
    digest->assign(hashSize, 0);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest->data(), &hashSize, 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptGetHashParam failed");
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return true;
#elif defined(__APPLE__)
    digest->assign(CC_SHA256_DIGEST_LENGTH, 0);
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), digest->data());
    return true;
#elif defined(URANIUM_HAS_OPENSSL)
    digest->assign(SHA256_DIGEST_LENGTH, 0);
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()),
               input.size(), digest->data()) == nullptr) {
        return setError(errorMessage, "OpenSSL SHA256 failed");
    }
    return true;
#else
    (void)input;
    return setError(errorMessage, "SHA-256 is not available on this platform build.");
#endif
}

bool aesEncryptBytes(const std::string& keyText,
                     const std::string& plainText,
                     std::string* encodedCipherText,
                     std::string* errorMessage) {
#ifdef _WIN32
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContext(&provider, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        return setError(errorMessage, "CryptAcquireContext failed");
    }

    HCRYPTHASH hash = 0;
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptCreateHash failed");
    }
    CryptHashData(hash, reinterpret_cast<const BYTE*>(keyText.data()),
                  static_cast<DWORD>(keyText.size()), 0);

    HCRYPTKEY key = 0;
    if (!CryptDeriveKey(provider, CALG_AES_256, hash, 0, &key)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptDeriveKey failed");
    }
    CryptDestroyHash(hash);

    DWORD dataLength = static_cast<DWORD>(plainText.size());
    DWORD bufferLength = dataLength + 32;
    std::vector<BYTE> buffer(bufferLength, 0);
    std::memcpy(buffer.data(), plainText.data(), plainText.size());

    if (!CryptEncrypt(key, 0, TRUE, 0, buffer.data(), &dataLength, bufferLength)) {
        CryptDestroyKey(key);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptEncrypt failed");
    }

    CryptDestroyKey(key);
    CryptReleaseContext(provider, 0);
    *encodedCipherText = encodeBase64(buffer.data(), dataLength);
    return true;
#elif defined(__APPLE__)
    std::vector<unsigned char> digest;
    if (!sha256Bytes(keyText, &digest, errorMessage)) {
        return false;
    }
    unsigned char iv[kCCBlockSizeAES128] = {};
    std::size_t outLength = plainText.size() + kCCBlockSizeAES128;
    std::vector<unsigned char> output(outLength, 0);
    std::size_t moved = 0;
    CCCryptorStatus status =
        CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                digest.data(), digest.size(), iv,
                plainText.data(), plainText.size(),
                output.data(), output.size(), &moved);
    if (status != kCCSuccess) {
        return setError(errorMessage, "CommonCrypto AES encrypt failed");
    }
    *encodedCipherText = encodeBase64(output.data(), moved);
    return true;
#elif defined(URANIUM_HAS_OPENSSL)
    std::vector<unsigned char> digest;
    if (!sha256Bytes(keyText, &digest, errorMessage)) {
        return false;
    }
    unsigned char iv[16] = {};
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return setError(errorMessage, "OpenSSL cipher context allocation failed");
    }

    std::vector<unsigned char> output(plainText.size() + 32, 0);
    int outLength1 = 0;
    int outLength2 = 0;
    bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_cbc(), nullptr,
                                 digest.data(), iv) == 1 &&
              EVP_EncryptUpdate(context, output.data(), &outLength1,
                                reinterpret_cast<const unsigned char*>(plainText.data()),
                                static_cast<int>(plainText.size())) == 1 &&
              EVP_EncryptFinal_ex(context, output.data() + outLength1, &outLength2) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        return setError(errorMessage, "OpenSSL AES encrypt failed");
    }
    *encodedCipherText =
        encodeBase64(output.data(), static_cast<std::size_t>(outLength1 + outLength2));
    return true;
#else
    (void)keyText;
    (void)plainText;
    (void)encodedCipherText;
    return setError(errorMessage, "AES encryption is not available on this platform build.");
#endif
}

bool aesDecryptBytes(const std::string& keyText,
                     const std::string& encodedCipherText,
                     std::string* plainText,
                     std::string* errorMessage) {
#ifdef _WIN32
    std::string decoded = decodeBase64(encodedCipherText);

    HCRYPTPROV provider = 0;
    if (!CryptAcquireContext(&provider, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        return setError(errorMessage, "CryptAcquireContext failed");
    }

    HCRYPTHASH hash = 0;
    CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    CryptHashData(hash, reinterpret_cast<const BYTE*>(keyText.data()),
                  static_cast<DWORD>(keyText.size()), 0);

    HCRYPTKEY key = 0;
    if (!CryptDeriveKey(provider, CALG_AES_256, hash, 0, &key)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptDeriveKey failed");
    }
    CryptDestroyHash(hash);

    DWORD dataLength = static_cast<DWORD>(decoded.size());
    std::vector<BYTE> buffer(decoded.begin(), decoded.end());
    if (!CryptDecrypt(key, 0, TRUE, 0, buffer.data(), &dataLength)) {
        CryptDestroyKey(key);
        CryptReleaseContext(provider, 0);
        return setError(errorMessage, "CryptDecrypt failed");
    }

    CryptDestroyKey(key);
    CryptReleaseContext(provider, 0);
    *plainText = std::string(reinterpret_cast<char*>(buffer.data()), dataLength);
    return true;
#elif defined(__APPLE__)
    std::vector<unsigned char> digest;
    if (!sha256Bytes(keyText, &digest, errorMessage)) {
        return false;
    }
    std::string decoded = decodeBase64(encodedCipherText);
    unsigned char iv[kCCBlockSizeAES128] = {};
    std::vector<unsigned char> output(decoded.size() + kCCBlockSizeAES128, 0);
    std::size_t moved = 0;
    CCCryptorStatus status =
        CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                digest.data(), digest.size(), iv,
                decoded.data(), decoded.size(),
                output.data(), output.size(), &moved);
    if (status != kCCSuccess) {
        return setError(errorMessage, "CommonCrypto AES decrypt failed");
    }
    *plainText = std::string(reinterpret_cast<char*>(output.data()), moved);
    return true;
#elif defined(URANIUM_HAS_OPENSSL)
    std::vector<unsigned char> digest;
    if (!sha256Bytes(keyText, &digest, errorMessage)) {
        return false;
    }
    std::string decoded = decodeBase64(encodedCipherText);
    unsigned char iv[16] = {};
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return setError(errorMessage, "OpenSSL cipher context allocation failed");
    }

    std::vector<unsigned char> output(decoded.size() + 32, 0);
    int outLength1 = 0;
    int outLength2 = 0;
    bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr,
                                 digest.data(), iv) == 1 &&
              EVP_DecryptUpdate(context, output.data(), &outLength1,
                                reinterpret_cast<const unsigned char*>(decoded.data()),
                                static_cast<int>(decoded.size())) == 1 &&
              EVP_DecryptFinal_ex(context, output.data() + outLength1, &outLength2) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        return setError(errorMessage, "OpenSSL AES decrypt failed");
    }
    *plainText = std::string(reinterpret_cast<char*>(output.data()), outLength1 + outLength2);
    return true;
#else
    (void)keyText;
    (void)encodedCipherText;
    (void)plainText;
    return setError(errorMessage, "AES decryption is not available on this platform build.");
#endif
}

} // namespace

Value nativeCryptoBase64Encode(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected a string to encode";
        }
        return Value::nilValue();
    }

    const std::string& input = args[0].asString();
    return Value::stringValue(
        encodeBase64(reinterpret_cast<const unsigned char*>(input.data()), input.size()));
}

Value nativeCryptoBase64Decode(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected a string to decode";
        }
        return Value::nilValue();
    }

    return Value::stringValue(decodeBase64(args[0].asString()));
}

Value nativeCryptoHashSha256(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected a string to hash";
        }
        return Value::nilValue();
    }

    std::vector<unsigned char> digest;
    if (!sha256Bytes(args[0].asString(), &digest, errorMessage)) {
        return Value::nilValue();
    }

    return Value::stringValue(hexEncode(digest));
}

Value nativeCryptoAesEncrypt(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected key and data to encrypt";
        }
        return Value::nilValue();
    }

    std::string encodedCipherText;
    if (!aesEncryptBytes(args[0].asString(), args[1].asString(), &encodedCipherText,
                         errorMessage)) {
        return Value::nilValue();
    }
    return Value::stringValue(encodedCipherText);
}

Value nativeCryptoAesDecrypt(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isString()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected key and base64 string to decrypt";
        }
        return Value::nilValue();
    }

    std::string plainText;
    if (!aesDecryptBytes(args[0].asString(), args[1].asString(), &plainText, errorMessage)) {
        return Value::nilValue();
    }
    return Value::stringValue(plainText);
}
