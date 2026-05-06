#include "http_native.h"
#include "heap.h"
#include "object.h"
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool ensureArgCount(int argCount, int expectedCount, std::string* errorMessage) {
    if (argCount == expectedCount) {
        return true;
    }

    return setError(errorMessage,
                    "Expected " + std::to_string(expectedCount) + " argument(s) but got " +
                        std::to_string(argCount) + ".");
}

bool ensureString(const Value& value,
                  const std::string& functionName,
                  int index,
                  std::string* errorMessage) {
    if (value.isString()) {
        return true;
    }

    return setError(errorMessage,
                    functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a string.");
}

#ifdef _WIN32

class ScopedHandle {
public:
    ScopedHandle() : handle_(nullptr) {
    }

    explicit ScopedHandle(HINTERNET handle) : handle_(handle) {
    }

    ~ScopedHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HINTERNET get() const {
        return handle_;
    }

private:
    HINTERNET handle_;
};

bool win32Error(const std::string& action, std::string* errorMessage) {
    DWORD errorCode = GetLastError();
    char* systemMessage = nullptr;
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&systemMessage),
        0,
        nullptr);

    std::string message = action + " failed";
    if (length > 0 && systemMessage != nullptr) {
        message += ": ";
        message.append(systemMessage, systemMessage + length);
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
    } else {
        message += " with Win32 error " + std::to_string(errorCode) + ".";
    }

    if (systemMessage != nullptr) {
        LocalFree(systemMessage);
    }

    return setError(errorMessage, message);
}

bool utf8ToWide(const std::string& input, std::wstring* output, std::string* errorMessage) {
    if (input.empty()) {
        output->clear();
        return true;
    }

    int required = MultiByteToWideChar(CP_UTF8, 0, input.c_str(),
                                       static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        return win32Error("UTF-8 to UTF-16 conversion", errorMessage);
    }

    output->assign(static_cast<std::size_t>(required), L'\0');
    int converted = MultiByteToWideChar(CP_UTF8, 0, input.c_str(),
                                        static_cast<int>(input.size()), output->data(), required);
    if (converted <= 0) {
        return win32Error("UTF-8 to UTF-16 conversion", errorMessage);
    }

    return true;
}

bool wideToUtf8(const std::wstring& input, std::string* output, std::string* errorMessage) {
    if (input.empty()) {
        output->clear();
        return true;
    }

    int required = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                       static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return win32Error("UTF-16 to UTF-8 conversion", errorMessage);
    }

    output->assign(static_cast<std::size_t>(required), '\0');
    int converted = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                        static_cast<int>(input.size()), output->data(), required,
                                        nullptr, nullptr);
    if (converted <= 0) {
        return win32Error("UTF-16 to UTF-8 conversion", errorMessage);
    }

    return true;
}

bool crackUrl(const std::wstring& url,
              std::wstring* host,
              std::wstring* resource,
              INTERNET_PORT* port,
              DWORD* flags,
              std::string* errorMessage) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        return win32Error("URL parsing", errorMessage);
    }

    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return setError(errorMessage, "httpRequest supports only http:// and https:// URLs.");
    }

    host->assign(components.lpszHostName, components.dwHostNameLength);
    resource->assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        resource->append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    if (resource->empty()) {
        *resource = L"/";
    }

    *port = components.nPort;
    *flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    return true;
}

bool queryStatusCode(HINTERNET request, DWORD* statusCode, std::string* errorMessage) {
    DWORD size = sizeof(*statusCode);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             statusCode,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return win32Error("HTTP status query", errorMessage);
    }

    return true;
}

bool queryHeaderString(HINTERNET request,
                       DWORD query,
                       std::string* value,
                       std::string* errorMessage) {
    DWORD size = 0;
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX)) {
        if (GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND) {
            value->clear();
            return true;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return win32Error("HTTP header query", errorMessage);
        }
    }

    std::wstring wideValue(static_cast<std::size_t>(size / sizeof(wchar_t)), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             wideValue.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return win32Error("HTTP header query", errorMessage);
    }

    if (!wideValue.empty() && wideValue.back() == L'\0') {
        wideValue.pop_back();
    }

    return wideToUtf8(wideValue, value, errorMessage);
}

