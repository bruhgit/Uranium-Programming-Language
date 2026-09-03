#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "common.h"
#include "compiler.h"
#include "package_manager.h"
#include "source_loader.h"
#include "tooling.h"
#include "umake.h"
#include "system_native.h"
#include "urc.h"
#include "vm.h"
#include "native_jit.h"
#include "optimizer.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

static bool pathExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode);
}

static bool fileExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_regular_file(path, errorCode);
}

static bool directoryExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_directory(path, errorCode);
}

static bool isDirectoryEmpty(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::is_empty(path, errorCode);
}

static bool isAbsolutePath(const std::filesystem::path& path) {
    if (path.empty()) return false;
    if (path.is_absolute()) return true;
    std::string s = path.generic_string();
    if (s[0] == '/') return true;
    return false;
}

static std::filesystem::path canonicalizePath(const std::filesystem::path& path) {
    std::error_code errorCode;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        return std::filesystem::absolute(path);
    }

    return canonical;
}

static void repl(VM& vm) {
    std::string line;
    for (;;) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }
        vm.interpret(line.c_str());
    }
}

static std::string lowerCase(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

static bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

static bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::filesystem::path searchUpwardForFile(const std::filesystem::path& startDirectory,
                                                 const std::filesystem::path& relativePath) {
    if (startDirectory.empty() || relativePath.empty() || isAbsolutePath(relativePath)) {
        return {};
    }

    std::filesystem::path current = canonicalizePath(startDirectory);
    for (;;) {
        std::filesystem::path candidate = current / relativePath;
        if (fileExists(candidate)) {
            return canonicalizePath(candidate);
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static std::filesystem::path searchUpwardForExistingPath(const std::filesystem::path& startDirectory,
                                                         const std::filesystem::path& relativePath) {
    if (startDirectory.empty() || relativePath.empty() || isAbsolutePath(relativePath)) {
        return {};
    }

    std::filesystem::path current = canonicalizePath(startDirectory);
    for (;;) {
        std::filesystem::path candidate = current / relativePath;
        if (pathExists(candidate)) {
            return canonicalizePath(candidate);
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static std::filesystem::path resolveInputPath(const std::filesystem::path& rawPath,
                                              const std::filesystem::path& executablePath) {
    if (isAbsolutePath(rawPath)) {
        return canonicalizePath(rawPath);
    }

    std::filesystem::path currentCandidate = std::filesystem::current_path() / rawPath;
    if (fileExists(currentCandidate)) {
        return canonicalizePath(currentCandidate);
    }

    std::filesystem::path fromCurrent = searchUpwardForFile(std::filesystem::current_path(), rawPath);
    if (!fromCurrent.empty()) {
        return fromCurrent;
    }

    std::filesystem::path executableDirectory = canonicalizePath(executablePath).parent_path();
    std::filesystem::path fromExecutable = searchUpwardForFile(executableDirectory, rawPath);
    if (!fromExecutable.empty()) {
        return fromExecutable;
    }

    return currentCandidate;
}

static std::filesystem::path resolveExistingPath(const std::filesystem::path& rawPath,
                                                 const std::filesystem::path& executablePath) {
    if (isAbsolutePath(rawPath)) {
        return canonicalizePath(rawPath);
    }

    std::filesystem::path currentCandidate = std::filesystem::current_path() / rawPath;
    if (pathExists(currentCandidate)) {
        return canonicalizePath(currentCandidate);
    }

    std::filesystem::path fromCurrent =
        searchUpwardForExistingPath(std::filesystem::current_path(), rawPath);
    if (!fromCurrent.empty()) {
        return fromCurrent;
    }

    std::filesystem::path executableDirectory = canonicalizePath(executablePath).parent_path();
    std::filesystem::path fromExecutable =
        searchUpwardForExistingPath(executableDirectory, rawPath);
    if (!fromExecutable.empty()) {
        return fromExecutable;
    }

    return currentCandidate;
}

static std::filesystem::path searchUpwardForWorkspaceRoot(std::filesystem::path start) {
    if (start.empty()) {
        return {};
    }

    std::filesystem::path current = canonicalizePath(start);
    for (;;) {
        if (fileExists(current / "uranium.pkg")) {
            return current;
        }

        if (directoryExists(current / "urlib")) {
            return current;
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static std::filesystem::path findWorkspaceRoot(const std::filesystem::path& resolvedSourcePath,
                                               const std::filesystem::path& executablePath) {
    std::filesystem::path fromSource =
        searchUpwardForWorkspaceRoot(resolvedSourcePath.parent_path());
    if (!fromSource.empty()) {
        return fromSource;
    }

    std::filesystem::path fromCurrent =
        searchUpwardForWorkspaceRoot(std::filesystem::current_path());
    if (!fromCurrent.empty()) {
        return fromCurrent;
    }

    std::filesystem::path fromExecutable =
        searchUpwardForWorkspaceRoot(canonicalizePath(executablePath).parent_path());
    if (!fromExecutable.empty()) {
        return fromExecutable;
    }

    return resolvedSourcePath.parent_path().empty()
               ? canonicalizePath(std::filesystem::current_path())
               : canonicalizePath(resolvedSourcePath.parent_path());
}

static std::string displayPath(const std::filesystem::path& path) {
    return path.generic_string();
}

static std::string relativeDisplayPath(const std::filesystem::path& path,
                                       const std::filesystem::path& base) {
    std::error_code errorCode;
    std::filesystem::path relative = std::filesystem::relative(path, base, errorCode);
    if (errorCode || relative.empty()) {
        return displayPath(path);
    }

    return relative.generic_string();
}

static int compileSourceProgram(const std::filesystem::path& path,
                                const std::filesystem::path& workspaceRoot,
                                FunctionPtr* function,
                                std::string* errorMessage) {
    std::string source;
    if (!loadProgramWithImports(path, workspaceRoot, &source, errorMessage)) {
        return 74;
    }

    // Debug: write the expanded source to a file to inspect it
    {
        std::ofstream dbgFile(workspaceRoot / "expanded_debug.ur");
        if (dbgFile.is_open()) {
            dbgFile << source;
            dbgFile.close();
        }
    }

    if (!compile(source.c_str(), function, path.string())) {
        return 65;
    }

    return 0;
}

static int executeFunction(VM& vm, const FunctionPtr& function) {
    InterpretResult result = vm.interpret(function);
    if (result == INTERPRET_COMPILE_ERROR) {
        return 65;
    }

    if (result == INTERPRET_RUNTIME_ERROR) {
        return 70;
    }

    return 0;
}

static int runCompiledFile(VM& vm, const std::filesystem::path& path) {
    FunctionPtr function = nullptr;
    std::string errorMessage;
    if (!readUrcFile(path, &function, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    return executeFunction(vm, function);
}

static int runPackageFile(VM& vm, const std::filesystem::path& path) {
    FunctionPtr function = nullptr;
    std::string manifestText;
    std::string entryPath;
    std::string errorMessage;
    if (!readUraFile(path, &function, &manifestText, &entryPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    return executeFunction(vm, function);
}

static std::string syntheticArchiveManifest(const std::filesystem::path& sourcePath,
                                            const std::string& entryPath) {
    std::string packageName = sourcePath.stem().string();
    if (packageName.empty()) {
        packageName = "uranium-app";
    }

    return "{\n"
           "  \"name\": \"" + packageName + "\",\n"
           "  \"version\": \"0.0.0-aot\",\n"
           "  \"entry\": \"" + entryPath + "\"\n"
           "}\n";
}

static int loadRunnableArchivePayload(const std::filesystem::path& path,
                                      const std::filesystem::path& executablePath,
                                      FunctionPtr* function,
                                      std::string* manifestText,
                                      std::string* entryPath,
                                      std::filesystem::path* workspaceRoot,
                                      std::string* errorMessage) {
    std::string extension = lowerCase(path.extension().string());
    if (extension == ".ura") {
        if (workspaceRoot != nullptr) {
            workspaceRoot->clear();
        }
        if (!readUraFile(path, function, manifestText, entryPath, errorMessage)) {
            return 74;
        }
        return 0;
    }

    if (extension == ".urc") {
        if (workspaceRoot != nullptr) {
            workspaceRoot->clear();
        }
        if (!readUrcFile(path, function, errorMessage)) {
            return 74;
        }

        std::filesystem::path inferredEntry = path.filename();
        inferredEntry.replace_extension(".ur");
        if (entryPath != nullptr) {
            *entryPath = inferredEntry.generic_string();
        }
        if (manifestText != nullptr) {
            *manifestText = syntheticArchiveManifest(path, inferredEntry.generic_string());
        }
        return 0;
    }

    std::filesystem::path resolvedWorkspaceRoot = findWorkspaceRoot(path, executablePath);
    int compileStatus =
        compileSourceProgram(path, resolvedWorkspaceRoot, function, errorMessage);
    if (compileStatus != 0) {
        return compileStatus;
    }

    std::string resolvedEntryPath = relativeDisplayPath(path, resolvedWorkspaceRoot);
    if (entryPath != nullptr) {
        *entryPath = resolvedEntryPath;
    }
    if (manifestText != nullptr) {
        *manifestText = syntheticArchiveManifest(path, resolvedEntryPath);
    }
    if (workspaceRoot != nullptr) {
        *workspaceRoot = resolvedWorkspaceRoot;
    }
    return 0;
}

static std::filesystem::path defaultAotOutputPath(const std::filesystem::path& inputPath) {
    std::filesystem::path binaryName = inputPath.filename();
    binaryName.replace_extension(".exe");
    return canonicalizePath(std::filesystem::current_path() / "compiled" / binaryName);
}

static int compileRunnableToBinary(const std::filesystem::path& rawInputPath,
                                   const std::filesystem::path& rawOutputPath,
                                   const std::filesystem::path& executablePath) {
    std::filesystem::path inputPath = resolveInputPath(rawInputPath, executablePath);
    FunctionPtr function = nullptr;
    std::string manifestText;
    std::string entryPath;
    std::string errorMessage;
    std::filesystem::path workspaceRoot;

    int loadStatus = loadRunnableArchivePayload(inputPath, executablePath, &function,
                                                &manifestText, &entryPath, &workspaceRoot,
                                                &errorMessage);
    if (loadStatus != 0) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        }
        return loadStatus;
    }

    if (lowerCase(inputPath.extension().string()) == ".ur") {
        std::filesystem::path compiledUrcPath = compiledPathForSource(inputPath, workspaceRoot);
        if (!writeUrcFile(function, compiledUrcPath, &errorMessage)) {
            std::cerr << errorMessage << std::endl;
            return 74;
        }
    }

    std::ostringstream archiveStream(std::ios::binary | std::ios::out);
    if (!writeUraStream(function, archiveStream, manifestText, entryPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::filesystem::path outputPath;
    if (rawOutputPath.empty()) {
        outputPath = defaultAotOutputPath(inputPath);
    } else {
        outputPath = isAbsolutePath(rawOutputPath)
                         ? rawOutputPath
                         : canonicalizePath(std::filesystem::current_path() / rawOutputPath);
        if (directoryExists(outputPath)) {
            std::filesystem::path binaryName = inputPath.filename();
            binaryName.replace_extension(".exe");
            outputPath /= binaryName;
        }
        if (outputPath.extension().empty()) {
            outputPath.replace_extension(".exe");
        }
    }

    if (!writeEmbeddedAotBinary(executablePath, archiveStream.str(), outputPath, std::filesystem::path(g_compileIconPath),
                                 &errorMessage)) {
         std::cerr << errorMessage << std::endl;
         return 74;
     }

    std::cout << "Compiled " << displayPath(inputPath)
              << " -> " << displayPath(outputPath) << std::endl;
    return 0;
}

static int maybeRunEmbeddedProgram(const std::filesystem::path& executablePath,
                                   int argc,
                                   const char* argv[],
                                   bool* handled) {
    if (handled != nullptr) {
        *handled = false;
    }

    std::string payload;
    std::string errorMessage;
    bool found = false;
    if (!readEmbeddedAotPayload(executablePath, &payload, &found, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        if (handled != nullptr) {
            *handled = true;
        }
        return 74;
    }

    if (!found) {
        return 0;
    }

    FunctionPtr function = nullptr;
    std::string manifestText;
    std::string entryPath;
    std::istringstream stream(payload, std::ios::binary | std::ios::in);
    if (!readUraStream(stream, &function, &manifestText, &entryPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        if (handled != nullptr) {
            *handled = true;
        }
        return 74;
    }

    std::vector<std::string> scriptArgs;
    for (int index = 1; index < argc; ++index) {
        scriptArgs.push_back(argv[index]);
    }

    configureRuntimeProcessContext(canonicalizePath(executablePath).generic_string(),
                                   canonicalizePath(executablePath).generic_string(),
                                   scriptArgs);

    if (handled != nullptr) {
        *handled = true;
    }

    VM vm;
    return executeFunction(vm, function);
}

static int runSourceFile(VM& vm,
                         const std::filesystem::path& path,
                         const std::filesystem::path& workspaceRoot) {
    FunctionPtr function = nullptr;
    std::string errorMessage;
    int compileStatus = compileSourceProgram(path, workspaceRoot, &function, &errorMessage);
    if (compileStatus != 0) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        }
        return compileStatus;
    }

    std::filesystem::path outputPath = compiledPathForSource(path, workspaceRoot);

    if (!writeUrcFile(function, outputPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    return executeFunction(vm, function);
}

static int runFile(VM& vm,
                   const std::filesystem::path& rawPath,
                   const std::filesystem::path& executablePath,
                   const std::vector<std::string>& scriptArgs) {
    std::filesystem::path path = resolveInputPath(rawPath, executablePath);
    configureRuntimeProcessContext(canonicalizePath(executablePath).generic_string(),
                                   canonicalizePath(path).generic_string(),
                                   scriptArgs);
    std::string extension = lowerCase(path.extension().string());

    if (extension == ".urc") {
        return runCompiledFile(vm, path);
    }

    if (extension == ".ura") {
        return runPackageFile(vm, path);
    }

    std::filesystem::path workspaceRoot = findWorkspaceRoot(path, executablePath);
    return runSourceFile(vm, path, workspaceRoot);
}

static bool resolvePackageRoot(const std::filesystem::path& rawTarget,
                               const std::filesystem::path& executablePath,
                               std::filesystem::path* packageRoot,
                               std::string* errorMessage) {
    std::filesystem::path resolved = resolveExistingPath(rawTarget, executablePath);
    if (!pathExists(resolved)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Package target does not exist: " + displayPath(resolved);
        }
        return false;
    }

    if (fileExists(resolved) && lowerCase(resolved.filename().string()) == "uranium.pkg") {
        *packageRoot = canonicalizePath(resolved.parent_path());
        return true;
    }

    if (directoryExists(resolved) && fileExists(resolved / "uranium.pkg")) {
        *packageRoot = canonicalizePath(resolved);
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = "Expected a package directory or a uranium.pkg file, got '" +
                        displayPath(resolved) + "'.";
    }
    return false;
}

static std::filesystem::path resolveRegistryRoot(const std::filesystem::path& rawRegistry,
                                                 const std::filesystem::path& basePath,
                                                 const std::filesystem::path& executablePath) {
    if (!rawRegistry.empty()) {
        return resolveExistingPath(rawRegistry, executablePath);
    }

    std::filesystem::path packageRoot;
    if (findPackageRootForPath(basePath, &packageRoot)) {
        std::filesystem::path lockPath = packageLockFilePath(packageRoot);
        if (fileExists(lockPath)) {
            PackageLockData lockData;
            std::string errorMessage;
            if (loadPackageLockData(packageRoot, &lockData, &errorMessage) &&
                !lockData.registryPath.empty()) {
                return canonicalizePath(lockData.registryPath);
            }
        }

        return canonicalizePath(defaultPackageRegistryPath(packageRoot));
    }

    return canonicalizePath(defaultPackageRegistryPath(basePath));
}

static int initializeRegistry(const std::filesystem::path& rawTarget,
                              const std::filesystem::path& executablePath) {
    (void)executablePath;
    std::filesystem::path registryRoot = isAbsolutePath(rawTarget)
                                             ? rawTarget
                                             : canonicalizePath(std::filesystem::current_path() / rawTarget);
    std::string errorMessage;
    if (!initializePackageRegistry(registryRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 73;
    }

    std::cout << "Initialized Uranium registry at " << displayPath(registryRoot) << std::endl;
    return 0;
}

static int publishPackage(const std::filesystem::path& rawTarget,
                          const std::filesystem::path& executablePath,
                          const std::filesystem::path& rawRegistry) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::filesystem::path registryRoot =
        resolveRegistryRoot(rawRegistry, packageRoot, executablePath);
    std::filesystem::path publishedRoot;
    if (!publishPackageToRegistry(packageRoot, registryRoot, &publishedRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Published " << displayPath(packageRoot)
              << " -> " << displayPath(publishedRoot) << std::endl;
    return 0;
}

static int lockPackage(const std::filesystem::path& rawTarget,
                       const std::filesystem::path& executablePath,
                       const std::filesystem::path& rawRegistry) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::filesystem::path registryRoot =
        resolveRegistryRoot(rawRegistry, packageRoot, executablePath);
    PackageLockData lockData;
    if (!generatePackageLockData(packageRoot, registryRoot, &lockData, &errorMessage) ||
        !writePackageLockData(packageRoot, lockData, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Wrote lockfile -> " << displayPath(packageLockFilePath(packageRoot))
              << std::endl;
    return 0;
}

static int installPackage(const std::filesystem::path& rawTarget,
                          const std::filesystem::path& executablePath,
                          const std::filesystem::path& rawRegistry) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::filesystem::path registryRoot =
        resolveRegistryRoot(rawRegistry, packageRoot, executablePath);
    PackageLockData lockData;
    if (!installPackageDependencies(packageRoot, registryRoot, &lockData, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Installed dependencies for " << displayPath(packageRoot) << std::endl;
    return 0;
}

static int updatePackage(const std::filesystem::path& rawTarget,
                         const std::filesystem::path& executablePath,
                         const std::filesystem::path& rawRegistry) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::filesystem::path registryRoot =
        resolveRegistryRoot(rawRegistry, packageRoot, executablePath);
    PackageLockData lockData;
    if (!updatePackageDependencies(packageRoot, registryRoot, &lockData, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Updated dependencies for " << displayPath(packageRoot) << std::endl;
    return 0;
}

static int removePackageDependencyCommand(const std::filesystem::path& rawTarget,
                                          const std::string& dependencyName,
                                          const std::filesystem::path& executablePath,
                                          const std::filesystem::path& rawRegistry) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::filesystem::path registryRoot =
        resolveRegistryRoot(rawRegistry, packageRoot, executablePath);
    PackageLockData lockData;
    if (!removePackageDependency(packageRoot, registryRoot, dependencyName,
                                 &lockData, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Removed dependency '" << dependencyName
              << "' from " << displayPath(packageRoot) << std::endl;
    return 0;
}

static int packPackage(const std::filesystem::path& rawTarget,
                       const std::filesystem::path& executablePath,
                       const std::filesystem::path& rawOutput) {
    std::filesystem::path packageRoot;
    std::string errorMessage;
    if (!resolvePackageRoot(rawTarget, executablePath, &packageRoot, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    PackageManifestData manifest;
    if (!loadPackageManifestData(packageRoot, &manifest, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::filesystem::path entryPath = std::filesystem::path(manifest.entry);
    if (!isAbsolutePath(entryPath)) {
        entryPath = packageRoot / entryPath;
    }
    entryPath = canonicalizePath(entryPath);

    if (!fileExists(entryPath)) {
        std::cerr << "Package entry does not exist: " << displayPath(entryPath) << std::endl;
        return 66;
    }

    std::string sourceText;
    if (!loadProgramWithImports(entryPath, packageRoot, &sourceText, &errorMessage)) {
        std::cerr << "Failed to load program source: " << errorMessage << std::endl;
        return 74;
    }

    FunctionPtr function = nullptr;
    if (!compile(sourceText.c_str(), &function, entryPath.string())) {
        return 65;
    }

    std::filesystem::path compiledUrcPath = compiledPathForSource(entryPath, packageRoot);
    if (!writeUrcFile(function, compiledUrcPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::filesystem::path archivePath;
    if (rawOutput.empty()) {
        archivePath = archivePathForPackage(packageRoot, manifest.name);
    } else {
        archivePath = isAbsolutePath(rawOutput)
            ? rawOutput
            : canonicalizePath(std::filesystem::current_path() / rawOutput);
        if (directoryExists(archivePath)) {
            archivePath /= std::filesystem::path(manifest.name).replace_extension(".ura");
        }
        if (archivePath.extension().empty()) {
            archivePath.replace_extension(".ura");
        }
    }

    if (!writeUraFile(function, archivePath, manifest.rawText, manifest.entry, sourceText, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Packed " << manifest.name << " -> " << displayPath(archivePath) << std::endl;
    return 0;
}

static bool shouldSkipTestDirectory(const std::filesystem::path& path) {
    std::string name = lowerCase(path.filename().string());
    return name == "compiled" || name == "build" || startsWith(name, "build");
}

static bool isTestSourceFile(const std::filesystem::path& path) {
    if (lowerCase(path.extension().string()) != ".ur") {
        return false;
    }

    std::string fileName = lowerCase(path.filename().string());
    return endsWith(fileName, "_test.ur") ||
           endsWith(fileName, ".test.ur") ||
           startsWith(fileName, "test_");
}

static std::vector<std::filesystem::path> discoverTestFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;

    if (fileExists(root)) {
        files.push_back(canonicalizePath(root));
        return files;
    }

    std::error_code errorCode;
    std::filesystem::recursive_directory_iterator iterator(root, errorCode);
    std::filesystem::recursive_directory_iterator end;
    while (!errorCode && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_directory(errorCode)) {
            if (shouldSkipTestDirectory(entry.path())) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(errorCode);
            continue;
        }

        if (!errorCode && entry.is_regular_file(errorCode) && isTestSourceFile(entry.path())) {
            files.push_back(canonicalizePath(entry.path()));
        }

        iterator.increment(errorCode);
    }

    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return left.generic_string() < right.generic_string();
              });
    return files;
}

static int runTests(const std::filesystem::path& rawTarget,
                    const std::filesystem::path& executablePath) {
    std::filesystem::path target;
    if (rawTarget.empty()) {
        std::filesystem::path defaultTests = std::filesystem::current_path() / "tests";
        target = directoryExists(defaultTests) ? defaultTests : std::filesystem::current_path();
    } else {
        target = resolveExistingPath(rawTarget, executablePath);
    }

    if (!pathExists(target)) {
        std::cerr << "Test target does not exist: " << displayPath(target) << std::endl;
        return 66;
    }

    std::vector<std::filesystem::path> testFiles = discoverTestFiles(target);
    if (testFiles.empty()) {
        std::cerr << "No Uranium tests found under " << displayPath(target) << std::endl;
        return 66;
    }

    std::filesystem::path displayRoot = directoryExists(target) ? canonicalizePath(target)
                                                                : canonicalizePath(target.parent_path());
    int passed = 0;
    int failed = 0;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (std::size_t index = 0; index < testFiles.size(); ++index) {
        const std::filesystem::path& testFile = testFiles[index];
        std::cout << "\033[1;34m[TEST]\033[0m " << relativeDisplayPath(testFile, displayRoot) << std::endl;

        VM vm;
        int status = runFile(vm, testFile, executablePath, {});
        if (status == 0) {
            std::cout << "\033[1;32mPASS\033[0m" << std::endl;
            passed++;
        } else {
            std::cout << "\033[1;31mFAIL (exit code: " << status << ")\033[0m" << std::endl;
            failed++;
        }

        if (index + 1 < testFiles.size()) {
            std::cout << std::endl;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = endTime - startTime;

    std::cout << std::endl
              << "Summary: " 
              << "\033[1;32m" << passed << " passed\033[0m, " 
              << "\033[1;31m" << failed << " failed\033[0m "
              << "in " << std::fixed << std::setprecision(1) << elapsed.count() << "ms" << std::endl;
    return failed == 0 ? 0 : 1;
}

static bool writeTextFile(const std::filesystem::path& path,
                          const std::string& content,
                          std::string* errorMessage) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not create directory '" +
                            displayPath(path.parent_path()) + "'.";
        }
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not open '" + displayPath(path) + "' for writing.";
        }
        return false;
    }

    file << content;
    if (!file.good()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not write '" + displayPath(path) + "'.";
        }
        return false;
    }

    return true;
}

static int initPackage(const std::filesystem::path& rawTarget) {
    std::filesystem::path packagePath = isAbsolutePath(rawTarget)
                                            ? rawTarget
                                            : std::filesystem::current_path() / rawTarget;
    packagePath = canonicalizePath(packagePath);

    if (fileExists(packagePath)) {
        std::cerr << "Cannot initialize a package over an existing file: "
                  << displayPath(packagePath) << std::endl;
        return 73;
    }

    if (directoryExists(packagePath) && !isDirectoryEmpty(packagePath)) {
        std::cerr << "Package directory is not empty: " << displayPath(packagePath) << std::endl;
        return 73;
    }

    std::string packageName = packagePath.filename().string();
    if (packageName.empty()) {
        packageName = "uranium-app";
    }

    std::error_code errorCode;
    std::filesystem::create_directories(packagePath / "src", errorCode);
    if (errorCode) {
        std::cerr << "Could not create package directories under "
                  << displayPath(packagePath) << std::endl;
        return 73;
    }

    std::filesystem::create_directories(packagePath / "tests", errorCode);
    if (errorCode) {
        std::cerr << "Could not create package test directory under "
                  << displayPath(packagePath) << std::endl;
        return 73;
    }

    std::string manifest =
        "{\n"
        "  \"name\": \"" + packageName + "\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"entry\": \"src/main.ur\",\n"
        "  \"tests\": \"tests\",\n"
        "  \"dependencies\": {}\n"
        "}\n";

    std::string readme =
        "# " + packageName + "\n\n"
        "Created with `uranium --init-package`.\n\n"
        "## Commands\n\n"
        "- `uranium src/main.ur`\n"
        "- `uranium --test`\n"
        "- `uranium --make`\n"
        "- `uranium --lock .`\n"
        "- `uranium --install .`\n"
        "- `uranium --update .`\n"
        "- `uranium --remove . <dependency>`\n"
        "- `uranium --pack .`\n";

    std::string umakeText =
        "ENTRY = src/main.ur\n"
        "TESTS = tests\n\n"
        "default: test\n\n"
        "run:\n"
        "    ${uranium} ${ENTRY}\n\n"
        "test:\n"
        "    ${uranium} --test ${TESTS}\n\n"
        "fmt:\n"
        "    ${uranium} --fmt src\n"
        "    ${uranium} --fmt ${TESTS}\n\n"
        "lint:\n"
        "    ${uranium} --lint src\n"
        "    ${uranium} --lint ${TESTS}\n\n"
        "pack: test\n"
        "    ${uranium} --pack .\n";

    std::string mainSource =
        "import std as std\n\n"
        "class main() {\n"
        "print(\"hello from " + packageName + "\")\n"
        "print(\"uranium => \" + URANIUM_VERSION)\n"
        "print(\"cwd => \" + std.envCwd())\n"
        "}\n";

    std::string smokeTest =
        "import assert as assert\n"
        "import std as std\n\n"
        "class main() {\n"
        "assert.equal(std.square(3), 9, \"square() should work\")\n"
        "assert.ok(std.threadCores() >= 1, \"thread core count should be positive\")\n"
        "assert.equal(std.pathJoin(\"a\", \"b\"), \"a/b\", \"path join should normalize\")\n"
        "print(\"smoke-ok\")\n"
        "}\n";

    std::string gitignore = "compiled/\n";
    std::string writeError;
    if (!writeTextFile(packagePath / "uranium.pkg", manifest, &writeError) ||
        !writeTextFile(packagePath / "README.md", readme, &writeError) ||
        !writeTextFile(packagePath / "UMake", umakeText, &writeError) ||
        !writeTextFile(packagePath / ".gitignore", gitignore, &writeError) ||
        !writeTextFile(packagePath / "src" / "main.ur", mainSource, &writeError) ||
        !writeTextFile(packagePath / "tests" / "smoke_test.ur", smokeTest, &writeError)) {
        std::cerr << writeError << std::endl;
        return 73;
    }

    std::cout << "Initialized Uranium package at " << displayPath(packagePath) << std::endl;
    return 0;
}

std::size_t g_maxHeapBytes = 0;
std::size_t g_baseYoungBytes = 64 * 1024;
std::size_t g_baseFullBytes = 512 * 1024;
int g_maxFrames = 100000;
bool g_vmDebugMode = false;
std::string g_entryPointName = "main";
int g_umakeJobs = 1;

int g_optimizerLevel = 0;

std::wstring g_compileIconPath = L"";
std::wstring g_compileCompanyName = L"";
std::wstring g_compileFileDescription = L"";
std::wstring g_compileFileVersion = L"";
std::wstring g_compileProductName = L"";
std::wstring g_compileProductVersion = L"";



static std::wstring toWString(const std::string& str) {
    if (str.empty()) return L"";
#ifdef _WIN32
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], sizeNeeded);
    return wstrTo;
#else
    return std::wstring(str.begin(), str.end());
#endif
}

static bool parseSize(const std::string& str, std::size_t* result) {
    if (str.empty()) return false;
    
    std::size_t length = str.length();
    char suffix = str.back();
    std::string numberPart = str;
    std::size_t multiplier = 1;
    
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        numberPart = str.substr(0, length - 1);
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        numberPart = str.substr(0, length - 1);
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024 * 1024 * 1024;
        numberPart = str.substr(0, length - 1);
    }
    
    for (char c : numberPart) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    
    try {
        *result = std::stoull(numberPart) * multiplier;
        return true;
    } catch (...) {
        return false;
    }
}

static bool parseLimit(const std::string& str, int* result) {
    for (char c : str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        *result = std::stoi(str);
        return true;
    } catch (...) {
        return false;
    }
}

static int compileToUrc(const std::filesystem::path& rawInputPath,
                         const std::filesystem::path& rawOutputPath,
                         const std::filesystem::path& executablePath) {
    std::filesystem::path inputPath = resolveInputPath(rawInputPath, executablePath);
    FunctionPtr function = nullptr;
    std::string errorMessage;
    std::filesystem::path workspaceRoot = findWorkspaceRoot(inputPath, executablePath);

    int compileStatus = compileSourceProgram(inputPath, workspaceRoot, &function, &errorMessage);
    if (compileStatus != 0) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        }
        return compileStatus;
    }

    std::filesystem::path outputPath = rawOutputPath;
    if (outputPath.empty()) {
        outputPath = inputPath;
        outputPath.replace_extension(".urc");
    }

    if (!writeUrcFile(function, outputPath, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 74;
    }

    std::cout << "Compiled " << displayPath(inputPath)
              << " -> " << displayPath(outputPath) << std::endl;
    return 0;
}

static void printUsage() {
    std::cout
        << "Usage:\n"
        << "  uranium [path] [args...]\n"
        << "  uranium <path> --compile [output.exe]\n"
        << "  uranium --compile <path> [output.exe]\n"
        << "  uranium --compile-urc <path> [-out <output.urc>]\n"
        << "  uranium --version\n"
        << "  uranium --help\n"
        << "  uranium --test [path]\n"
        << "  uranium --fmt <path>\n"
        << "  uranium --fmt-check <path>\n"
        << "  uranium --lint <path>\n"
        << "  uranium --debug <path>\n"
        << "  uranium --debug-run <path> [args...]\n"
        << "  uranium --lsp\n"
        << "  uranium --make [target]\n"
        << "  uranium --make-file <path> [target]\n"
        << "  uranium --make-list [path]\n"
        << "  uranium --registry-init <directory>\n"
        << "  uranium --publish <package-dir-or-manifest> [registry-dir]\n"
        << "  uranium --lock <package-dir-or-manifest> [registry-dir]\n"
        << "  uranium --install <package-dir-or-manifest> [registry-dir]\n"
        << "  uranium --update <package-dir-or-manifest> [registry-dir]\n"
        << "  uranium --remove <package-dir-or-manifest> <dependency-name> [registry-dir]\n"
        << "  uranium --pack <package-dir-or-manifest> [output.ura]\n"
        << "  uranium --init-package <directory>\n"
        << "  uranium --install-file <path.ura> [registry-dir]\n"
        << "  uranium --load-library <path.ura/.urc/.ur>\n"
        << "  uranium --unload-library <name>\n"
        << "\nOptimization Options:\n"
        << "  --O0                     Disable optimization (default)\n"
        << "  --O1                     Basic optimizations (constant fold, dead store)\n"
        << "  --O2                     Medium optimizations (O1 + jump threading, BOC elimination)\n"
        << "  --O3                     Aggressive optimizations (O2 + strength reduction, NOP compaction)\n"
        << "\nVM Options:\n"
        << "  --vm-debug-mode          Enable execution instruction debug tracing\n"
        << "  -j <N>                   Run N jobs in parallel for Umake\n"
        << "  -e, --entry <function>   Set the auto-main entry point (default: main)\n"
        << "  --gc <size>              Set young/full collection threshold (e.g. 64K, 1M)\n"
        << "  --vm <size>              Set maximum heap memory limit (e.g. 10M, 1G)\n"
        << "  --re <limit>             Set maximum recursion call frame limit\n"
        << "\nCompile Customization Options:\n"
        << "  --icon <path.ico>        Custom icon for compiled executables\n"
        << "  --company <text>         Custom CompanyName metadata\n"
        << "  --desc <text>            Custom FileDescription metadata\n"
        << "  --product <text>         Custom ProductName metadata\n"
        << "  --version-info <text>    Custom File/Product Version metadata\n";
}

int main(int argc, const char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hErr, &dwMode)) {
            SetConsoleMode(hErr, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
    std::filesystem::path executablePath = canonicalizePath(argv[0]);

    // Parse and filter options
    std::vector<const char*> filteredArgs;
    filteredArgs.push_back(argv[0]);

    bool vmDebugMode = false;
    bool targetWasm = false;
    std::string gcSizeStr = "";
    std::string vmSizeStr = "";
    std::string reLimitStr = "";
    bool compileUrc = false;
    std::string outPathStr = "";

    std::string compileIcon = "";
    std::string compileCompany = "";
    std::string compileDesc = "";
    std::string compileProduct = "";
    std::string compileVersion = "";

    for (int index = 1; index < argc; ++index) {
        std::string arg = argv[index];
        if (arg == "--vm-debug-mode") {
            vmDebugMode = true;
        } else if (arg == "--target=wasm") {
            targetWasm = true;
        } else if (arg == "--compile-urc") {
            compileUrc = true;
        } else if (arg == "--O0") {
            g_optimizerLevel = 0;
        } else if (arg == "--O1") {
            g_optimizerLevel = 1;
        } else if (arg == "--O2") {
            g_optimizerLevel = 2;
        } else if (arg == "--O3") {
            g_optimizerLevel = 3;
        } else if ((arg == "-e" || arg == "--entry") && index + 1 < argc) {
            g_entryPointName = argv[++index];
        } else if (arg == "-j" && index + 1 < argc) {
            try {
                g_umakeJobs = std::stoi(argv[++index]);
                if (g_umakeJobs < 1) g_umakeJobs = 1;
            } catch (...) {
                std::cerr << "Invalid -j value: " << argv[index] << std::endl;
                return 64;
            }
        } else if (arg == "--gc" && index + 1 < argc) {
            gcSizeStr = argv[++index];
        } else if (arg == "--vm" && index + 1 < argc) {
            vmSizeStr = argv[++index];
        } else if (arg == "--re" && index + 1 < argc) {
            reLimitStr = argv[++index];
        } else if (arg == "-out" && index + 1 < argc) {
            outPathStr = argv[++index];
        } else if (arg == "--icon" && index + 1 < argc) {
            compileIcon = argv[++index];
        } else if (arg == "--company" && index + 1 < argc) {
            compileCompany = argv[++index];
        } else if (arg == "--desc" && index + 1 < argc) {
            compileDesc = argv[++index];
        } else if (arg == "--product" && index + 1 < argc) {
            compileProduct = argv[++index];
        } else if (arg == "--version-info" && index + 1 < argc) {
            compileVersion = argv[++index];
        } else {
            filteredArgs.push_back(argv[index]);
        }
    }

    g_vmDebugMode = vmDebugMode;

    if (!gcSizeStr.empty()) {
        std::size_t size = 0;
        if (parseSize(gcSizeStr, &size)) {
            g_baseYoungBytes = size;
            g_baseFullBytes = size * 8;
        } else {
            std::cerr << "Invalid --gc size: " << gcSizeStr << std::endl;
            return 64;
        }
    }

    if (!vmSizeStr.empty()) {
        std::size_t size = 0;
        if (parseSize(vmSizeStr, &size)) {
            g_maxHeapBytes = size;
        } else {
            std::cerr << "Invalid --vm size: " << vmSizeStr << std::endl;
            return 64;
        }
    }

    if (!reLimitStr.empty()) {
        int limit = 0;
        if (parseLimit(reLimitStr, &limit)) {
            g_maxFrames = limit;
        } else {
            std::cerr << "Invalid --re limit: " << reLimitStr << std::endl;
            return 64;
        }
    }

    g_compileIconPath = toWString(compileIcon);
    g_compileCompanyName = toWString(compileCompany);
    g_compileFileDescription = toWString(compileDesc);
    g_compileFileVersion = toWString(compileVersion);
    g_compileProductName = toWString(compileProduct);
    g_compileProductVersion = toWString(compileVersion);

    int newArgc = static_cast<int>(filteredArgs.size());
    const char** newArgv = filteredArgs.data();
    argc = newArgc;
    argv = newArgv;

    // Re-configure runtime process context with updated args/argc
    configureRuntimeProcessContext(executablePath.generic_string(), "", {});

    bool handledEmbeddedProgram = false;
    int embeddedProgramStatus =
        maybeRunEmbeddedProgram(executablePath, newArgc, newArgv, &handledEmbeddedProgram);
    if (handledEmbeddedProgram) {
        return embeddedProgramStatus;
    }

    if (newArgc == 1) {
        VM vm;
        repl(vm);
        return 0;
    }

    if (targetWasm) {
        if (newArgc < 2) {
            std::cerr << "Usage: uranium --target=wasm <path>\n";
            return 64;
        }
        std::filesystem::path inputPath = newArgv[1];
        std::filesystem::path workspaceRoot = findWorkspaceRoot(inputPath, executablePath);
        FunctionPtr function = nullptr;
        std::string errorMessage;
        if (compileSourceProgram(inputPath, workspaceRoot, &function, &errorMessage) != 0) {
            std::cerr << errorMessage << std::endl;
            return 65;
        }
        FastPathPlan plan;
        if (!buildFastPathPlan(function, &plan, &errorMessage)) {
            std::cerr << "Optimizer Error: " << errorMessage << std::endl;
            return 65;
        }

        NativeJitArtifact artifact;
        if (!compileNativeJit(function, plan, &artifact, &errorMessage, true)) {
            std::cerr << errorMessage << std::endl;
            return 65;
        }
        return 0;
    }

    if (compileUrc) {
        if (newArgc < 2) {
            std::cerr << "Usage: uranium --compile-urc <path> [-out <output.urc>]\n";
            return 64;
        }
        std::filesystem::path inputPath = newArgv[1];
        std::filesystem::path outputPath = outPathStr;
        return compileToUrc(inputPath, outputPath, executablePath);
    }

    int compileIndex = -1;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--compile") {
            compileIndex = index;
            break;
        }
    }

    if (compileIndex != -1) {
        std::filesystem::path inputPath;
        std::filesystem::path outputPath;
        if (compileIndex == 1) {
            if (argc < 3 || argc > 4) {
                std::cerr << "Usage: uranium --compile <path> [output.exe]\n";
                return 64;
            }
            inputPath = argv[2];
            if (argc == 4) {
                outputPath = argv[3];
            }
        } else {
            if (compileIndex != 2 || argc > 4) {
                std::cerr << "Usage: uranium <path> --compile [output.exe]\n";
                return 64;
            }
            inputPath = argv[1];
            if (argc == 4) {
                outputPath = argv[3];
            }
        }

        return compileRunnableToBinary(inputPath, outputPath, executablePath);
    }

    std::string argument = argv[1];
    if (argument == "--version") {
        std::cout << "Copyright (C) 2026 Uranium Programming Language\n";
        std::cout << "This project author is : omerdev\n";
        std::cout << "License : GPLv3\n";
        return 0;
    }

    if (argument == "--help") {
        printUsage();
        return 0;
    }

    if (argument == "--test") {
        std::filesystem::path target;
        if (argc >= 3) {
            target = argv[2];
        }
        return runTests(target, executablePath);
    }

    if (argument == "--fmt") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --fmt <path>\n";
            return 64;
        }
        return formatPath(argv[2], false, 4);
    }

    if (argument == "--fmt-check") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --fmt-check <path>\n";
            return 64;
        }
        return formatPath(argv[2], true, 4);
    }

    if (argument == "--lint") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --lint <path>\n";
            return 64;
        }
        return lintPath(argv[2], executablePath);
    }

    if (argument == "--debug") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --debug <path>\n";
            return 64;
        }
        return debugPath(argv[2], executablePath);
    }

    if (argument == "--debug-run") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --debug-run <path> [args...]\n";
            return 64;
        }

        std::vector<std::string> debugArgs;
        for (int index = 3; index < argc; ++index) {
            debugArgs.push_back(argv[index]);
        }
        return debugRunPath(argv[2], executablePath, debugArgs);
    }

    if (argument == "--lsp") {
        return runLspServer(executablePath);
    }

    if (argument == "--make" || argument == "--umake") {
        std::string targetName;
        if (argc >= 3) {
            targetName = argv[2];
        }
        return runUMakeTarget({}, targetName, executablePath);
    }

    if (argument == "--make-file") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --make-file <path> [target]\n";
            return 64;
        }

        std::string targetName;
        if (argc >= 4) {
            targetName = argv[3];
        }
        return runUMakeTarget(argv[2], targetName, executablePath);
    }

    if (argument == "--make-list") {
        std::filesystem::path target;
        if (argc >= 3) {
            target = argv[2];
        }
        return listUMakeTargets(target, executablePath);
    }

    if (argument == "--registry-init") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --registry-init <directory>\n";
            return 64;
        }
        return initializeRegistry(argv[2], executablePath);
    }

    if (argument == "--publish") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --publish <package-dir-or-manifest> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path registryPath;
        if (argc >= 4) {
            registryPath = argv[3];
        }
        return publishPackage(argv[2], executablePath, registryPath);
    }

    if (argument == "--lock") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --lock <package-dir-or-manifest> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path registryPath;
        if (argc >= 4) {
            registryPath = argv[3];
        }
        return lockPackage(argv[2], executablePath, registryPath);
    }

    if (argument == "--install") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --install <package-dir-or-manifest> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path registryPath;
        if (argc >= 4) {
            registryPath = argv[3];
        }
        return installPackage(argv[2], executablePath, registryPath);
    }

    if (argument == "--update") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --update <package-dir-or-manifest> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path registryPath;
        if (argc >= 4) {
            registryPath = argv[3];
        }
        return updatePackage(argv[2], executablePath, registryPath);
    }

    if (argument == "--remove") {
        if (argc < 4) {
            std::cerr << "Usage: uranium --remove <package-dir-or-manifest> <dependency-name> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path registryPath;
        if (argc >= 5) {
            registryPath = argv[4];
        }
        return removePackageDependencyCommand(argv[2], argv[3], executablePath, registryPath);
    }

    if (argument == "--pack") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --pack <package-dir-or-manifest> [output.ura]\n";
            return 64;
        }

        std::filesystem::path outputPath;
        if (argc >= 4) {
            outputPath = argv[3];
        }
        return packPackage(argv[2], executablePath, outputPath);
    }

    if (argument == "--install-file") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --install-file <path.ura> [registry-dir]\n";
            return 64;
        }

        std::filesystem::path uraPath = resolveInputPath(argv[2], executablePath);
        if (!fileExists(uraPath)) {
            std::cerr << "File not found: " << displayPath(uraPath) << std::endl;
            return 66;
        }

        FunctionPtr function = nullptr;
        std::string manifestText;
        std::string entryPath;
        std::string sourceText;
        std::string errorMessage;
        if (!readUraFile(uraPath, &function, &manifestText, &entryPath, &sourceText, &errorMessage)) {
            std::cerr << "Failed to read .ura package: " << errorMessage << std::endl;
            return 74;
        }

        // Parse manifest to get package name and version
        std::string name = "unknown";
        std::string version = "0.1.0";
        // Simple JSON-like parsing for name and version in manifestText
        std::size_t namePos = manifestText.find("\"name\"");
        if (namePos != std::string::npos) {
            std::size_t colonPos = manifestText.find(":", namePos);
            if (colonPos != std::string::npos) {
                std::size_t startQuote = manifestText.find("\"", colonPos);
                if (startQuote != std::string::npos) {
                    std::size_t endQuote = manifestText.find("\"", startQuote + 1);
                    if (endQuote != std::string::npos) {
                        name = manifestText.substr(startQuote + 1, endQuote - startQuote - 1);
                    }
                }
            }
        }
        std::size_t verPos = manifestText.find("\"version\"");
        if (verPos != std::string::npos) {
            std::size_t colonPos = manifestText.find(":", verPos);
            if (colonPos != std::string::npos) {
                std::size_t startQuote = manifestText.find("\"", colonPos);
                if (startQuote != std::string::npos) {
                    std::size_t endQuote = manifestText.find("\"", startQuote + 1);
                    if (endQuote != std::string::npos) {
                        version = manifestText.substr(startQuote + 1, endQuote - startQuote - 1);
                    }
                }
            }
        }

        std::filesystem::path registryPath;
        if (argc >= 4) {
            registryPath = argv[3];
        } else {
            registryPath = defaultPackageRegistryPath(std::filesystem::current_path());
        }

        std::error_code ec;
        std::filesystem::path destDir = registryPath / "packages" / name / version;
        std::filesystem::create_directories(destDir, ec);
        if (ec) {
            std::cerr << "Could not create registry directory: " << ec.message() << std::endl;
            return 74;
        }

        // Write the package files or copy the .ura file
        std::filesystem::path destFile = destDir / (name + ".ura");
        std::filesystem::copy_file(uraPath, destFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Failed to copy package file: " << ec.message() << std::endl;
            return 74;
        }

        // Write the source file so the loader can read it
        if (!sourceText.empty()) {
            std::filesystem::path sourceDest = destDir / entryPath;
            std::filesystem::create_directories(sourceDest.parent_path(), ec);
            std::ofstream srcFile(sourceDest);
            srcFile << sourceText;
            srcFile.close();
        }

        // Also write a basic uranium.pkg so loader can read it
        std::ofstream pkgFile(destDir / "uranium.pkg");
        pkgFile << manifestText;
        pkgFile.close();

        std::cout << "Successfully installed package " << name << "@" << version << " to " << displayPath(destDir) << std::endl;
        return 0;
    }

    if (argument == "--load-library") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --load-library <path.ura/.urc/.ur>\n";
            return 64;
        }

        std::filesystem::path libPath = resolveInputPath(argv[2], executablePath);
        if (!fileExists(libPath)) {
            std::cerr << "Library file not found: " << displayPath(libPath) << std::endl;
            return 66;
        }

        std::filesystem::path tpDir = executablePath.parent_path() / "urlib" / "third_party";
        std::error_code ec;
        std::filesystem::create_directories(tpDir, ec);

        std::string targetName = libPath.stem().string();
        std::string ext = lowerCase(libPath.extension().string());

        if (ext == ".ura") {
            FunctionPtr function = nullptr;
            std::string manifestText;
            std::string entryPath;
            std::string sourceText;
            std::string errorMessage;
            if (readUraFile(libPath, &function, &manifestText, &entryPath, &sourceText, &errorMessage)) {
                // Parse manifest to get package name
                std::size_t namePos = manifestText.find("\"name\"");
                if (namePos != std::string::npos) {
                    std::size_t colonPos = manifestText.find(":", namePos);
                    if (colonPos != std::string::npos) {
                        std::size_t startQuote = manifestText.find("\"", colonPos);
                        if (startQuote != std::string::npos) {
                            std::size_t endQuote = manifestText.find("\"", startQuote + 1);
                            if (endQuote != std::string::npos) {
                                targetName = manifestText.substr(startQuote + 1, endQuote - startQuote - 1);
                            }
                        }
                    }
                }
                // If it has source code embedded, extract it as [targetName].ur in third_party
                if (!sourceText.empty()) {
                    std::ofstream srcFile(tpDir / (targetName + ".ur"));
                    srcFile << sourceText;
                    srcFile.close();
                }
            }
        }

        std::filesystem::path destFile = tpDir / (targetName + ext);
        std::filesystem::copy_file(libPath, destFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Failed to load library: " << ec.message() << std::endl;
            return 74;
        }

        std::cout << "Successfully loaded library '" << targetName << "' to global third_party." << std::endl;
        return 0;
    }

    if (argument == "--unload-library") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --unload-library <name>\n";
            return 64;
        }

        std::string libName = argv[2];
        std::filesystem::path tpDir = executablePath.parent_path() / "urlib" / "third_party";
        std::error_code ec;

        bool removed = false;
        std::vector<std::string> extensions = {".ur", ".ura", ".urc"};
        for (const auto& ext : extensions) {
            std::filesystem::path file = tpDir / (libName + ext);
            if (fileExists(file)) {
                std::filesystem::remove(file, ec);
                removed = true;
            }
        }

        if (removed) {
            std::cout << "Successfully unloaded library '" << libName << "' from global third_party." << std::endl;
            return 0;
        } else {
            std::cerr << "Library '" << libName << "' not found in global third_party." << std::endl;
            return 66;
        }
    }

    if (argument == "--init-package") {
        if (argc < 3) {
            std::cerr << "Usage: uranium --init-package <directory>\n";
            return 64;
        }
        return initPackage(argv[2]);
    }

    std::vector<std::string> scriptArgs;
    for (int index = 2; index < argc; ++index) {
        scriptArgs.push_back(argv[index]);
    }

    VM vm;
    return runFile(vm, argv[1], executablePath, scriptArgs);
}
