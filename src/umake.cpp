#include "umake.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include "common.h"

namespace {

bool fileExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_regular_file(path, errorCode);
}

bool directoryExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) &&
           std::filesystem::is_directory(path, errorCode);
}

static bool isAbsolutePath(const std::filesystem::path& path) {
    if (path.empty()) return false;
    if (path.is_absolute()) return true;
    std::string s = path.generic_string();
    if (s[0] == '/') return true;
    return false;
}

std::filesystem::path canonicalize(const std::filesystem::path& path) {
    std::error_code errorCode;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        return std::filesystem::absolute(path);
    }
    return result;
}

std::string displayPath(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) {
        start++;
    }

    std::size_t end = value.size();
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string leftTrim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) {
        start++;
    }
    return value.substr(start);
}

std::vector<std::string> splitWhitespace(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    for (char character : value) {
        if (character == ' ' || character == '\t' || character == '\r') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

std::filesystem::path findInDirectory(const std::filesystem::path& directory) {
    static const char* kNames[] = { "UMake", "UMakefile", "umake" };
    for (const char* name : kNames) {
        std::filesystem::path candidate = directory / name;
        if (fileExists(candidate)) {
            return canonicalize(candidate);
        }
    }
    return {};
}

std::filesystem::path searchUpwardForUMake(const std::filesystem::path& startPath) {
    if (startPath.empty()) {
        return {};
    }

    std::filesystem::path current = canonicalize(startPath);
    if (fileExists(current)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        std::filesystem::path found = findInDirectory(current);
        if (!found.empty()) {
            return found;
        }

        std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

bool readTextFile(const std::filesystem::path& path,
                  std::string* content,
                  std::string* errorMessage) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not open '" + displayPath(path) + "'.";
        }
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    *content = buffer.str();
    return true;
}

std::string stripComment(const std::string& line) {
    std::size_t comment = line.find('#');
    if (comment == std::string::npos) {
        return line;
    }
    return line.substr(0, comment);
}

bool isIdentifierLike(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    char first = value[0];
    bool firstOk =
        (first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z') ||
        first == '_';
    if (!firstOk) {
        return false;
    }

    for (char current : value) {
        bool ok =
            (current >= 'A' && current <= 'Z') ||
            (current >= 'a' && current <= 'z') ||
            (current >= '0' && current <= '9') ||
            current == '_';
        if (!ok) {
            return false;
        }
    }

    return true;
}

std::string quoteForShell(const std::filesystem::path& path) {
    std::string text = displayPath(path);
    if (text.find(' ') == std::string::npos && text.find('\t') == std::string::npos) {
        return text;
    }

    return "\"" + text + "\"";
}

std::string builtInVariable(const std::string& name,
                            const UMakeFileData& data,
                            const UMakeTargetData* target,
                            const std::filesystem::path& executablePath,
                            bool forShell) {
    if (name == "uranium") {
        return forShell ? quoteForShell(canonicalize(executablePath))
                        : displayPath(canonicalize(executablePath));
    }

    if (name == "root") {
        return forShell ? quoteForShell(data.path.parent_path())
                        : displayPath(data.path.parent_path());
    }

    if (name == "umake") {
        return forShell ? quoteForShell(data.path) : displayPath(data.path);
    }

    if (name == "target") {
        return target == nullptr ? std::string() : target->name;
    }

    if (name == "cwd") {
        std::filesystem::path cwd = canonicalize(std::filesystem::current_path());
        return forShell ? quoteForShell(cwd) : displayPath(cwd);
    }

    return "";
}

bool expandTemplate(const std::string& input,
                    const UMakeFileData& data,
                    const UMakeTargetData* target,
                    const std::filesystem::path& executablePath,
                    bool forShell,
                    int depth,
                    std::string* output,
                    std::string* errorMessage) {
    if (depth > 8) {
        if (errorMessage != nullptr) {
            *errorMessage = "UMake variable expansion exceeded the safe recursion limit.";
        }
        return false;
    }

    std::string expanded;
    expanded.reserve(input.size() + 16);

    std::size_t index = 0;
    while (index < input.size()) {
        if (input[index] != '$' || index + 1 >= input.size() || input[index + 1] != '{') {
            expanded.push_back(input[index]);
            index++;
            continue;
        }

        std::size_t end = input.find('}', index + 2);
        if (end == std::string::npos) {
            if (errorMessage != nullptr) {
                *errorMessage = "UMake variable reference is missing a closing '}'.";
            }
            return false;
        }

        std::string key = trim(input.substr(index + 2, end - (index + 2)));
        std::string value = builtInVariable(key, data, target, executablePath, forShell);
        if (value.empty()) {
            auto variable = data.variables.find(key);
            if (variable != data.variables.end()) {
                if (!expandTemplate(variable->second, data, target, executablePath,
                                    forShell, depth + 1, &value, errorMessage)) {
                    return false;
                }
            }
        }

        expanded.append(value);
        index = end + 1;
    }

    *output = expanded;
    return true;
}

bool readQuotedToken(const std::string& text,
                     std::size_t* index,
                     std::string* value) {
    if (*index >= text.size() || text[*index] != '"') {
        return false;
    }

    std::size_t start = ++(*index);
    while (*index < text.size()) {
        if (text[*index] == '"' && text[*index - 1] != '\\') {
            *value = text.substr(start, *index - start);
            (*index)++;
            return true;
        }
        (*index)++;
    }

    return false;
}

bool parseIncludePath(const std::string& text,
                      std::string* includePath,
                      std::string* errorMessage) {
    std::size_t index = 0;
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
        index++;
    }

    if (index >= text.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = "UMake include is missing a path.";
        }
        return false;
    }

    if (text[index] == '"') {
        if (!readQuotedToken(text, &index, includePath)) {
            if (errorMessage != nullptr) {
                *errorMessage = "UMake include has an unterminated quoted path.";
            }
            return false;
        }
    } else {
        *includePath = trim(text.substr(index));
    }

    if (includePath->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "UMake include is missing a path.";
        }
        return false;
    }

    return true;
}

