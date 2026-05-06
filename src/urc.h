#ifndef uranium_urc_h
#define uranium_urc_h

#include "value.h"
#include <iosfwd>
#include <filesystem>
#include <string>

bool writeUrcStream(const FunctionPtr& function,
                    std::ostream& stream,
                    std::string* errorMessage);

bool readUrcStream(std::istream& stream,
                   FunctionPtr* function,
                   std::string* errorMessage);

bool writeUrcFile(const FunctionPtr& function,
                  const std::filesystem::path& path,
                  std::string* errorMessage);

bool readUrcFile(const std::filesystem::path& path,
                 FunctionPtr* function,
                 std::string* errorMessage);

bool writeUraFile(const FunctionPtr& function,
                  const std::filesystem::path& path,
                  const std::string& manifestText,
                  const std::string& entryPath,
                  std::string* errorMessage);

bool readUraFile(const std::filesystem::path& path,
                 FunctionPtr* function,
                 std::string* manifestText,
                 std::string* entryPath,
                 std::string* errorMessage);

std::filesystem::path compiledPathForSource(const std::filesystem::path& sourcePath,
                                            const std::filesystem::path& workingDirectory);

std::filesystem::path archivePathForPackage(const std::filesystem::path& packageRoot,
                                            const std::string& packageName);

#endif
