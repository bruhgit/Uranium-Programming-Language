#include "gui_native.h"

#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

constexpr const wchar_t* kPromptWindowClass = L"UraniumPromptDialogWindow";
constexpr const wchar_t* kGuiWindowClass = L"UraniumMainGuiWindow";

enum class GuiControlKind {
    Label,
    Button,
    Input,
    TextArea,
    Checkbox,
    ListBox,
    ProgressBar,
};

struct GuiWindowState {
    int id;
    HWND handle;
};

struct GuiControlState {
    int id;
    int windowId;
    HWND handle;
    GuiControlKind kind;
};

struct GuiEventState {
    std::string type;
    int windowId;
    int controlId;
    std::string text;
    bool checked;
    int index;

    GuiEventState()
        : type("none"), windowId(0), controlId(0), checked(false), index(-1) {
    }
};

struct GuiRuntimeState {
    int nextWindowId;
    int nextControlId;
    bool windowClassReady;
    bool commonControlsReady;
    std::unordered_map<int, GuiWindowState> windowsById;
    std::unordered_map<HWND, int> windowIdsByHandle;
    std::unordered_map<int, GuiControlState> controlsById;
    std::unordered_map<HWND, int> controlIdsByHandle;
    std::deque<GuiEventState> queuedEvents;
    GuiEventState currentEvent;

    GuiRuntimeState()
        : nextWindowId(1),
          nextControlId(1000),
          windowClassReady(false),
          commonControlsReady(false) {
    }
};

GuiRuntimeState& guiRuntime() {
    static GuiRuntimeState runtime;
    return runtime;
}

std::string hresultMessage(const std::string& context, HRESULT result) {
    std::ostringstream stream;
    stream << context << " failed (HRESULT 0x"
           << std::hex << std::uppercase << static_cast<unsigned long>(result) << ").";
    return stream.str();
}

bool writeError(const std::string& message, std::string* errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool utf8ToWide(const std::string& value, std::wstring* wideValue, std::string* errorMessage) {
    if (wideValue == nullptr) {
        return writeError("Internal GUI conversion error.", errorMessage);
    }

    if (value.empty()) {
        wideValue->clear();
        return true;
    }

    int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return writeError("GUI expects valid UTF-8 text.", errorMessage);
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, buffer.data(), required) <= 0) {
        return writeError("GUI expects valid UTF-8 text.", errorMessage);
    }

    *wideValue = buffer.data();
    return true;
}

bool wideToUtf8(const std::wstring& value, std::string* utf8Value, std::string* errorMessage) {
    if (utf8Value == nullptr) {
        return writeError("Internal GUI conversion error.", errorMessage);
    }

    if (value.empty()) {
        utf8Value->clear();
        return true;
    }

    int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return writeError("GUI could not convert text from Windows.", errorMessage);
    }

    std::vector<char> buffer(static_cast<std::size_t>(required));
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, buffer.data(), required,
            nullptr, nullptr) <= 0) {
        return writeError("GUI could not convert text from Windows.", errorMessage);
    }

    *utf8Value = buffer.data();
    return true;
}

UINT messageBoxIconFlags(GuiMessageKind kind) {
    switch (kind) {
        case GuiMessageKind::Info:
            return MB_ICONINFORMATION;
        case GuiMessageKind::Warning:
            return MB_ICONWARNING;
        case GuiMessageKind::Error:
            return MB_ICONERROR;
    }

    return MB_ICONINFORMATION;
}

class ScopedComInitializer {
public:
    ScopedComInitializer() : initialized(false) {
    }

    bool initialize(std::string* errorMessage) {
        HRESULT result =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(result)) {
            initialized = true;
            return true;
        }

        if (result == RPC_E_CHANGED_MODE) {
            return true;
        }

        return writeError(hresultMessage("COM initialization", result), errorMessage);
    }

    ~ScopedComInitializer() {
        if (initialized) {
            CoUninitialize();
        }
    }

private:
    bool initialized;
};