bool resolveIncludePath(const std::filesystem::path& currentFile,
                        const std::string& rawInclude,
                        std::filesystem::path* resolvedPath,
                        std::string* errorMessage) {
    std::filesystem::path candidate = std::filesystem::path(rawInclude);
    if (!isAbsolutePath(candidate)) {
        candidate = currentFile.parent_path() / candidate;
    }

    if (directoryExists(candidate)) {
        candidate = findInDirectory(candidate);
    } else if (fileExists(candidate)) {
        candidate = canonicalize(candidate);
    }

    if (candidate.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not resolve included UMake path '" + rawInclude + "'.";
        }
        return false;
    }

    *resolvedPath = candidate;
    return true;
}

std::string chooseDefaultTarget(const UMakeFileData& data) {
    if (data.targets.find("default") != data.targets.end()) {
        return "default";
    }

    if (!data.targetOrder.empty()) {
        return data.targetOrder.front();
    }

    return "";
}

struct WorkingDirectoryGuard {
    std::filesystem::path original;
    bool active = false;

    explicit WorkingDirectoryGuard(const std::filesystem::path& next) {
        std::error_code errorCode;
        original = std::filesystem::current_path(errorCode);
        if (errorCode) {
            return;
        }

        std::filesystem::current_path(next, errorCode);
        if (!errorCode) {
            active = true;
        }
    }

    ~WorkingDirectoryGuard() {
        if (!active) {
            return;
        }

        std::error_code errorCode;
        std::filesystem::current_path(original, errorCode);
    }
};

