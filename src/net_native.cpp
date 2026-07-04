#include "net_native.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

bool setErrorStatus(std::string* errorMessage, const std::string& message) {
    if (errorMessage == nullptr) {
        return false;
    }

#ifdef _WIN32
    *errorMessage =
        message + " (socket error: " + std::to_string(WSAGetLastError()) + ")";
#else
    *errorMessage =
        message + " (socket error: " + std::to_string(errno) + ": " + std::strerror(errno) + ")";
#endif
    return false;
}

#ifdef _WIN32
bool ensureSocketsInitialized(std::string* errorMessage) {
    static bool initialized = false;
    if (initialized) {
        return true;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return setErrorStatus(errorMessage, "Could not initialize Winsock");
    }

    initialized = true;
    return true;
}
#else
bool ensureSocketsInitialized(std::string*) {
    return true;
}
#endif

void closeSocket(NativeSocket socketHandle) {
    if (socketHandle == kInvalidSocket) {
        return;
    }

#ifdef _WIN32
    closesocket(socketHandle);
#else
    ::close(socketHandle);
#endif
}

bool ensureSocketNumber(const Value& value, const std::string& functionName,
                        std::string* errorMessage) {
    if (value.isNumber()) {
        return true;
    }

    if (errorMessage != nullptr) {
        *errorMessage = functionName + " expects a socket number.";
    }
    return false;
}

NativeSocket asSocket(const Value& value) {
#ifdef _WIN32
    return static_cast<NativeSocket>(static_cast<unsigned long long>(value.asNumber()));
#else
    return static_cast<NativeSocket>(static_cast<int>(value.asNumber()));
#endif
}

std::string portString(int port) {
    return std::to_string(port);
}

bool configureReusableSocket(NativeSocket socketHandle, std::string* errorMessage) {
    int enabled = 1;
    if (setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enabled),
                   static_cast<socklen_t>(sizeof(enabled))) != 0) {
        return setErrorStatus(errorMessage, "Could not enable SO_REUSEADDR");
    }
    return true;
}

} // namespace

Value nativeNetTcpConnect(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isNumber()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected host string and port number.";
        }
        return Value::nilValue();
    }

    if (!ensureSocketsInitialized(errorMessage)) {
        return Value::nilValue();
    }

    std::string host = args[0].asString();
    int port = static_cast<int>(args[1].asNumber());
    if (host.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Host must not be empty.";
        }
        return Value::nilValue();
    }

    if (port < 0 || port > 65535) {
        if (errorMessage != nullptr) {
            *errorMessage = "Port must be between 0 and 65535.";
        }
        return Value::nilValue();
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), portString(port).c_str(), &hints, &results) != 0) {
        setErrorStatus(errorMessage, "Could not resolve connect address");
        return Value::nilValue();
    }

    NativeSocket clientSocket = kInvalidSocket;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        clientSocket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (clientSocket == kInvalidSocket) {
            continue;
        }

        if (connect(clientSocket, current->ai_addr,
                    static_cast<socklen_t>(current->ai_addrlen)) == 0) {
            break;
        }

        closeSocket(clientSocket);
        clientSocket = kInvalidSocket;
    }

    freeaddrinfo(results);

    if (clientSocket == kInvalidSocket) {
        setErrorStatus(errorMessage, "Could not connect to remote host");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(clientSocket));
}

Value nativeNetTcpListen(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isNumber()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Expected host string and port number.";
        }
        return Value::nilValue();
    }

    if (!ensureSocketsInitialized(errorMessage)) {
        return Value::nilValue();
    }

    std::string host = args[0].asString();
    int port = static_cast<int>(args[1].asNumber());
    if (port < 0 || port > 65535) {
        if (errorMessage != nullptr) {
            *errorMessage = "Port must be between 0 and 65535.";
        }
        return Value::nilValue();
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const char* hostName = host.empty() || host == "*" ? nullptr : host.c_str();
    addrinfo* results = nullptr;
    if (getaddrinfo(hostName, portString(port).c_str(), &hints, &results) != 0) {
        setErrorStatus(errorMessage, "Could not resolve bind address");
        return Value::nilValue();
    }

    NativeSocket listenSocket = kInvalidSocket;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        listenSocket = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (listenSocket == kInvalidSocket) {
            continue;
        }

        if (!configureReusableSocket(listenSocket, errorMessage)) {
            closeSocket(listenSocket);
            listenSocket = kInvalidSocket;
            continue;
        }

        if (bind(listenSocket, current->ai_addr,
                 static_cast<socklen_t>(current->ai_addrlen)) == 0 &&
            listen(listenSocket, SOMAXCONN) == 0) {
            break;
        }

        closeSocket(listenSocket);
        listenSocket = kInvalidSocket;
    }

    freeaddrinfo(results);

    if (listenSocket == kInvalidSocket) {
        setErrorStatus(errorMessage, "Could not create listening socket");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(listenSocket));
}

Value nativeNetTcpAccept(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !ensureSocketNumber(args[0], "netTcpAccept", errorMessage)) {
        return Value::nilValue();
    }

    NativeSocket listenSocket = asSocket(args[0]);
    NativeSocket clientSocket = accept(listenSocket, nullptr, nullptr);
    if (clientSocket == kInvalidSocket) {
        setErrorStatus(errorMessage, "Could not accept client connection");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(clientSocket));
}

Value nativeNetTcpReceive(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !ensureSocketNumber(args[0], "netTcpReceive", errorMessage) ||
        !args[1].isNumber()) {
        if (errorMessage != nullptr && (argCount != 2 || !args[1].isNumber())) {
            *errorMessage = "Expected socket number and buffer size.";
        }
        return Value::nilValue();
    }

    NativeSocket clientSocket = asSocket(args[0]);
    int bufferSize = static_cast<int>(args[1].asNumber());
    if (bufferSize <= 0) {
        bufferSize = 1024;
    }

    std::string buffer(static_cast<std::size_t>(bufferSize), '\0');
    int bytesReceived =
        recv(clientSocket, &buffer[0], bufferSize, 0);
    if (bytesReceived < 0) {
        setErrorStatus(errorMessage, "Could not receive data");
        return Value::nilValue();
    }

    buffer.resize(static_cast<std::size_t>(bytesReceived));
    return Value::stringValue(buffer);
}

Value nativeNetTcpSend(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !ensureSocketNumber(args[0], "netTcpSend", errorMessage) ||
        !args[1].isString()) {
        if (errorMessage != nullptr && (argCount != 2 || !args[1].isString())) {
            *errorMessage = "Expected socket number and data string.";
        }
        return Value::nilValue();
    }

    NativeSocket clientSocket = asSocket(args[0]);
    const std::string& data = args[1].asString();
    int bytesSent = send(clientSocket, data.data(), static_cast<int>(data.size()), 0);
    if (bytesSent < 0) {
        setErrorStatus(errorMessage, "Could not send data");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(bytesSent));
}

Value nativeNetTcpClose(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !ensureSocketNumber(args[0], "netTcpClose", errorMessage)) {
        return Value::nilValue();
    }

    closeSocket(asSocket(args[0]));
    return Value::nilValue();
}