void applyDefaultGuiFont(HWND window) {
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (font != nullptr) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

bool ensurePositiveSize(int width, int height, const std::string& context,
                        std::string* errorMessage) {
    if (width <= 0 || height <= 0) {
        return writeError(context + " expects positive width and height.", errorMessage);
    }

    return true;
}

GuiWindowState* getWindowState(int windowId) {
    GuiRuntimeState& runtime = guiRuntime();
    auto it = runtime.windowsById.find(windowId);
    if (it == runtime.windowsById.end()) {
        return nullptr;
    }

    return &it->second;
}

GuiControlState* getControlState(int controlId) {
    GuiRuntimeState& runtime = guiRuntime();
    auto it = runtime.controlsById.find(controlId);
    if (it == runtime.controlsById.end()) {
        return nullptr;
    }

    return &it->second;
}

void clearCurrentEvent() {
    guiRuntime().currentEvent = GuiEventState();
}

bool consumeQueuedEvent(bool* hasEvent, std::string* errorMessage) {
    if (hasEvent == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    GuiRuntimeState& runtime = guiRuntime();
    if (runtime.queuedEvents.empty()) {
        clearCurrentEvent();
        *hasEvent = false;
        return true;
    }

    runtime.currentEvent = runtime.queuedEvents.front();
    runtime.queuedEvents.pop_front();
    *hasEvent = true;
    return true;
}

bool hasOpenWindows() {
    return !guiRuntime().windowsById.empty();
}

bool getWindowTextUtf8(HWND handle, std::string* result, std::string* errorMessage) {
    if (result == nullptr) {
        return writeError("Internal GUI text error.", errorMessage);
    }

    int length = GetWindowTextLengthW(handle);
    if (length < 0) {
        result->clear();
        return true;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    GetWindowTextW(handle, buffer.data(), length + 1);
    return wideToUtf8(buffer.data(), result, errorMessage);
}

std::string bestEffortWindowText(HWND handle) {
    std::string result;
    std::string ignoredError;
    getWindowTextUtf8(handle, &result, &ignoredError);
    return result;
}

std::string bestEffortControlText(const GuiControlState& control, int explicitIndex = -1) {
    if (control.kind == GuiControlKind::ListBox) {
        int index = explicitIndex;
        if (index < 0) {
            index = static_cast<int>(SendMessageW(control.handle, LB_GETCURSEL, 0, 0));
        }

        if (index == LB_ERR) {
            return "";
        }

        LRESULT length = SendMessageW(control.handle, LB_GETTEXTLEN, index, 0);
        if (length == LB_ERR) {
            return "";
        }

        std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
        if (SendMessageW(
                control.handle, LB_GETTEXT, index,
                reinterpret_cast<LPARAM>(buffer.data())) == LB_ERR) {
            return "";
        }

        std::string result;
        std::string ignoredError;
        wideToUtf8(buffer.data(), &result, &ignoredError);
        return result;
    }

    return bestEffortWindowText(control.handle);
}

void queueEvent(const GuiEventState& eventState) {
    guiRuntime().queuedEvents.push_back(eventState);
}

void removeWindowState(int windowId) {
    GuiRuntimeState& runtime = guiRuntime();

    auto windowIt = runtime.windowsById.find(windowId);
    if (windowIt != runtime.windowsById.end()) {
        runtime.windowIdsByHandle.erase(windowIt->second.handle);
        runtime.windowsById.erase(windowIt);
    }

    std::vector<int> controlsToRemove;
    for (const auto& pair : runtime.controlsById) {
        if (pair.second.windowId == windowId) {
            controlsToRemove.push_back(pair.first);
        }
    }

    for (int controlId : controlsToRemove) {
        auto controlIt = runtime.controlsById.find(controlId);
        if (controlIt != runtime.controlsById.end()) {
            runtime.controlIdsByHandle.erase(controlIt->second.handle);
            runtime.controlsById.erase(controlIt);
        }
    }
}

void queueWindowCloseEvent(int windowId, HWND handle) {
    GuiEventState eventState;
    eventState.type = "close";
    eventState.windowId = windowId;
    eventState.text = bestEffortWindowText(handle);
    queueEvent(eventState);
}

void queueControlEvent(const std::string& type, const GuiControlState& control) {
    GuiEventState eventState;
    eventState.type = type;
    eventState.windowId = control.windowId;
    eventState.controlId = control.id;
    eventState.text = bestEffortControlText(control);

    if (control.kind == GuiControlKind::Checkbox) {
        eventState.checked =
            SendMessageW(control.handle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    if (control.kind == GuiControlKind::ListBox) {
        int index = static_cast<int>(SendMessageW(control.handle, LB_GETCURSEL, 0, 0));
        if (index == LB_ERR) {
            eventState.index = -1;
            eventState.text.clear();
        } else {
            eventState.index = index;
            eventState.text = bestEffortControlText(control, index);
        }
    }

    queueEvent(eventState);
}

LRESULT CALLBACK guiWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    int windowId = static_cast<int>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            int createWindowId = static_cast<int>(
                reinterpret_cast<std::intptr_t>(create->lpCreateParams));
            SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(createWindowId));
            return TRUE;
        }
        case WM_CLOSE:
            queueWindowCloseEvent(windowId, window);
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            removeWindowState(windowId);
            return 0;
        case WM_COMMAND: {
            GuiControlState* control =
                getControlState(static_cast<int>(LOWORD(wParam)));
            if (control == nullptr) {
                break;
            }

            int notificationCode = HIWORD(wParam);
            switch (control->kind) {
                case GuiControlKind::Button:
                    if (notificationCode == BN_CLICKED) {
                        queueControlEvent("click", *control);
                    }
                    break;
                case GuiControlKind::Checkbox:
                    if (notificationCode == BN_CLICKED) {
                        queueControlEvent("toggle", *control);
                    }
                    break;
                case GuiControlKind::Input:
                case GuiControlKind::TextArea:
                    if (notificationCode == EN_CHANGE) {
                        queueControlEvent("change", *control);
                    }
                    break;
                case GuiControlKind::ListBox:
                    if (notificationCode == LBN_SELCHANGE) {
                        queueControlEvent("select", *control);
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }
        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool ensureCommonControls(std::string* errorMessage) {
    GuiRuntimeState& runtime = guiRuntime();
    if (runtime.commonControlsReady) {
        return true;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&commonControls)) {
        return writeError("Windows common controls could not be initialized.", errorMessage);
    }

    runtime.commonControlsReady = true;
    return true;
}

bool ensureGuiWindowClass(std::string* errorMessage) {
    GuiRuntimeState& runtime = guiRuntime();
    if (runtime.windowClassReady) {
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = guiWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kGuiWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    ATOM registration = RegisterClassW(&windowClass);
    if (registration == 0) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return writeError("Could not register the Uranium GUI window class.", errorMessage);
        }
    }

    runtime.windowClassReady = true;
    return true;
}

bool createControl(HWND parentHandle, GuiControlKind kind,
                   const wchar_t* className, const std::wstring& text,
                   DWORD style, DWORD exStyle,
                   int x, int y, int width, int height,
                   int* controlId, std::string* errorMessage) {
    if (controlId == nullptr) {
        return writeError("Internal GUI control error.", errorMessage);
    }

    if (!ensurePositiveSize(width, height, "GUI control creation", errorMessage)) {
        return false;
    }

    GuiRuntimeState& runtime = guiRuntime();
    int nextId = runtime.nextControlId++;
    HINSTANCE instance = GetModuleHandleW(nullptr);

    HWND handle = CreateWindowExW(
        exStyle, className, text.c_str(),
        WS_CHILD | WS_VISIBLE | style,
        x, y, width, height,
        parentHandle, reinterpret_cast<HMENU>(static_cast<std::intptr_t>(nextId)),
        instance, nullptr);

    if (handle == nullptr) {
        return writeError("Windows could not create a GUI control.", errorMessage);
    }

    applyDefaultGuiFont(handle);

    auto parentWindowIt = runtime.windowIdsByHandle.find(parentHandle);
    if (parentWindowIt == runtime.windowIdsByHandle.end()) {
        DestroyWindow(handle);
        return writeError("Internal GUI parent window error.", errorMessage);
    }

    GuiControlState state{};
    state.id = nextId;
    state.windowId = parentWindowIt->second;
    state.handle = handle;
    state.kind = kind;

    runtime.controlsById[nextId] = state;
    runtime.controlIdsByHandle[handle] = nextId;
    *controlId = nextId;
    return true;
}

bool processPendingMessages(std::string* errorMessage) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            clearCurrentEvent();
            return true;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    (void)errorMessage;
    return true;
}

struct PromptDialogState {
    std::wstring title;
    std::wstring message;
    std::wstring value;
    bool accepted;
    HWND editHandle;

    PromptDialogState()
        : accepted(false), editHandle(nullptr) {
    }
};

LRESULT CALLBACK promptWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    PromptDialogState* state = reinterpret_cast<PromptDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            state = reinterpret_cast<PromptDialogState*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (state == nullptr) {
                return -1;
            }

            HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));

            HWND label = CreateWindowExW(
                0, L"STATIC", state->message.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                16, 16, 432, 40,
                window, nullptr, instance, nullptr);

            state->editHandle = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", state->value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                16, 64, 432, 24,
                window, reinterpret_cast<HMENU>(1001), instance, nullptr);

            HWND okButton = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                272, 104, 80, 28,
                window, reinterpret_cast<HMENU>(IDOK), instance, nullptr);

            HWND cancelButton = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                368, 104, 80, 28,
                window, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);

            applyDefaultGuiFont(label);
            applyDefaultGuiFont(state->editHandle);
            applyDefaultGuiFont(okButton);
            applyDefaultGuiFont(cancelButton);

            SendMessageW(state->editHandle, EM_SETSEL, 0, -1);
            SetFocus(state->editHandle);
            return 0;
        }
        case WM_COMMAND: {
            if (state == nullptr) {
                break;
            }

            switch (LOWORD(wParam)) {
                case IDOK: {
                    int length = GetWindowTextLengthW(state->editHandle);
                    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
                    GetWindowTextW(state->editHandle, buffer.data(), length + 1);
                    state->value = buffer.data();
                    state->accepted = true;
                    DestroyWindow(window);
                    return 0;
                }
                case IDCANCEL:
                    state->accepted = false;
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            if (state != nullptr) {
                state->accepted = false;
            }
            DestroyWindow(window);
            return 0;
        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool ensurePromptWindowClass(std::string* errorMessage) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = promptWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kPromptWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    ATOM registration = RegisterClassW(&windowClass);
    if (registration == 0) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return writeError("Could not register the Uranium GUI prompt window.", errorMessage);
        }
    }

    return true;
}