bool loadUMakeFileRecursive(const std::filesystem::path& resolvedPath,
                            const std::filesystem::path& executablePath,
                            UMakeFileData* data,
                            std::unordered_set<std::string>* activeFiles,
                            std::unordered_set<std::string>* loadedFiles,
                            std::string* errorMessage) {
    std::string canonicalKey = canonicalize(resolvedPath).string();
    if (loadedFiles->find(canonicalKey) != loadedFiles->end()) {
        return true;
    }

    if (!activeFiles->insert(canonicalKey).second) {
        if (errorMessage != nullptr) {
            *errorMessage = "UMake include cycle detected at '" + canonicalKey + "'.";
        }
        return false;
    }

    std::string text;
    if (!readTextFile(resolvedPath, &text, errorMessage)) {
        activeFiles->erase(canonicalKey);
        return false;
    }

    if (data->path.empty()) {
        data->path = canonicalize(resolvedPath);
    }

    std::stringstream stream(text);
    std::string line;
    std::string currentTarget;
    int lineNumber = 0;

    while (std::getline(stream, line)) {
        lineNumber++;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            continue;
        }

        if (line[0] == ' ' || line[0] == '\t') {
            std::string command = leftTrim(line);
            if (trim(command).empty()) {
                continue;
            }

            if (currentTarget.empty()) {
                if (errorMessage != nullptr) {
                    *errorMessage = "UMake command before first target at line " +
                                    std::to_string(lineNumber) + " in '" +
                                    displayPath(resolvedPath) + "'.";
                }
                return false;
            }

            data->targets[currentTarget].commands.push_back(command);
            continue;
        }

        currentTarget.clear();
        std::string header = trim(stripComment(line));
        if (header.empty()) {
            continue;
        }

        if (header.rfind("include ", 0) == 0) {
            std::string rawInclude;
            if (!parseIncludePath(header.substr(8), &rawInclude, errorMessage)) {
                return false;
            }

            std::string expandedInclude;
            if (!expandTemplate(rawInclude, *data, nullptr, executablePath, false, 0,
                                &expandedInclude, errorMessage)) {
                return false;
            }

            std::filesystem::path includePath;
            if (!resolveIncludePath(resolvedPath, expandedInclude, &includePath, errorMessage)) {
                return false;
            }

            if (!loadUMakeFileRecursive(includePath, executablePath, data,
                                        activeFiles, loadedFiles, errorMessage)) {
                return false;
            }
            continue;
        }

        std::size_t equals = header.find('=');
        std::size_t colon = header.find(':');
        if (equals != std::string::npos &&
            (colon == std::string::npos || equals < colon)) {
            std::string variableName = trim(header.substr(0, equals));
            std::string variableValue = trim(header.substr(equals + 1));
            if (!isIdentifierLike(variableName)) {
                if (errorMessage != nullptr) {
                    *errorMessage = "Invalid UMake variable name '" + variableName +
                                    "' at line " + std::to_string(lineNumber) + " in '" +
                                    displayPath(resolvedPath) + "'.";
                }
                return false;
            }

            data->variables[variableName] = variableValue;
            continue;
        }

        if (colon == std::string::npos) {
            if (errorMessage != nullptr) {
                *errorMessage = "UMake target header is missing ':' at line " +
                                std::to_string(lineNumber) + " in '" +
                                displayPath(resolvedPath) + "'.";
            }
            return false;
        }

        std::string targetName = trim(header.substr(0, colon));
        if (!isIdentifierLike(targetName)) {
            if (errorMessage != nullptr) {
                *errorMessage = "Invalid UMake target name '" + targetName +
                                "' at line " + std::to_string(lineNumber) + " in '" +
                                displayPath(resolvedPath) + "'.";
            }
            return false;
        }

        if (data->targets.find(targetName) != data->targets.end()) {
            if (errorMessage != nullptr) {
                *errorMessage = "UMake target '" + targetName + "' is defined more than once.";
            }
            return false;
        }

        UMakeTargetData target;
        target.name = targetName;
        target.dependencies = splitWhitespace(trim(header.substr(colon + 1)));
        target.line = lineNumber;
        data->targets[targetName] = target;
        data->targetOrder.push_back(targetName);
        currentTarget = targetName;
    }

    activeFiles->erase(canonicalKey);
    loadedFiles->insert(canonicalKey);
    return true;
}

