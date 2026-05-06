#ifndef uranium_umake_h
#define uranium_umake_h

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct UMakeTargetData {
    std::string name;
    std::vector<std::string> dependencies;
    std::vector<std::string> commands;
    int line = 0;
};

struct UMakeFileData {
    std::filesystem::path path;
    std::vector<std::string> targetOrder;
    std::unordered_map<std::string, UMakeTargetData> targets;
    std::unordered_map<std::string, std::string> variables;
};

bool findUMakeFile(const std::filesystem::path& rawPath,
                   const std::filesystem::path& executablePath,
                   std::filesystem::path* resolvedPath,
                   std::string* errorMessage);

bool loadUMakeFile(const std::filesystem::path& rawPath,
                   const std::filesystem::path& executablePath,
                   UMakeFileData* data,
                   std::string* errorMessage);

int listUMakeTargets(const std::filesystem::path& rawPath,
                     const std::filesystem::path& executablePath);

int runUMakeTarget(const std::filesystem::path& rawPath,
                   const std::string& targetName,
                   const std::filesystem::path& executablePath);

#endif
