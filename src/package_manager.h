#ifndef uranium_package_manager_h
#define uranium_package_manager_h

#include <filesystem>
#include <string>
#include <unordered_map>

struct PackageManifestData {
    std::string name;
    std::string version;
    std::string entry;
    std::string tests;
    std::unordered_map<std::string, std::string> dependencies;
    std::string rawText;
    std::filesystem::path packageRoot;
};

struct PackageLockEntryData {
    std::string name;
    std::string version;
    std::string entry;
    std::string integrity;
    std::unordered_map<std::string, std::string> dependencies;
};

struct PackageLockData {
    std::string name;
    std::string version;
    std::string registryPath;
    std::unordered_map<std::string, std::string> directDependencies;
    std::unordered_map<std::string, PackageLockEntryData> packages;
    std::string rawText;
    std::filesystem::path lockPath;
};

bool loadPackageManifestData(const std::filesystem::path& packageRoot,
                             PackageManifestData* manifest,
                             std::string* errorMessage);

bool loadPackageLockData(const std::filesystem::path& packageRoot,
                         PackageLockData* lockData,
                         std::string* errorMessage);

bool writePackageManifestData(const std::filesystem::path& packageRoot,
                              const PackageManifestData& manifest,
                              std::string* errorMessage);

bool writePackageLockData(const std::filesystem::path& packageRoot,
                          const PackageLockData& lockData,
                          std::string* errorMessage);

bool generatePackageLockData(const std::filesystem::path& packageRoot,
                             const std::filesystem::path& registryRoot,
                             PackageLockData* lockData,
                             std::string* errorMessage);

bool installPackageDependencies(const std::filesystem::path& packageRoot,
                                const std::filesystem::path& registryRoot,
                                PackageLockData* installedLock,
                                std::string* errorMessage);

bool updatePackageDependencies(const std::filesystem::path& packageRoot,
                               const std::filesystem::path& registryRoot,
                               PackageLockData* installedLock,
                               std::string* errorMessage);

bool removePackageDependency(const std::filesystem::path& packageRoot,
                             const std::filesystem::path& registryRoot,
                             const std::string& dependencyName,
                             PackageLockData* installedLock,
                             std::string* errorMessage);

bool initializePackageRegistry(const std::filesystem::path& registryRoot,
                               std::string* errorMessage);

bool publishPackageToRegistry(const std::filesystem::path& packageRoot,
                              const std::filesystem::path& registryRoot,
                              std::filesystem::path* publishedRoot,
                              std::string* errorMessage);

bool findPackageRootForPath(const std::filesystem::path& startPath,
                            std::filesystem::path* packageRoot);

std::filesystem::path packageLockFilePath(const std::filesystem::path& packageRoot);

std::filesystem::path defaultPackageRegistryPath(const std::filesystem::path& startPath);

std::filesystem::path installedDependencyRoot(const std::filesystem::path& ownerPackageRoot,
                                              const std::string& packageName,
                                              const std::string& version);

bool tryResolveInstalledPackageImport(const std::string& spec,
                                      const std::filesystem::path& importerPath,
                                      const std::filesystem::path& workingDirectory,
                                      bool* resolved,
                                      std::filesystem::path* resolvedPath,
                                      std::string* errorMessage);

#endif
