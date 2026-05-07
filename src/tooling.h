#ifndef uranium_tooling_h
#define uranium_tooling_h

#include <filesystem>
#include <string>
#include <vector>

enum ToolSeverity {
    TOOL_SEVERITY_ERROR = 1,
    TOOL_SEVERITY_WARNING = 2,
    TOOL_SEVERITY_INFORMATION = 3,
};

struct ToolDiagnostic {
    int line = 1;
    int column = 1;
    int endLine = 1;
    int endColumn = 1;
    int severity = TOOL_SEVERITY_WARNING;
    std::string code;
    std::string message;
};

bool formatUraniumSource(const std::string& source,
                         int indentSize,
                         std::string* formattedSource);

void lintUraniumSource(const std::string& source,
                       std::vector<ToolDiagnostic>* diagnostics);

int formatPath(const std::filesystem::path& targetPath,
               bool checkOnly,
               int indentSize);

int lintPath(const std::filesystem::path& targetPath,
             const std::filesystem::path& executablePath);

int debugPath(const std::filesystem::path& targetPath,
              const std::filesystem::path& executablePath);

int debugRunPath(const std::filesystem::path& targetPath,
                 const std::filesystem::path& executablePath,
                 const std::vector<std::string>& scriptArgs);

int runLspServer(const std::filesystem::path& executablePath);

#endif
