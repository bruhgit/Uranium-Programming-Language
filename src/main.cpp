#include "common.h"
#include "compiler.h"
#include "package_manager.h"
#include "source_loader.h"
#include "tooling.h"
#include "umake.h"
#include "system_native.h"
#include "urc.h"
#include "vm.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    if (startDirectory.empty() || relativePath.empty() || relativePath.is_absolute()) {
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
    if (startDirectory.empty() || relativePath.empty() || relativePath.is_absolute()) {
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
    if (rawPath.is_absolute()) {
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
    if (rawPath.is_absolute()) {
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

    if (!compile(source.c_str(), function)) {
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
    std::filesystem::path registryRoot = rawTarget.is_absolute()
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
    if (!entryPath.is_absolute()) {
        entryPath = packageRoot / entryPath;
    }
    entryPath = canonicalizePath(entryPath);

    if (!fileExists(entryPath)) {
        std::cerr << "Package entry does not exist: " << displayPath(entryPath) << std::endl;
        return 66;
    }

    FunctionPtr function = nullptr;
    int compileStatus = compileSourceProgram(entryPath, packageRoot, &function, &errorMessage);
    if (compileStatus != 0) {
        if (!errorMessage.empty()) {
            std::cerr << errorMessage << std::endl;
        }
        return compileStatus;
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
        archivePath = rawOutput.is_absolute()
                          ? rawOutput
                          : canonicalizePath(std::filesystem::current_path() / rawOutput);
        if (directoryExists(archivePath)) {
            archivePath /= std::filesystem::path(manifest.name).replace_extension(".ura");
        }
        if (archivePath.extension().empty()) {
            archivePath.replace_extension(".ura");
        }
    }

    if (!writeUraFile(function, archivePath, manifest.rawText, manifest.entry, &errorMessage)) {
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

    for (std::size_t index = 0; index < testFiles.size(); ++index) {
        const std::filesystem::path& testFile = testFiles[index];
        std::cout << "[TEST] " << relativeDisplayPath(testFile, displayRoot) << std::endl;

        VM vm;
        int status = runFile(vm, testFile, executablePath, {});
        if (status == 0) {
            std::cout << "PASS" << std::endl;
            passed++;
        } else {
            std::cout << "FAIL (" << status << ")" << std::endl;
            failed++;
        }

        if (index + 1 < testFiles.size()) {
            std::cout << std::endl;
        }
    }

    std::cout << std::endl
              << "Summary: " << passed << " passed, " << failed << " failed." << std::endl;
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
    std::filesystem::path packagePath = rawTarget.is_absolute()
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

static void printUsage() {
    std::cout
        << "Usage:\n"
        << "  uranium [path] [args...]\n"
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
        << "  uranium --init-package <directory>\n";
}

int main(int argc, const char* argv[]) {
    std::filesystem::path executablePath = canonicalizePath(argv[0]);
    configureRuntimeProcessContext(executablePath.generic_string(), "", {});

    if (argc == 1) {
        VM vm;
        repl(vm);
        return 0;
    }
    std::string argument = argv[1];
    if (argument == "--version") {
        std::cout << "Uranium Compiler V3 from omerdev\n";
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
