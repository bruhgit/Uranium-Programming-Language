#include "net_native.h"
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

static bool wsaInitialized = false;
static void ensureWsa() {
    if (!wsaInitialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsaInitialized = true;
    }
}

static bool setErrorStatus(std::string* errorMessage, const std::string& msg) {
    if (errorMessage) {
        *errorMessage = msg + " (Winsock Error: " + std::to_string(WSAGetLastError()) + ")";
    }
    return false;
}

Value nativeNetTcpListen(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isString() || !args[1].isNumber()) {
        if (errorMessage) *errorMessage = "Expected host string and port number";
        return Value::nilValue();
    }
    ensureWsa();
    std::string host = args[0].asString();
    int port = (int)args[1].asNumber();

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        setErrorStatus(errorMessage, "Failed to create socket");
        return Value::nilValue();
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        setErrorStatus(errorMessage, "Bind failed");
        closesocket(listenSocket);
        return Value::nilValue();
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        setErrorStatus(errorMessage, "Listen failed");
        closesocket(listenSocket);
        return Value::nilValue();
    }

    return Value::numberValue((double)listenSocket);
}

Value nativeNetTcpAccept(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isNumber()) {
        if (errorMessage) *errorMessage = "Expected a valid socket number";
        return Value::nilValue();
    }
    SOCKET listenSocket = (SOCKET)args[0].asNumber();
    
    SOCKET clientSocket = accept(listenSocket, NULL, NULL);
    if (clientSocket == INVALID_SOCKET) {
        setErrorStatus(errorMessage, "Accept failed");
        return Value::nilValue();
    }
    return Value::numberValue((double)clientSocket);
}

Value nativeNetTcpReceive(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isNumber() || !args[1].isNumber()) {
        if (errorMessage) *errorMessage = "Expected socket number and buffer size";
        return Value::nilValue();
    }
    SOCKET clientSocket = (SOCKET)args[0].asNumber();
    int bufferSize = (int)args[1].asNumber();
    if (bufferSize <= 0) bufferSize = 1024;

    std::string buffer(bufferSize, '\0');
    int bytesReceived = recv(clientSocket, &buffer[0], bufferSize, 0);
    
    if (bytesReceived < 0) {
        setErrorStatus(errorMessage, "Receive failed");
        return Value::nilValue();
    }
    buffer.resize(bytesReceived);
    return Value::stringValue(buffer);
}

Value nativeNetTcpSend(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isNumber() || !args[1].isString()) {
        if (errorMessage) *errorMessage = "Expected socket number and data string";
        return Value::nilValue();
    }
    SOCKET clientSocket = (SOCKET)args[0].asNumber();
    std::string data = args[1].asString();

    int bytesSent = send(clientSocket, data.c_str(), (int)data.length(), 0);
    if (bytesSent == SOCKET_ERROR) {
        setErrorStatus(errorMessage, "Send failed");
        return Value::nilValue();
    }

    return Value::numberValue((double)bytesSent);
}

Value nativeNetTcpClose(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isNumber()) {
         if (errorMessage) *errorMessage = "Expected a valid socket number";
         return Value::nilValue();
    }
    SOCKET sock = (SOCKET)args[0].asNumber();
    closesocket(sock);
    return Value::nilValue();
}
