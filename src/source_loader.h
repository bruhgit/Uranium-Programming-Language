#ifndef uranium_source_loader_h
#define uranium_source_loader_h

#include <filesystem>
#include <string>

bool loadProgramWithImports(const std::filesystem::path& entryPath,
                            const std::filesystem::path& workingDirectory,
                            std::string* expandedSource,
                            std::string* errorMessage);

#endif