int executeTarget(const UMakeFileData& data,
                  const std::string& targetName,
                  const std::filesystem::path& executablePath,
                  std::unordered_set<std::string>* visiting,
                  std::unordered_set<std::string>* completed) {
    if (completed->find(targetName) != completed->end()) {
        return 0;
    }

    auto targetIt = data.targets.find(targetName);
    if (targetIt == data.targets.end()) {
        std::cerr << "Unknown UMake target '" << targetName << "'." << std::endl;
        return 66;
    }

    if (!visiting->insert(targetName).second) {
        std::cerr << "UMake dependency cycle detected at target '" << targetName << "'."
                  << std::endl;
        return 65;
    }

    const UMakeTargetData& target = targetIt->second;
    for (const std::string& dependency : target.dependencies) {
        int dependencyStatus =
            executeTarget(data, dependency, executablePath, visiting, completed);
        if (dependencyStatus != 0) {
            return dependencyStatus;
        }
    }

    for (const std::string& rawCommand : target.commands) {
        bool silent = false;
        bool ignoreFailure = false;
        std::size_t index = 0;
        while (index < rawCommand.size()) {
            if (rawCommand[index] == '@') {
                silent = true;
                index++;
                continue;
            }
            if (rawCommand[index] == '-') {
                ignoreFailure = true;
                index++;
                continue;
            }
            break;
        }

        std::string command = trim(rawCommand.substr(index));
        if (command.empty()) {
            continue;
        }

        std::string expanded;
        std::string errorMessage;
        if (!expandTemplate(command, data, &target, executablePath, true, 0,
                            &expanded, &errorMessage)) {
            std::cerr << errorMessage << std::endl;
            return 65;
        }

        if (expanded.rfind("download(", 0) == 0 && expanded.back() == ')') {
            std::string url = expanded.substr(9, expanded.size() - 10);
            expanded = "curl -LO " + url;
        }

        if (!silent) {
            std::cout << "[UMake] " << target.name << " -> " << expanded << std::endl;
        }

        int status = std::system(expanded.c_str());
        if (status != 0 && !ignoreFailure) {
            std::cerr << "UMake command failed in target '" << target.name
                      << "' with exit code " << status << "." << std::endl;
            return status == 0 ? 1 : status;
        }
    }

    visiting->erase(targetName);
    completed->insert(targetName);
    return 0;
}

}  // namespace

bool findUMakeFile(const std::filesystem::path& rawPath,
                   const std::filesystem::path& executablePath,
                   std::filesystem::path* resolvedPath,
                   std::string* errorMessage) {
    if (resolvedPath == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Internal UMake resolution error.";
        }
        return false;
    }

    if (!rawPath.empty()) {
        std::filesystem::path candidate = isAbsolutePath(rawPath)
                                              ? rawPath
                                              : std::filesystem::current_path() / rawPath;
        if (directoryExists(candidate)) {
            candidate = findInDirectory(candidate);
        } else if (fileExists(candidate)) {
            candidate = canonicalize(candidate);
        }

        if (candidate.empty()) {
            if (errorMessage != nullptr) {
                *errorMessage = "Could not find a UMake file under '" +
                                displayPath(rawPath) + "'.";
            }
            return false;
        }

        *resolvedPath = candidate;
        return true;
    }

    std::filesystem::path found = searchUpwardForUMake(std::filesystem::current_path());
    if (found.empty()) {
        found = searchUpwardForUMake(canonicalize(executablePath).parent_path());
    }

    if (found.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Could not locate UMake, UMakefile or umake in the current workspace.";
        }
        return false;
    }

    *resolvedPath = found;
    return true;
}

