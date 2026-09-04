#include "microcode_native.h"
#include "heap.h"
#include "object.h"
#include "value.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
using SerialHandle = HANDLE;
const SerialHandle INVALID_SERIAL = INVALID_HANDLE_VALUE;
#else
using SerialHandle = int;
const SerialHandle INVALID_SERIAL = -1;
#endif

std::unordered_map<int, SerialHandle> g_openPorts;
int g_nextHandleId = 1;

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool ensureArgCount(int argCount, int minArgs, int maxArgs, std::string* errorMessage) {
    if (argCount >= minArgs && argCount <= maxArgs) {
        return true;
    }
    return setError(errorMessage, "Expected between " + std::to_string(minArgs) +
                                      " and " + std::to_string(maxArgs) + " arguments, got " +
                                      std::to_string(argCount) + ".");
}

bool runProcessCapture(const std::string& command, int* exitCode, std::string* output) {
#ifdef _WIN32
    std::string fullCmd = command + " 2>&1";
    FILE* pipe = _popen(fullCmd.c_str(), "r");
#else
    std::string fullCmd = command + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
#endif
    if (!pipe) {
        if (output) *output = "Failed to spawn process.";
        if (exitCode) *exitCode = -1;
        return false;
    }

    std::string result;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }

#ifdef _WIN32
    int code = _pclose(pipe);
#else
    int code = pclose(pipe);
#endif

    if (output) *output = result;
    if (exitCode) *exitCode = code;
    return true;
}

std::string findArduinoCli(const std::string& userSpecifiedPath) {
    if (!userSpecifiedPath.empty() && fs::exists(userSpecifiedPath)) {
        return userSpecifiedPath;
    }

    // Check if in PATH
#ifdef _WIN32
    int exitCode = 0;
    std::string out;
    if (runProcessCapture("where.exe arduino-cli", &exitCode, &out) && exitCode == 0) {
        std::istringstream iss(out);
        std::string line;
        if (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (fs::exists(line)) return line;
        }
    }

    // Check common Windows locations
    std::vector<std::string> candidates = {
        (fs::current_path() / "bin" / "arduino-cli.exe").string(),
        (fs::current_path() / "urlib" / "microcode" / "bin" / "arduino-cli.exe").string(),
        (fs::current_path() / "arduino-cli.exe").string(),
        "C:\\Program Files\\Arduino CLI\\arduino-cli.exe",
        "C:\\Program Files (x86)\\Arduino CLI\\arduino-cli.exe",
        "C:\\arduino-cli\\arduino-cli.exe"
    };

    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        candidates.push_back(std::string(localAppData) + "\\Arduino15\\bin\\arduino-cli.exe");
        candidates.push_back(std::string(localAppData) + "\\Programs\\Arduino IDE\\resources\\app\\lib\\backend\\resources\\arduino-cli.exe");
    }
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile) {
        candidates.push_back(std::string(userProfile) + "\\bin\\arduino-cli.exe");
        candidates.push_back(std::string(userProfile) + "\\AppData\\Local\\Arduino15\\bin\\arduino-cli.exe");
    }

    for (const auto& path : candidates) {
        if (fs::exists(path)) return path;
    }
#else
    int exitCode = 0;
    std::string out;
    if (runProcessCapture("which arduino-cli", &exitCode, &out) && exitCode == 0) {
        std::istringstream iss(out);
        std::string line;
        if (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (fs::exists(line)) return line;
        }
    }
    std::vector<std::string> candidates = {
        "/usr/local/bin/arduino-cli",
        "/usr/bin/arduino-cli",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/bin/arduino-cli"
    };
    for (const auto& path : candidates) {
        if (!path.empty() && fs::exists(path)) return path;
    }
#endif

    return "";
}

} // namespace