bool fileDialogPath(IShellItem* item, std::string* result, std::string* errorMessage) {
    if (item == nullptr) {
        return writeError("Windows did not return a file selection.", errorMessage);
    }

    PWSTR path = nullptr;
    HRESULT status = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(status)) {
        return writeError(hresultMessage("Windows file dialog result", status), errorMessage);
    }

    std::wstring widePath = path == nullptr ? L"" : path;
    CoTaskMemFree(path);
    return wideToUtf8(widePath, result, errorMessage);
}

template <typename DialogT>
bool finalizeDialogResult(DialogT* dialog, std::string* result,
                          bool* accepted, std::string* errorMessage) {
    if (accepted != nullptr) {
        *accepted = false;
    }

    HRESULT showResult = dialog->Show(nullptr);
    if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        if (accepted != nullptr) {
            *accepted = false;
        }
        return true;
    }

    if (FAILED(showResult)) {
        return writeError(hresultMessage("Windows dialog", showResult), errorMessage);
    }

    IShellItem* item = nullptr;
    HRESULT resultStatus = dialog->GetResult(&item);
    if (FAILED(resultStatus)) {
        return writeError(hresultMessage("Windows dialog result", resultStatus), errorMessage);
    }

    bool converted = fileDialogPath(item, result, errorMessage);
    item->Release();

    if (!converted) {
        return false;
    }

    if (accepted != nullptr) {
        *accepted = true;
    }
    return true;
}

} // namespace

