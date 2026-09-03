#include "encoding.h"
#include <cstdint>

namespace uranium {
namespace encoding {

static void appendUTF8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        appendUTF8(out, 0xFFFD); // Replacement char
    }
}

std::string decodeUTF16LE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i + 1 < input.size()) {
        uint16_t w1 = static_cast<unsigned char>(input[i]) | (static_cast<unsigned char>(input[i+1]) << 8);
        i += 2;
        if (w1 >= 0xD800 && w1 <= 0xDBFF) { 
            if (i + 1 < input.size()) {
                uint16_t w2 = static_cast<unsigned char>(input[i]) | (static_cast<unsigned char>(input[i+1]) << 8);
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) { 
                    i += 2;
                    uint32_t cp = 0x10000 + ((w1 - 0xD800) << 10) + (w2 - 0xDC00);
                    appendUTF8(out, cp);
                    continue;
                }
            }
        }
        appendUTF8(out, w1);
    }
    return out;
}

std::string decodeUTF16BE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i + 1 < input.size()) {
        uint16_t w1 = (static_cast<unsigned char>(input[i]) << 8) | static_cast<unsigned char>(input[i+1]);
        i += 2;
        if (w1 >= 0xD800 && w1 <= 0xDBFF) { 
            if (i + 1 < input.size()) {
                uint16_t w2 = (static_cast<unsigned char>(input[i]) << 8) | static_cast<unsigned char>(input[i+1]);
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) { 
                    i += 2;
                    uint32_t cp = 0x10000 + ((w1 - 0xD800) << 10) + (w2 - 0xDC00);
                    appendUTF8(out, cp);
                    continue;
                }
            }
        }
        appendUTF8(out, w1);
    }
    return out;
}

std::string decodeUTF32LE(const std::string& input) {
    std::string out;
    for (size_t i = 0; i + 3 < input.size(); i += 4) {
        uint32_t cp = static_cast<unsigned char>(input[i]) | 
                      (static_cast<unsigned char>(input[i+1]) << 8) | 
                      (static_cast<unsigned char>(input[i+2]) << 16) | 
                      (static_cast<unsigned char>(input[i+3]) << 24);
        appendUTF8(out, cp);
    }
    return out;
}

std::string decodeUTF32BE(const std::string& input) {
    std::string out;
    for (size_t i = 0; i + 3 < input.size(); i += 4) {
        uint32_t cp = (static_cast<unsigned char>(input[i]) << 24) | 
                      (static_cast<unsigned char>(input[i+1]) << 16) | 
                      (static_cast<unsigned char>(input[i+2]) << 8) | 
                      static_cast<unsigned char>(input[i+3]);
        appendUTF8(out, cp);
    }
    return out;
}

std::string decodeISO8859_1(const std::string& input) {
    std::string out;
    for (char c : input) {
        appendUTF8(out, static_cast<unsigned char>(c));
    }
    return out;
}

// UTF-8 decoding helper for encode functions
static uint32_t nextUTF8CodePoint(const std::string& input, size_t& i) {
    if (i >= input.size()) return 0;
    unsigned char c = input[i++];
    if (c <= 0x7F) return c;
    if ((c & 0xE0) == 0xC0) {
        if (i >= input.size()) return 0xFFFD;
        unsigned char c2 = input[i++];
        return ((c & 0x1F) << 6) | (c2 & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        if (i + 1 >= input.size()) { i = input.size(); return 0xFFFD; }
        unsigned char c2 = input[i++];
        unsigned char c3 = input[i++];
        return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {
        if (i + 2 >= input.size()) { i = input.size(); return 0xFFFD; }
        unsigned char c2 = input[i++];
        unsigned char c3 = input[i++];
        unsigned char c4 = input[i++];
        return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
    }
    return 0xFFFD;
}

std::string encodeUTF16LE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        uint32_t cp = nextUTF8CodePoint(input, i);
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(cp & 0xFF));
            out.push_back(static_cast<char>((cp >> 8) & 0xFF));
        } else {
            cp -= 0x10000;
            uint16_t w1 = 0xD800 | ((cp >> 10) & 0x3FF);
            uint16_t w2 = 0xDC00 | (cp & 0x3FF);
            out.push_back(static_cast<char>(w1 & 0xFF));
            out.push_back(static_cast<char>((w1 >> 8) & 0xFF));
            out.push_back(static_cast<char>(w2 & 0xFF));
            out.push_back(static_cast<char>((w2 >> 8) & 0xFF));
        }
    }
    return out;
}

