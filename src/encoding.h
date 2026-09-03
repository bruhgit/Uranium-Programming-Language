#ifndef URANIUM_ENCODING_H
#define URANIUM_ENCODING_H

#include <string>
#include <vector>

namespace uranium {
namespace encoding {

std::string decodeUTF16LE(const std::string& input);
std::string decodeUTF16BE(const std::string& input);
std::string decodeUTF32LE(const std::string& input);
std::string decodeUTF32BE(const std::string& input);
std::string decodeISO8859_1(const std::string& input);

std::string encodeUTF16LE(const std::string& input);
std::string encodeUTF16BE(const std::string& input);
std::string encodeUTF32LE(const std::string& input);
std::string encodeUTF32BE(const std::string& input);
std::string encodeISO8859_1(const std::string& input);

bool isValidUTF8(const std::string& input);

// Will detect BOM and return UTF-8 string, stripping BOM. 
// If no BOM, checks if valid UTF-8. If not, assumes ISO-8859-1.
std::string decodeSourceFile(const std::string& rawBytes);

}
}

#endif // URANIUM_ENCODING_H