bool readResponseBody(HINTERNET request, std::string* body, std::string* errorMessage) {
    body->clear();

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return win32Error("HTTP response size query", errorMessage);
        }

        if (available == 0) {
            return true;
        }

        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &bytesRead)) {
            return win32Error("HTTP response read", errorMessage);
        }

        chunk.resize(static_cast<std::size_t>(bytesRead));
        body->append(chunk);
    }
}

#endif

} // namespace

Value nativeHttpRequest(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 4, errorMessage) ||
        !ensureString(args[0], "httpRequest", 0, errorMessage) ||
        !ensureString(args[1], "httpRequest", 1, errorMessage) ||
        !ensureString(args[2], "httpRequest", 2, errorMessage) ||
        !ensureString(args[3], "httpRequest", 3, errorMessage)) {
        return Value::nilValue();
    }

#ifndef _WIN32
    setError(errorMessage, "httpRequest is currently supported only on Windows.");
    return Value::nilValue();
#else
    std::wstring wideMethod;
    std::wstring wideUrl;
    if (!utf8ToWide(args[0].asString(), &wideMethod, errorMessage) ||
        !utf8ToWide(args[1].asString(), &wideUrl, errorMessage)) {
        return Value::nilValue();
    }

    std::wstring host;
    std::wstring resource;
    INTERNET_PORT port = 0;
    DWORD requestFlags = 0;
    if (!crackUrl(wideUrl, &host, &resource, &port, &requestFlags, errorMessage)) {
        return Value::nilValue();
    }

    ScopedHandle session(WinHttpOpen(L"Uranium/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0));
    if (session.get() == nullptr) {
        win32Error("HTTP session open", errorMessage);
        return Value::nilValue();
    }

    WinHttpSetTimeouts(session.get(), 5000, 5000, 15000, 15000);

    ScopedHandle connect(WinHttpConnect(session.get(), host.c_str(), port, 0));
    if (connect.get() == nullptr) {
        win32Error("HTTP connect", errorMessage);
        return Value::nilValue();
    }

    ScopedHandle request(WinHttpOpenRequest(connect.get(),
                                            wideMethod.c_str(),
                                            resource.c_str(),
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            requestFlags));
    if (request.get() == nullptr) {
        win32Error("HTTP request open", errorMessage);
        return Value::nilValue();
    }

    std::wstring requestHeaders;
    if (!args[3].asString().empty()) {
        std::wstring contentType;
        if (!utf8ToWide(args[3].asString(), &contentType, errorMessage)) {
            return Value::nilValue();
        }

        requestHeaders = L"Content-Type: ";
        requestHeaders += contentType;
    }

    const std::string& bodyText = args[2].asString();
    LPVOID bodyPointer = bodyText.empty() ? WINHTTP_NO_REQUEST_DATA
                                          : const_cast<char*>(bodyText.data());
    DWORD bodySize = static_cast<DWORD>(bodyText.size());

    if (!WinHttpSendRequest(request.get(),
                            requestHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                   : requestHeaders.c_str(),
                            requestHeaders.empty() ? 0 : static_cast<DWORD>(requestHeaders.size()),
                            bodyPointer,
                            bodySize,
                            bodySize,
                            0)) {
        win32Error("HTTP request send", errorMessage);
        return Value::nilValue();
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        win32Error("HTTP response receive", errorMessage);
        return Value::nilValue();
    }

    DWORD statusCode = 0;
    if (!queryStatusCode(request.get(), &statusCode, errorMessage)) {
        return Value::nilValue();
    }

    std::string responseBody;
    if (!readResponseBody(request.get(), &responseBody, errorMessage)) {
        return Value::nilValue();
    }

    std::string responseContentType;
    if (!queryHeaderString(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                           &responseContentType, errorMessage)) {
        return Value::nilValue();
    }

    MapPtr result = uraniumHeap().allocateMap();
    result->entries["method"] = Value::stringValue(args[0].asString());
    result->entries["url"] = Value::stringValue(args[1].asString());
    result->entries["status"] = Value::numberValue(static_cast<double>(statusCode));
    result->entries["ok"] = Value::boolValue(statusCode >= 200 && statusCode < 300);
    result->entries["body"] = Value::stringValue(responseBody);
    result->entries["contentType"] = Value::stringValue(responseContentType);
    return Value::mapValue(result);
#endif
}