bool guiShowMessage(const std::string& title, const std::string& message,
                    GuiMessageKind kind, std::string* errorMessage) {
    std::wstring wideTitle;
    std::wstring wideMessage;
    if (!utf8ToWide(title, &wideTitle, errorMessage) ||
        !utf8ToWide(message, &wideMessage, errorMessage)) {
        return false;
    }

    int result = MessageBoxW(
        nullptr, wideMessage.c_str(), wideTitle.c_str(),
        MB_OK | MB_TASKMODAL | messageBoxIconFlags(kind));
    if (result == 0) {
        return writeError("Windows could not show the message dialog.", errorMessage);
    }

    return true;
}

bool guiAskYesNo(const std::string& title, const std::string& message,
                 GuiMessageKind kind, bool* result, std::string* errorMessage) {
    std::wstring wideTitle;
    std::wstring wideMessage;
    if (!utf8ToWide(title, &wideTitle, errorMessage) ||
        !utf8ToWide(message, &wideMessage, errorMessage)) {
        return false;
    }

    int answer = MessageBoxW(
        nullptr, wideMessage.c_str(), wideTitle.c_str(),
        MB_YESNO | MB_TASKMODAL | messageBoxIconFlags(kind));
    if (answer == 0) {
        return writeError("Windows could not show the confirmation dialog.", errorMessage);
    }

    if (result != nullptr) {
        *result = answer == IDYES;
    }
    return true;
}

bool guiAskOkCancel(const std::string& title, const std::string& message,
                    GuiMessageKind kind, bool* result, std::string* errorMessage) {
    std::wstring wideTitle;
    std::wstring wideMessage;
    if (!utf8ToWide(title, &wideTitle, errorMessage) ||
        !utf8ToWide(message, &wideMessage, errorMessage)) {
        return false;
    }

    int answer = MessageBoxW(
        nullptr, wideMessage.c_str(), wideTitle.c_str(),
        MB_OKCANCEL | MB_TASKMODAL | messageBoxIconFlags(kind));
    if (answer == 0) {
        return writeError("Windows could not show the OK/Cancel dialog.", errorMessage);
    }

    if (result != nullptr) {
        *result = answer == IDOK;
    }
    return true;
}

bool guiPromptText(const std::string& title, const std::string& message,
                   const std::string& defaultValue, std::string* result,
                   bool* accepted, std::string* errorMessage) {
    std::wstring wideTitle;
    std::wstring wideMessage;
    std::wstring wideDefault;
    if (!utf8ToWide(title, &wideTitle, errorMessage) ||
        !utf8ToWide(message, &wideMessage, errorMessage) ||
        !utf8ToWide(defaultValue, &wideDefault, errorMessage)) {
        return false;
    }

    if (!ensurePromptWindowClass(errorMessage)) {
        return false;
    }

    PromptDialogState state;
    state.title = wideTitle;
    state.message = wideMessage;
    state.value = wideDefault;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kPromptWindowClass,
        state.title.c_str(),
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 176,
        nullptr, nullptr, instance, &state);

    if (window == nullptr) {
        return writeError("Windows could not create the Uranium prompt window.", errorMessage);
    }

    RECT rect{};
    GetWindowRect(window, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(
        window, nullptr,
        (screenWidth - width) / 2, (screenHeight - height) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG messageData{};
    while (IsWindow(window)) {
        BOOL messageResult = GetMessageW(&messageData, nullptr, 0, 0);
        if (messageResult <= 0) {
            break;
        }

        if (!IsDialogMessageW(window, &messageData)) {
            TranslateMessage(&messageData);
            DispatchMessageW(&messageData);
        }
    }

    if (accepted != nullptr) {
        *accepted = state.accepted;
    }

    if (state.accepted && result != nullptr) {
        return wideToUtf8(state.value, result, errorMessage);
    }

    if (result != nullptr) {
        result->clear();
    }
    return true;
}

bool guiOpenFileDialog(const std::string& title, std::string* result,
                       bool* accepted, std::string* errorMessage) {
    ScopedComInitializer com;
    if (!com.initialize(errorMessage)) {
        return false;
    }

    std::wstring wideTitle;
    if (!utf8ToWide(title, &wideTitle, errorMessage)) {
        return false;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT creation = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(creation)) {
        return writeError(hresultMessage("Windows open file dialog", creation), errorMessage);
    }

    dialog->SetTitle(wideTitle.c_str());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    bool okay = finalizeDialogResult(dialog, result, accepted, errorMessage);
    dialog->Release();
    return okay;
}

bool guiSaveFileDialog(const std::string& title, const std::string& defaultName,
                       std::string* result, bool* accepted, std::string* errorMessage) {
    ScopedComInitializer com;
    if (!com.initialize(errorMessage)) {
        return false;
    }

    std::wstring wideTitle;
    std::wstring wideDefaultName;
    if (!utf8ToWide(title, &wideTitle, errorMessage) ||
        !utf8ToWide(defaultName, &wideDefaultName, errorMessage)) {
        return false;
    }

    IFileSaveDialog* dialog = nullptr;
    HRESULT creation = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(creation)) {
        return writeError(hresultMessage("Windows save file dialog", creation), errorMessage);
    }

    dialog->SetTitle(wideTitle.c_str());
    dialog->SetFileName(wideDefaultName.c_str());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

    bool okay = finalizeDialogResult(dialog, result, accepted, errorMessage);
    dialog->Release();
    return okay;
}