// 1. List available serial ports (COMx on Windows, /dev/tty on Unix)
Value nativeMicrocodeListPorts(int argCount, const Value* args, std::string* errorMessage) {
    (void)argCount;
    (void)args;
    (void)errorMessage;

    ArrayPtr arr = uraniumHeap().allocateArray();

#ifdef _WIN32
    // Enumerate via Windows Registry: HARDWARE\DEVICEMAP\SERIALCOMM
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char valueName[256];
        BYTE data[256];
        DWORD index = 0;
        DWORD valLen = sizeof(valueName);
        DWORD dataLen = sizeof(data);
        DWORD type = 0;

        while (RegEnumValueA(hKey, index, valueName, &valLen, NULL, &type, data, &dataLen) == ERROR_SUCCESS) {
            if (type == REG_SZ) {
                std::string portStr(reinterpret_cast<char*>(data));
                arr->elements.push_back(Value::stringValue(portStr));
            }
            index++;
            valLen = sizeof(valueName);
            dataLen = sizeof(data);
        }
        RegCloseKey(hKey);
    }

    // Fallback if registry had none: test QueryDosDevice for COM1..COM32
    if (arr->elements.empty()) {
        char targetPath[512];
        for (int i = 1; i <= 32; ++i) {
            std::string portName = "COM" + std::to_string(i);
            if (QueryDosDeviceA(portName.c_str(), targetPath, sizeof(targetPath)) != 0) {
                arr->elements.push_back(Value::stringValue(portName));
            }
        }
    }
#else
    // Scan /dev for ttyUSB*, ttyACM*, etc.
    DIR* dir = opendir("/dev");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.find("ttyUSB") == 0 || name.find("ttyACM") == 0 || name.find("cu.usb") == 0) {
                arr->elements.push_back(Value::stringValue("/dev/" + name));
            }
        }
        closedir(dir);
    }
#endif

    return Value::arrayValue(arr);
}

// 2. Open serial port: microcodeOpen(port, baudRate = 115200)
Value nativeMicrocodeOpen(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, 2, errorMessage)) {
        return Value::numberValue(-1);
    }
    if (!args[0].isString()) {
        setError(errorMessage, "Port name must be a string.");
        return Value::numberValue(-1);
    }

    std::string port = args[0].asString();
    int baudRate = 115200;
    if (argCount >= 2 && args[1].isNumber()) {
        baudRate = static_cast<int>(args[1].asNumber());
    }

#ifdef _WIN32
    std::string devicePath = "\\\\.\\" + port;
    HANDLE h = CreateFileA(devicePath.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           NULL,
                           OPEN_EXISTING,
                           0,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return Value::numberValue(-1);
    }

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(h, &dcbSerialParams)) {
        CloseHandle(h);
        return Value::numberValue(-1);
    }

    dcbSerialParams.BaudRate = baudRate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
    dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcbSerialParams)) {
        CloseHandle(h);
        return Value::numberValue(-1);
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 1;
    timeouts.WriteTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 1;

    SetCommTimeouts(h, &timeouts);

    int id = g_nextHandleId++;
    g_openPorts[id] = h;
    return Value::numberValue(id);
#else
    int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        return Value::numberValue(-1);
    }

    struct termios options;
    tcgetattr(fd, &options);
    speed_t speed = B115200;
    if (baudRate == 9600) speed = B9600;
    else if (baudRate == 19200) speed = B19200;
    else if (baudRate == 38400) speed = B38400;
    else if (baudRate == 57600) speed = B57600;
    else if (baudRate == 115200) speed = B115200;

    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;

    tcsetattr(fd, TCSANOW, &options);

    int id = g_nextHandleId++;
    g_openPorts[id] = fd;
    return Value::numberValue(id);
#endif
}

// 3. Close serial port: microcodeClose(handleId)
Value nativeMicrocodeClose(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, 1, errorMessage) || !args[0].isNumber()) {
        return Value::boolValue(false);
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::boolValue(false);

#ifdef _WIN32
    CloseHandle(it->second);
#else
    close(it->second);
#endif
    g_openPorts.erase(it);
    return Value::boolValue(true);
}

// 4. Write data: microcodeWrite(handleId, data)
Value nativeMicrocodeWrite(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, 2, errorMessage) || !args[0].isNumber() || !args[1].isString()) {
        return Value::numberValue(0);
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::numberValue(0);

    std::string text = args[1].asString();
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(it->second, text.data(), static_cast<DWORD>(text.size()), &written, NULL);
    return Value::numberValue(static_cast<double>(written));
#else
    ssize_t written = write(it->second, text.data(), text.size());
    return Value::numberValue(written > 0 ? static_cast<double>(written) : 0);
#endif
}