bool loadUMakeFile(const std::filesystem::path& rawPath,
                   const std::filesystem::path& executablePath,
                   UMakeFileData* data,
                   std::string* errorMessage) {
    if (data == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Internal UMake load error.";
        }
        return false;
    }

    std::filesystem::path resolved;
    if (!findUMakeFile(rawPath, executablePath, &resolved, errorMessage)) {
        return false;
    }

    data->path.clear();
    data->targetOrder.clear();
    data->targets.clear();
    data->variables.clear();

    std::unordered_set<std::string> activeFiles;
    std::unordered_set<std::string> loadedFiles;
    if (!loadUMakeFileRecursive(resolved, executablePath, data,
                                &activeFiles, &loadedFiles, errorMessage)) {
        return false;
    }

    if (data->targets.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "UMake file '" + displayPath(resolved) +
                            "' does not define any targets.";
        }
        return false;
    }

    return true;
}

int listUMakeTargets(const std::filesystem::path& rawPath,
                     const std::filesystem::path& executablePath) {
    UMakeFileData data;
    std::string errorMessage;
    if (!loadUMakeFile(rawPath, executablePath, &data, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::string defaultTarget = chooseDefaultTarget(data);
    std::cout << "UMake file: " << displayPath(data.path) << std::endl;
    for (const std::string& name : data.targetOrder) {
        const UMakeTargetData& target = data.targets.at(name);
        std::cout << " - " << name;
        if (name == defaultTarget) {
            std::cout << " (default)";
        }
        if (!target.dependencies.empty()) {
            std::cout << " :";
            for (const std::string& dependency : target.dependencies) {
                std::cout << " " << dependency;
            }
        }
        std::cout << std::endl;
    }

    if (!data.variables.empty()) {
        std::cout << "Variables:" << std::endl;
        std::vector<std::string> keys;
        keys.reserve(data.variables.size());
        for (const auto& entry : data.variables) {
            keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());
        for (const std::string& key : keys) {
            std::cout << " - " << key << "=" << data.variables.at(key) << std::endl;
        }
    }

    return 0;
}

int executeTargetParallel(const UMakeFileData& data,
                          const std::string& targetName,
                          const std::filesystem::path& executablePath,
                          int jobs) {
    std::unordered_set<std::string> reachable;
    std::unordered_set<std::string> visiting;
    
    std::function<int(const std::string&)> dfs = [&](const std::string& node) -> int {
        if (reachable.find(node) != reachable.end()) return 0;
        if (!visiting.insert(node).second) {
            std::cerr << "UMake dependency cycle detected at target '" << node << "'." << std::endl;
            return 65;
        }
        auto it = data.targets.find(node);
        if (it == data.targets.end()) {
            std::cerr << "Unknown UMake target '" << node << "'." << std::endl;
            return 66;
        }
        for (const std::string& dep : it->second.dependencies) {
            int status = dfs(dep);
            if (status != 0) return status;
        }
        visiting.erase(node);
        reachable.insert(node);
        return 0;
    };
    
    int dfsStatus = dfs(targetName);
    if (dfsStatus != 0) return dfsStatus;
    
    std::unordered_map<std::string, int> inDegree;
    std::unordered_map<std::string, std::vector<std::string>> dependants;
    for (const std::string& node : reachable) {
        inDegree[node] = 0;
    }
    
    for (const std::string& node : reachable) {
        const UMakeTargetData& t = data.targets.at(node);
        for (const std::string& dep : t.dependencies) {
            if (reachable.find(dep) != reachable.end()) {
                dependants[dep].push_back(node);
                inDegree[node]++;
            }
        }
    }
    
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::string> readyQueue;
    
    int runningTasks = 0;
    int completedTasks = 0;
    int totalTasks = static_cast<int>(reachable.size());
    std::atomic<bool> failed{false};
    std::atomic<int> exitCode{0};
    
    for (const auto& pair : inDegree) {
        if (pair.second == 0) {
            readyQueue.push(pair.first);
        }
    }
    
    while (true) {
        std::unique_lock<std::mutex> lock(mtx);
        
        if (completedTasks == totalTasks || failed.load()) {
            cv.wait(lock, [&]() { return runningTasks == 0; });
            break;
        }
        
        while (runningTasks < jobs && !readyQueue.empty() && !failed.load()) {
            std::string currentTarget = readyQueue.front();
            readyQueue.pop();
            runningTasks++;
            
            std::thread([currentTarget, &data, executablePath, &mtx, &cv, &readyQueue, &runningTasks, &completedTasks, &dependants, &inDegree, &failed, &exitCode]() {
                const UMakeTargetData& t = data.targets.at(currentTarget);
                bool taskFailed = false;
                int taskExitCode = 0;
                
                for (const std::string& rawCommand : t.commands) {
                    if (failed.load()) break;
                    
                    bool silent = false;
                    bool ignoreFailure = false;
                    std::size_t index = 0;
                    while (index < rawCommand.size()) {
                        if (rawCommand[index] == '@') { silent = true; index++; continue; }
                        if (rawCommand[index] == '-') { ignoreFailure = true; index++; continue; }
                        break;
                    }
                    
                    std::string command = trim(rawCommand.substr(index));
                    if (command.empty()) continue;
                    
                    std::string expanded;
                    std::string errorMessage;
                    if (!expandTemplate(command, data, &t, executablePath, true, 0, &expanded, &errorMessage)) {
                        std::lock_guard<std::mutex> errLock(mtx);
                        std::cerr << errorMessage << std::endl;
                        taskFailed = true;
                        taskExitCode = 65;
                        break;
                    }
                    
                    if (expanded.rfind("download(", 0) == 0 && expanded.back() == ')') {
                        std::string url = expanded.substr(9, expanded.size() - 10);
                        expanded = "curl -LO " + url;
                    }

                    if (!silent) {
                        std::lock_guard<std::mutex> printLock(mtx);
                        std::cout << "[UMake] " << t.name << " -> " << expanded << std::endl;
                    }
                    
                    int status = std::system(expanded.c_str());
                    if (status != 0 && !ignoreFailure) {
                        std::lock_guard<std::mutex> errLock(mtx);
                        std::cerr << "UMake command failed in target '" << t.name << "' with exit code " << status << "." << std::endl;
                        taskFailed = true;
                        taskExitCode = status == 0 ? 1 : status;
                        break;
                    }
                }
                
                std::lock_guard<std::mutex> threadLock(mtx);
                if (taskFailed) {
                    failed.store(true);
                    int currentExit = 0;
                    if (exitCode.compare_exchange_strong(currentExit, taskExitCode)) {
                        // Successfully recorded the first failure exit code
                    }
                } else {
                    completedTasks++;
                    for (const std::string& dep : dependants[currentTarget]) {
                        inDegree[dep]--;
                        if (inDegree[dep] == 0) {
                            readyQueue.push(dep);
                        }
                    }
                }
                runningTasks--;
                cv.notify_all();
            }).detach();
        }
        
        if (completedTasks < totalTasks && !failed.load()) {
            cv.wait(lock);
        }
    }
    
    return failed.load() ? exitCode.load() : 0;
}

int runUMakeTarget(const std::filesystem::path& rawPath,
                   const std::string& targetName,
                   const std::filesystem::path& executablePath) {
    UMakeFileData data;
    std::string errorMessage;
    if (!loadUMakeFile(rawPath, executablePath, &data, &errorMessage)) {
        std::cerr << errorMessage << std::endl;
        return 66;
    }

    std::string selectedTarget = targetName.empty() ? chooseDefaultTarget(data) : targetName;
    if (selectedTarget.empty()) {
        std::cerr << "UMake file '" << displayPath(data.path)
                  << "' does not contain a runnable target." << std::endl;
        return 66;
    }

    WorkingDirectoryGuard guard(data.path.parent_path());
    if (g_umakeJobs > 1) {
        return executeTargetParallel(data, selectedTarget, executablePath, g_umakeJobs);
    } else {
        std::unordered_set<std::string> visiting;
        std::unordered_set<std::string> completed;
        return executeTarget(data, selectedTarget, executablePath, &visiting, &completed);
    }
}