bool guiPickFolderDialog(const std::string& title, std::string* result,
                         bool* accepted, std::string* errorMessage) {
    ScopedComInitializer com;
    if (!com.initialize(errorMessage)) {
        return false;
    }

    std::wstring wideTitle;
    if (!utf8ToWide(title, &wideTitle, errorMessage)) {
        return false;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT creation = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(creation)) {
        return writeError(hresultMessage("Windows folder picker", creation), errorMessage);
    }

    dialog->SetTitle(wideTitle.c_str());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    bool okay = finalizeDialogResult(dialog, result, accepted, errorMessage);
    dialog->Release();
    return okay;
}

bool guiGetScreenWidth(int* width, std::string* errorMessage) {
    (void)errorMessage;
    if (width == nullptr) {
        return false;
    }

    *width = GetSystemMetrics(SM_CXSCREEN);
    return true;
}

bool guiGetScreenHeight(int* height, std::string* errorMessage) {
    (void)errorMessage;
    if (height == nullptr) {
        return false;
    }

    *height = GetSystemMetrics(SM_CYSCREEN);
    return true;
}

bool guiCreateWindow(const std::string& title, int width, int height,
                     int* windowId, std::string* errorMessage) {
    if (windowId == nullptr) {
        return writeError("Internal GUI window error.", errorMessage);
    }

    if (!ensurePositiveSize(width, height, "guiCreateWindow", errorMessage) ||
        !ensureCommonControls(errorMessage) ||
        !ensureGuiWindowClass(errorMessage)) {
        return false;
    }

    std::wstring wideTitle;
    if (!utf8ToWide(title, &wideTitle, errorMessage)) {
        return false;
    }

    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    GuiRuntimeState& runtime = guiRuntime();
    int nextId = runtime.nextWindowId++;
    HINSTANCE instance = GetModuleHandleW(nullptr);

    HWND handle = CreateWindowExW(
        0, kGuiWindowClass, wideTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance,
        reinterpret_cast<LPVOID>(static_cast<std::intptr_t>(nextId)));

    if (handle == nullptr) {
        return writeError("Windows could not create the GUI window.", errorMessage);
    }

    GuiWindowState state{};
    state.id = nextId;
    state.handle = handle;
    runtime.windowsById[nextId] = state;
    runtime.windowIdsByHandle[handle] = nextId;
    *windowId = nextId;
    return true;
}

bool guiShowWindow(int windowId, std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    ShowWindow(state->handle, SW_SHOW);
    UpdateWindow(state->handle);
    return true;
}

bool guiHideWindow(int windowId, std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    ShowWindow(state->handle, SW_HIDE);
    return true;
}

bool guiCloseWindow(int windowId, std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    if (!DestroyWindow(state->handle)) {
        return writeError("Windows could not close the GUI window.", errorMessage);
    }

    return true;
}

