#ifndef uranium_gui_native_h
#define uranium_gui_native_h

#include <string>

enum class GuiMessageKind {
    Info,
    Warning,
    Error,
};

bool guiShowMessage(const std::string& title, const std::string& message,
                    GuiMessageKind kind, std::string* errorMessage);
bool guiAskYesNo(const std::string& title, const std::string& message,
                 GuiMessageKind kind, bool* result, std::string* errorMessage);
bool guiAskOkCancel(const std::string& title, const std::string& message,
                    GuiMessageKind kind, bool* result, std::string* errorMessage);
bool guiPromptText(const std::string& title, const std::string& message,
                   const std::string& defaultValue, std::string* result,
                   bool* accepted, std::string* errorMessage);
bool guiOpenFileDialog(const std::string& title, std::string* result,
                       bool* accepted, std::string* errorMessage);
bool guiSaveFileDialog(const std::string& title, const std::string& defaultName,
                       std::string* result, bool* accepted, std::string* errorMessage);
bool guiPickFolderDialog(const std::string& title, std::string* result,
                         bool* accepted, std::string* errorMessage);
bool guiGetScreenWidth(int* width, std::string* errorMessage);
bool guiGetScreenHeight(int* height, std::string* errorMessage);

bool guiCreateWindow(const std::string& title, int width, int height,
                     int* windowId, std::string* errorMessage);
bool guiShowWindow(int windowId, std::string* errorMessage);
bool guiHideWindow(int windowId, std::string* errorMessage);
bool guiCloseWindow(int windowId, std::string* errorMessage);
bool guiCenterWindow(int windowId, std::string* errorMessage);
bool guiSetWindowTitle(int windowId, const std::string& title,
                       std::string* errorMessage);
bool guiSetWindowSize(int windowId, int width, int height,
                      std::string* errorMessage);

bool guiAddLabel(int windowId, const std::string& text,
                 int x, int y, int width, int height,
                 int* controlId, std::string* errorMessage);
bool guiAddButton(int windowId, const std::string& text,
                  int x, int y, int width, int height,
                  int* controlId, std::string* errorMessage);
bool guiAddInput(int windowId, const std::string& text,
                 int x, int y, int width, int height,
                 int* controlId, std::string* errorMessage);
bool guiAddTextArea(int windowId, const std::string& text,
                    int x, int y, int width, int height,
                    int* controlId, std::string* errorMessage);
bool guiAddCheckbox(int windowId, const std::string& text, bool checked,
                    int x, int y, int width, int height,
                    int* controlId, std::string* errorMessage);
bool guiAddListBox(int windowId, int x, int y, int width, int height,
                   int* controlId, std::string* errorMessage);
bool guiAddProgressBar(int windowId, int x, int y, int width, int height,
                       int* controlId, std::string* errorMessage);

bool guiSetText(int controlId, const std::string& text, std::string* errorMessage);
bool guiGetText(int controlId, std::string* result, std::string* errorMessage);
bool guiSetValue(int controlId, double value, std::string* errorMessage);
bool guiGetValue(int controlId, double* value, std::string* errorMessage);
bool guiSetChecked(int controlId, bool checked, std::string* errorMessage);
bool guiGetChecked(int controlId, bool* checked, std::string* errorMessage);
bool guiAddListItem(int controlId, const std::string& text, std::string* errorMessage);
bool guiClearList(int controlId, std::string* errorMessage);
bool guiGetSelectedIndex(int controlId, int* index, std::string* errorMessage);
bool guiSetSelectedIndex(int controlId, int index, std::string* errorMessage);
bool guiSetBounds(int controlId, int x, int y, int width, int height,
                  std::string* errorMessage);
bool guiShowControl(int controlId, std::string* errorMessage);
bool guiHideControl(int controlId, std::string* errorMessage);
bool guiSetControlEnabled(int controlId, bool enabled, std::string* errorMessage);

bool guiPollEvent(bool* hasEvent, std::string* errorMessage);
bool guiWaitEvent(bool* hasEvent, std::string* errorMessage);
bool guiGetEventType(std::string* eventType, std::string* errorMessage);
bool guiGetEventWindow(int* windowId, std::string* errorMessage);
bool guiGetEventControl(int* controlId, std::string* errorMessage);
bool guiGetEventText(std::string* text, std::string* errorMessage);
bool guiGetEventChecked(bool* checked, std::string* errorMessage);
bool guiGetEventIndex(int* index, std::string* errorMessage);

#endif