// 5. Read data: microcodeRead(handleId, maxBytes = 1024, timeoutMs = 100)
Value nativeMicrocodeRead(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, 3, errorMessage) || !args[0].isNumber()) {
        return Value::stringValue("");
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::stringValue("");

    int maxBytes = 1024;
    if (argCount >= 2 && args[1].isNumber()) {
        maxBytes = static_cast<int>(args[1].asNumber());
    }

    std::vector<char> buf(maxBytes + 1, 0);
#ifdef _WIN32
    DWORD readBytes = 0;
    if (ReadFile(it->second, buf.data(), static_cast<DWORD>(maxBytes), &readBytes, NULL) && readBytes > 0) {
        return Value::stringValue(std::string(buf.data(), readBytes));
    }
#else
    ssize_t readBytes = read(it->second, buf.data(), maxBytes);
    if (readBytes > 0) {
        return Value::stringValue(std::string(buf.data(), readBytes));
    }
#endif
    return Value::stringValue("");
}

// 6. Read line: microcodeReadLine(handleId, timeoutMs = 1000)
Value nativeMicrocodeReadLine(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, 2, errorMessage) || !args[0].isNumber()) {
        return Value::stringValue("");
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::stringValue("");

    int timeoutMs = 1000;
    if (argCount >= 2 && args[1].isNumber()) {
        timeoutMs = static_cast<int>(args[1].asNumber());
    }

    std::string line;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        char ch = 0;
#ifdef _WIN32
        DWORD readBytes = 0;
        if (ReadFile(it->second, &ch, 1, &readBytes, NULL) && readBytes == 1) {
            if (ch == '\n') break;
            if (ch != '\r') line += ch;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#else
        ssize_t readBytes = read(it->second, &ch, 1);
        if (readBytes == 1) {
            if (ch == '\n') break;
            if (ch != '\r') line += ch;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#endif
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeoutMs) {
            break;
        }
    }

    return Value::stringValue(line);
}

// 7. Set DTR line
Value nativeMicrocodeSetDTR(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, 2, errorMessage) || !args[0].isNumber() || !args[1].isBool()) {
        return Value::boolValue(false);
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::boolValue(false);

    bool state = args[1].asBool();
#ifdef _WIN32
    EscapeCommFunction(it->second, state ? SETDTR : CLRDTR);
#else
    int status;
    ioctl(it->second, TIOCMGET, &status);
    if (state) status |= TIOCM_DTR;
    else status &= ~TIOCM_DTR;
    ioctl(it->second, TIOCMSET, &status);
#endif
    return Value::boolValue(true);
}

// 8. Set RTS line
Value nativeMicrocodeSetRTS(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, 2, errorMessage) || !args[0].isNumber() || !args[1].isBool()) {
        return Value::boolValue(false);
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::boolValue(false);

    bool state = args[1].asBool();
#ifdef _WIN32
    EscapeCommFunction(it->second, state ? SETRTS : CLRRTS);
#else
    int status;
    ioctl(it->second, TIOCMGET, &status);
    if (state) status |= TIOCM_RTS;
    else status &= ~TIOCM_RTS;
    ioctl(it->second, TIOCMSET, &status);
#endif
    return Value::boolValue(true);
}

// 9. Hardware reset for ESP32 using DTR/RTS pulse (esptool pattern)
Value nativeMicrocodeResetEsp32(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, 1, errorMessage) || !args[0].isNumber()) {
        return Value::boolValue(false);
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::boolValue(false);

#ifdef _WIN32
    // ESP32 reset sequence:
    // IO0 is controlled by DTR, EN (reset) is controlled by RTS
    EscapeCommFunction(it->second, SETRTS);
    EscapeCommFunction(it->second, CLRDTR);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EscapeCommFunction(it->second, CLRRTS);
    EscapeCommFunction(it->second, SETDTR);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EscapeCommFunction(it->second, CLRRTS);
    EscapeCommFunction(it->second, CLRDTR);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
    return Value::boolValue(true);
}