bool guiCenterWindow(int windowId, std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    RECT rect{};
    if (!GetWindowRect(state->handle, &rect)) {
        return writeError("Windows could not measure the GUI window.", errorMessage);
    }

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (!SetWindowPos(
            state->handle, nullptr,
            (screenWidth - width) / 2, (screenHeight - height) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return writeError("Windows could not center the GUI window.", errorMessage);
    }

    return true;
}

bool guiSetWindowTitle(int windowId, const std::string& title,
                       std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideTitle;
    if (!utf8ToWide(title, &wideTitle, errorMessage)) {
        return false;
    }

    if (!SetWindowTextW(state->handle, wideTitle.c_str())) {
        return writeError("Windows could not change the window title.", errorMessage);
    }

    return true;
}

bool guiSetWindowSize(int windowId, int width, int height,
                      std::string* errorMessage) {
    GuiWindowState* state = getWindowState(windowId);
    if (state == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    if (!ensurePositiveSize(width, height, "guiSetWindowSize", errorMessage)) {
        return false;
    }

    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(state->handle, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(state->handle, GWL_EXSTYLE));
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    if (!SetWindowPos(
            state->handle, nullptr, 0, 0,
            rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return writeError("Windows could not resize the GUI window.", errorMessage);
    }

    return true;
}

bool guiAddLabel(int windowId, const std::string& text,
                 int x, int y, int width, int height,
                 int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    return createControl(
        window->handle, GuiControlKind::Label,
        L"STATIC", wideText,
        SS_LEFT, 0,
        x, y, width, height, controlId, errorMessage);
}

bool guiAddButton(int windowId, const std::string& text,
                  int x, int y, int width, int height,
                  int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    return createControl(
        window->handle, GuiControlKind::Button,
        L"BUTTON", wideText,
        WS_TABSTOP | BS_PUSHBUTTON, 0,
        x, y, width, height, controlId, errorMessage);
}

bool guiAddInput(int windowId, const std::string& text,
                 int x, int y, int width, int height,
                 int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    return createControl(
        window->handle, GuiControlKind::Input,
        L"EDIT", wideText,
        WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
        x, y, width, height, controlId, errorMessage);
}

bool guiAddTextArea(int windowId, const std::string& text,
                    int x, int y, int width, int height,
                    int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    return createControl(
        window->handle, GuiControlKind::TextArea,
        L"EDIT", wideText,
        WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
        WS_EX_CLIENTEDGE,
        x, y, width, height, controlId, errorMessage);
}

bool guiAddCheckbox(int windowId, const std::string& text, bool checked,
                    int x, int y, int width, int height,
                    int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    if (!createControl(
            window->handle, GuiControlKind::Checkbox,
            L"BUTTON", wideText,
            WS_TABSTOP | BS_AUTOCHECKBOX, 0,
            x, y, width, height, controlId, errorMessage)) {
        return false;
    }

    GuiControlState* control = getControlState(*controlId);
    if (control != nullptr) {
        SendMessageW(control->handle, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    return true;
}

bool guiAddListBox(int windowId, int x, int y, int width, int height,
                   int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    return createControl(
        window->handle, GuiControlKind::ListBox,
        L"LISTBOX", L"",
        WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
        WS_EX_CLIENTEDGE,
        x, y, width, height, controlId, errorMessage);
}

bool guiAddProgressBar(int windowId, int x, int y, int width, int height,
                       int* controlId, std::string* errorMessage) {
    GuiWindowState* window = getWindowState(windowId);
    if (window == nullptr) {
        return writeError("Unknown GUI window handle.", errorMessage);
    }

    if (!ensureCommonControls(errorMessage)) {
        return false;
    }

    if (!createControl(
            window->handle, GuiControlKind::ProgressBar,
            PROGRESS_CLASSW, L"",
            0, 0,
            x, y, width, height, controlId, errorMessage)) {
        return false;
    }

    GuiControlState* control = getControlState(*controlId);
    if (control != nullptr) {
        SendMessageW(control->handle, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(control->handle, PBM_SETPOS, 0, 0);
    }

    return true;
}

bool guiSetText(int controlId, const std::string& text, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind == GuiControlKind::ListBox ||
        control->kind == GuiControlKind::ProgressBar) {
        return writeError("guiSetText is not supported for this control type.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    if (!SetWindowTextW(control->handle, wideText.c_str())) {
        return writeError("Windows could not update the control text.", errorMessage);
    }

    return true;
}

bool guiGetText(int controlId, std::string* result, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind == GuiControlKind::ProgressBar) {
        return writeError("guiGetText is not supported for progress bars.", errorMessage);
    }

    if (control->kind == GuiControlKind::ListBox) {
        int index = static_cast<int>(SendMessageW(control->handle, LB_GETCURSEL, 0, 0));
        if (index == LB_ERR) {
            if (result != nullptr) {
                result->clear();
            }
            return true;
        }

        if (result == nullptr) {
            return writeError("Internal GUI text error.", errorMessage);
        }

        *result = bestEffortControlText(*control, index);
        return true;
    }

    return getWindowTextUtf8(control->handle, result, errorMessage);
}

bool guiSetValue(int controlId, double value, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ProgressBar) {
        return writeError("guiSetValue is only supported for progress bars.", errorMessage);
    }

    int boundedValue = static_cast<int>(std::clamp(value, 0.0, 100.0));
    SendMessageW(control->handle, PBM_SETPOS, boundedValue, 0);
    return true;
}

bool guiGetValue(int controlId, double* value, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ProgressBar) {
        return writeError("guiGetValue is only supported for progress bars.", errorMessage);
    }

    if (value == nullptr) {
        return writeError("Internal GUI value error.", errorMessage);
    }

    *value = static_cast<double>(SendMessageW(control->handle, PBM_GETPOS, 0, 0));
    return true;
}

bool guiSetChecked(int controlId, bool checked, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::Checkbox) {
        return writeError("guiSetChecked is only supported for checkboxes.", errorMessage);
    }

    SendMessageW(control->handle, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return true;
}

bool guiGetChecked(int controlId, bool* checked, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::Checkbox) {
        return writeError("guiGetChecked is only supported for checkboxes.", errorMessage);
    }

    if (checked == nullptr) {
        return writeError("Internal GUI checkbox error.", errorMessage);
    }

    *checked = SendMessageW(control->handle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return true;
}

bool guiAddListItem(int controlId, const std::string& text, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ListBox) {
        return writeError("guiAddListItem is only supported for list boxes.", errorMessage);
    }

    std::wstring wideText;
    if (!utf8ToWide(text, &wideText, errorMessage)) {
        return false;
    }

    LRESULT result = SendMessageW(
        control->handle, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wideText.c_str()));
    if (result == LB_ERR || result == LB_ERRSPACE) {
        return writeError("Windows could not append the list item.", errorMessage);
    }

    return true;
}

bool guiClearList(int controlId, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ListBox) {
        return writeError("guiClearList is only supported for list boxes.", errorMessage);
    }

    SendMessageW(control->handle, LB_RESETCONTENT, 0, 0);
    return true;
}