std::string encodeUTF16BE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        uint32_t cp = nextUTF8CodePoint(input, i);
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>((cp >> 8) & 0xFF));
            out.push_back(static_cast<char>(cp & 0xFF));
        } else {
            cp -= 0x10000;
            uint16_t w1 = 0xD800 | ((cp >> 10) & 0x3FF);
            uint16_t w2 = 0xDC00 | (cp & 0x3FF);
            out.push_back(static_cast<char>((w1 >> 8) & 0xFF));
            out.push_back(static_cast<char>(w1 & 0xFF));
            out.push_back(static_cast<char>((w2 >> 8) & 0xFF));
            out.push_back(static_cast<char>(w2 & 0xFF));
        }
    }
    return out;
}

std::string encodeUTF32LE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        uint32_t cp = nextUTF8CodePoint(input, i);
        out.push_back(static_cast<char>(cp & 0xFF));
        out.push_back(static_cast<char>((cp >> 8) & 0xFF));
        out.push_back(static_cast<char>((cp >> 16) & 0xFF));
        out.push_back(static_cast<char>((cp >> 24) & 0xFF));
    }
    return out;
}

std::string encodeUTF32BE(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        uint32_t cp = nextUTF8CodePoint(input, i);
        out.push_back(static_cast<char>((cp >> 24) & 0xFF));
        out.push_back(static_cast<char>((cp >> 16) & 0xFF));
        out.push_back(static_cast<char>((cp >> 8) & 0xFF));
        out.push_back(static_cast<char>(cp & 0xFF));
    }
    return out;
}

std::string encodeISO8859_1(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        uint32_t cp = nextUTF8CodePoint(input, i);
        if (cp <= 0xFF) {
            out.push_back(static_cast<char>(cp));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

bool isValidUTF8(const std::string& input) {
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = input[i];
        int numBytes = 0;
        if (c <= 0x7F) numBytes = 1;
        else if ((c & 0xE0) == 0xC0) numBytes = 2;
        else if ((c & 0xF0) == 0xE0) numBytes = 3;
        else if ((c & 0xF8) == 0xF0) numBytes = 4;
        else return false;
        
        if (i + numBytes > input.size()) return false;
        for (int j = 1; j < numBytes; j++) {
            if ((input[i + j] & 0xC0) != 0x80) return false;
        }
        i += numBytes;
    }
    return true;
}

std::string decodeSourceFile(const std::string& rawBytes) {
    if (rawBytes.size() >= 3 && 
        static_cast<unsigned char>(rawBytes[0]) == 0xEF &&
        static_cast<unsigned char>(rawBytes[1]) == 0xBB &&
        static_cast<unsigned char>(rawBytes[2]) == 0xBF) {
        return rawBytes.substr(3); // UTF-8 BOM
    }
    if (rawBytes.size() >= 4 && 
        static_cast<unsigned char>(rawBytes[0]) == 0x00 &&
        static_cast<unsigned char>(rawBytes[1]) == 0x00 &&
        static_cast<unsigned char>(rawBytes[2]) == 0xFE &&
        static_cast<unsigned char>(rawBytes[3]) == 0xFF) {
        return decodeUTF32BE(rawBytes.substr(4));
    }
    if (rawBytes.size() >= 4 && 
        static_cast<unsigned char>(rawBytes[0]) == 0xFF &&
        static_cast<unsigned char>(rawBytes[1]) == 0xFE &&
        static_cast<unsigned char>(rawBytes[2]) == 0x00 &&
        static_cast<unsigned char>(rawBytes[3]) == 0x00) {
        return decodeUTF32LE(rawBytes.substr(4));
    }
    if (rawBytes.size() >= 2 && 
        static_cast<unsigned char>(rawBytes[0]) == 0xFE &&
        static_cast<unsigned char>(rawBytes[1]) == 0xFF) {
        return decodeUTF16BE(rawBytes.substr(2));
    }
    if (rawBytes.size() >= 2 && 
        static_cast<unsigned char>(rawBytes[0]) == 0xFF &&
        static_cast<unsigned char>(rawBytes[1]) == 0xFE) {
        return decodeUTF16LE(rawBytes.substr(2));
    }
    
    if (isValidUTF8(rawBytes)) {
        return rawBytes;
    }
    
    return decodeISO8859_1(rawBytes);
}

}
}