// 10. Execute serial command and await response: microcodeExecute(handleId, command, timeoutMs = 2000)
Value nativeMicrocodeExecute(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, 3, errorMessage) || !args[0].isNumber() || !args[1].isString()) {
        return Value::stringValue("");
    }
    int id = static_cast<int>(args[0].asNumber());
    auto it = g_openPorts.find(id);
    if (it == g_openPorts.end()) return Value::stringValue("");

    std::string cmd = args[1].asString() + "\r\n";
    int timeoutMs = 2000;
    if (argCount >= 3 && args[2].isNumber()) {
        timeoutMs = static_cast<int>(args[2].asNumber());
    }

#ifdef _WIN32
    DWORD written = 0;
    WriteFile(it->second, cmd.data(), static_cast<DWORD>(cmd.size()), &written, NULL);
#else
    write(it->second, cmd.data(), cmd.size());
#endif

    std::string output;
    auto start = std::chrono::steady_clock::now();
    char buf[256];

    while (true) {
#ifdef _WIN32
        DWORD readBytes = 0;
        if (ReadFile(it->second, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
            output.append(buf, readBytes);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
#else
        ssize_t readBytes = read(it->second, buf, sizeof(buf) - 1);
        if (readBytes > 0) {
            output.append(buf, readBytes);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
#endif
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeoutMs) {
            break;
        }
    }

    return Value::stringValue(output);
}

// 11. Compile & Flash sketch using Arduino toolchain (arduino-cli)
// microcodeCompileAndFlash(sketchPath, fqbn, port, customCliPath = "")
Value nativeMicrocodeCompileAndFlash(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 3, 4, errorMessage)) {
        return Value::nilValue();
    }
    if (!args[0].isString() || !args[1].isString() || !args[2].isString()) {
        setError(errorMessage, "Arguments must be strings (sketchPath, fqbn, port).");
        return Value::nilValue();
    }

    std::string sketchPath = args[0].asString();
    std::string fqbn = args[1].asString();
    std::string port = args[2].asString();
    std::string customCli = (argCount >= 4 && args[3].isString()) ? args[3].asString() : "";

    std::string cliPath = findArduinoCli(customCli);

    MapPtr map = uraniumHeap().allocateMap();
    map->entries["sketch"] = Value::stringValue(sketchPath);
    map->entries["fqbn"] = Value::stringValue(fqbn);
    map->entries["port"] = Value::stringValue(port);

    if (cliPath.empty()) {
        map->entries["cliFound"] = Value::boolValue(false);
        map->entries["ok"] = Value::boolValue(false);
        map->entries["exitCode"] = Value::numberValue(-1);
        map->entries["output"] = Value::stringValue(
            "arduino-cli executable not found! Please install arduino-cli or specify its path."
        );
        return Value::mapValue(map);
    }

    map->entries["cliFound"] = Value::boolValue(true);
    map->entries["cliPath"] = Value::stringValue(cliPath);

    // 1. Compile step: arduino-cli compile --fqbn <fqbn> <sketchPath>
    std::string compileCmd = "\"" + cliPath + "\" compile --fqbn " + fqbn + " \"" + sketchPath + "\"";
    int compileCode = 0;
    std::string compileOutput;
    runProcessCapture(compileCmd, &compileCode, &compileOutput);

    if (compileCode != 0) {
        map->entries["ok"] = Value::boolValue(false);
        map->entries["exitCode"] = Value::numberValue(compileCode);
        map->entries["output"] = Value::stringValue("Compilation failed:\n" + compileOutput);
        return Value::mapValue(map);
    }

    // 2. Upload / Flash step: arduino-cli upload -p <port> --fqbn <fqbn> <sketchPath>
    std::string uploadCmd = "\"" + cliPath + "\" upload -p " + port + " --fqbn " + fqbn + " \"" + sketchPath + "\"";
    int uploadCode = 0;
    std::string uploadOutput;
    runProcessCapture(uploadCmd, &uploadCode, &uploadOutput);

    bool success = (uploadCode == 0);
    map->entries["ok"] = Value::boolValue(success);
    map->entries["exitCode"] = Value::numberValue(uploadCode);
    map->entries["output"] = Value::stringValue("Compiled successfully.\nUpload output:\n" + uploadOutput);

    return Value::mapValue(map);
}