bool guiGetSelectedIndex(int controlId, int* index, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ListBox) {
        return writeError("guiGetSelectedIndex is only supported for list boxes.", errorMessage);
    }

    if (index == nullptr) {
        return writeError("Internal GUI list error.", errorMessage);
    }

    LRESULT result = SendMessageW(control->handle, LB_GETCURSEL, 0, 0);
    *index = result == LB_ERR ? -1 : static_cast<int>(result);
    return true;
}

bool guiSetSelectedIndex(int controlId, int index, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (control->kind != GuiControlKind::ListBox) {
        return writeError("guiSetSelectedIndex is only supported for list boxes.", errorMessage);
    }

    if (index < -1) {
        return writeError("guiSetSelectedIndex expects -1 or a non-negative index.", errorMessage);
    }

    LRESULT result = SendMessageW(control->handle, LB_SETCURSEL, index, 0);
    if (index >= 0 && result == LB_ERR) {
        return writeError("Windows could not select that list item.", errorMessage);
    }

    return true;
}

bool guiSetBounds(int controlId, int x, int y, int width, int height,
                  std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    if (!ensurePositiveSize(width, height, "guiSetBounds", errorMessage)) {
        return false;
    }

    if (!SetWindowPos(
            control->handle, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE)) {
        return writeError("Windows could not resize or move the control.", errorMessage);
    }

    return true;
}

bool guiShowControl(int controlId, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    ShowWindow(control->handle, SW_SHOW);
    return true;
}

bool guiHideControl(int controlId, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    ShowWindow(control->handle, SW_HIDE);
    return true;
}

bool guiSetControlEnabled(int controlId, bool enabled, std::string* errorMessage) {
    GuiControlState* control = getControlState(controlId);
    if (control == nullptr) {
        return writeError("Unknown GUI control handle.", errorMessage);
    }

    EnableWindow(control->handle, enabled ? TRUE : FALSE);
    (void)errorMessage;
    return true;
}

bool guiPollEvent(bool* hasEvent, std::string* errorMessage) {
    if (!processPendingMessages(errorMessage)) {
        return false;
    }

    return consumeQueuedEvent(hasEvent, errorMessage);
}

bool guiWaitEvent(bool* hasEvent, std::string* errorMessage) {
    if (!processPendingMessages(errorMessage)) {
        return false;
    }

    if (!guiRuntime().queuedEvents.empty()) {
        return consumeQueuedEvent(hasEvent, errorMessage);
    }

    if (!hasOpenWindows()) {
        return consumeQueuedEvent(hasEvent, errorMessage);
    }

    for (;;) {
        MSG message{};
        BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) {
            return writeError("Windows GUI message loop failed.", errorMessage);
        }

        if (result == 0) {
            return consumeQueuedEvent(hasEvent, errorMessage);
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);

        if (!guiRuntime().queuedEvents.empty()) {
            return consumeQueuedEvent(hasEvent, errorMessage);
        }

        if (!hasOpenWindows()) {
            return consumeQueuedEvent(hasEvent, errorMessage);
        }
    }
}

bool guiGetEventType(std::string* eventType, std::string* errorMessage) {
    if (eventType == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *eventType = guiRuntime().currentEvent.type;
    return true;
}

bool guiGetEventWindow(int* windowId, std::string* errorMessage) {
    if (windowId == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *windowId = guiRuntime().currentEvent.windowId;
    return true;
}

bool guiGetEventControl(int* controlId, std::string* errorMessage) {
    if (controlId == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *controlId = guiRuntime().currentEvent.controlId;
    return true;
}

bool guiGetEventText(std::string* text, std::string* errorMessage) {
    if (text == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *text = guiRuntime().currentEvent.text;
    return true;
}

bool guiGetEventChecked(bool* checked, std::string* errorMessage) {
    if (checked == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *checked = guiRuntime().currentEvent.checked;
    return true;
}

bool guiGetEventIndex(int* index, std::string* errorMessage) {
    if (index == nullptr) {
        return writeError("Internal GUI event error.", errorMessage);
    }

    *index = guiRuntime().currentEvent.index;
    return true;
}

#else

namespace {

bool unsupported(std::string* errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = "The GUI library is currently only available on Windows.";
    }
    return false;
}

} // namespace

#define GUI_STUB(name, signature) bool name signature { return unsupported(errorMessage); }

GUI_STUB(guiShowMessage, (const std::string& title, const std::string& message,
                         GuiMessageKind kind, std::string* errorMessage))
GUI_STUB(guiAskYesNo, (const std::string& title, const std::string& message,
                      GuiMessageKind kind, bool* result, std::string* errorMessage))
GUI_STUB(guiAskOkCancel, (const std::string& title, const std::string& message,
                         GuiMessageKind kind, bool* result, std::string* errorMessage))
GUI_STUB(guiPromptText, (const std::string& title, const std::string& message,
                        const std::string& defaultValue, std::string* result,
                        bool* accepted, std::string* errorMessage))
GUI_STUB(guiOpenFileDialog, (const std::string& title, std::string* result,
                            bool* accepted, std::string* errorMessage))
GUI_STUB(guiSaveFileDialog, (const std::string& title, const std::string& defaultName,
                            std::string* result, bool* accepted,
                            std::string* errorMessage))
GUI_STUB(guiPickFolderDialog, (const std::string& title, std::string* result,
                              bool* accepted, std::string* errorMessage))
GUI_STUB(guiGetScreenWidth, (int* width, std::string* errorMessage))
GUI_STUB(guiGetScreenHeight, (int* height, std::string* errorMessage))
GUI_STUB(guiCreateWindow, (const std::string& title, int width, int height,
                          int* windowId, std::string* errorMessage))
GUI_STUB(guiShowWindow, (int windowId, std::string* errorMessage))
GUI_STUB(guiHideWindow, (int windowId, std::string* errorMessage))
GUI_STUB(guiCloseWindow, (int windowId, std::string* errorMessage))
GUI_STUB(guiCenterWindow, (int windowId, std::string* errorMessage))
GUI_STUB(guiSetWindowTitle, (int windowId, const std::string& title,
                            std::string* errorMessage))
GUI_STUB(guiSetWindowSize, (int windowId, int width, int height,
                           std::string* errorMessage))
GUI_STUB(guiAddLabel, (int windowId, const std::string& text,
                      int x, int y, int width, int height,
                      int* controlId, std::string* errorMessage))
GUI_STUB(guiAddButton, (int windowId, const std::string& text,
                       int x, int y, int width, int height,
                       int* controlId, std::string* errorMessage))
GUI_STUB(guiAddInput, (int windowId, const std::string& text,
                      int x, int y, int width, int height,
                      int* controlId, std::string* errorMessage))
GUI_STUB(guiAddTextArea, (int windowId, const std::string& text,
                         int x, int y, int width, int height,
                         int* controlId, std::string* errorMessage))
GUI_STUB(guiAddCheckbox, (int windowId, const std::string& text, bool checked,
                         int x, int y, int width, int height,
                         int* controlId, std::string* errorMessage))
GUI_STUB(guiAddListBox, (int windowId, int x, int y, int width, int height,
                        int* controlId, std::string* errorMessage))
GUI_STUB(guiAddProgressBar, (int windowId, int x, int y, int width, int height,
                            int* controlId, std::string* errorMessage))
GUI_STUB(guiSetText, (int controlId, const std::string& text, std::string* errorMessage))
GUI_STUB(guiGetText, (int controlId, std::string* result, std::string* errorMessage))
GUI_STUB(guiSetValue, (int controlId, double value, std::string* errorMessage))
GUI_STUB(guiGetValue, (int controlId, double* value, std::string* errorMessage))
GUI_STUB(guiSetChecked, (int controlId, bool checked, std::string* errorMessage))
GUI_STUB(guiGetChecked, (int controlId, bool* checked, std::string* errorMessage))
GUI_STUB(guiAddListItem, (int controlId, const std::string& text, std::string* errorMessage))
GUI_STUB(guiClearList, (int controlId, std::string* errorMessage))
GUI_STUB(guiGetSelectedIndex, (int controlId, int* index, std::string* errorMessage))
GUI_STUB(guiSetSelectedIndex, (int controlId, int index, std::string* errorMessage))
GUI_STUB(guiSetBounds, (int controlId, int x, int y, int width, int height,
                       std::string* errorMessage))
GUI_STUB(guiShowControl, (int controlId, std::string* errorMessage))
GUI_STUB(guiHideControl, (int controlId, std::string* errorMessage))
GUI_STUB(guiSetControlEnabled, (int controlId, bool enabled, std::string* errorMessage))
GUI_STUB(guiPollEvent, (bool* hasEvent, std::string* errorMessage))
GUI_STUB(guiWaitEvent, (bool* hasEvent, std::string* errorMessage))
GUI_STUB(guiGetEventType, (std::string* eventType, std::string* errorMessage))
GUI_STUB(guiGetEventWindow, (int* windowId, std::string* errorMessage))
GUI_STUB(guiGetEventControl, (int* controlId, std::string* errorMessage))
GUI_STUB(guiGetEventText, (std::string* text, std::string* errorMessage))
GUI_STUB(guiGetEventChecked, (bool* checked, std::string* errorMessage))
GUI_STUB(guiGetEventIndex, (int* index, std::string* errorMessage))

#undef GUI_STUB

#endif
