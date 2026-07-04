const EVENT_NONE = "none"
const EVENT_CLOSE = "close"
const EVENT_CLICK = "click"
const EVENT_CHANGE = "change"
const EVENT_TOGGLE = "toggle"
const EVENT_SELECT = "select"

fn info(title, message) {
return guiInfo(title, message)
}

fn warn(title, message) {
return guiWarn(title, message)
}

fn error(title, message) {
return guiError(title, message)
}

fn confirm(title, message) {
return guiConfirm(title, message)
}

fn okCancel(title, message) {
return guiConfirmCancel(title, message)
}

fn prompt(title, message, defaultValue) {
return guiPrompt(title, message, defaultValue)
}

fn openFile(title) {
return guiOpenFile(title)
}

fn saveFile(title, defaultName) {
return guiSaveFile(title, defaultName)
}

fn pickFolder(title) {
return guiPickFolder(title)
}

fn screenWidth() {
return guiScreenWidth()
}

fn screenHeight() {
return guiScreenHeight()
}

fn screenSize() {
return str(guiScreenWidth()) + "x" + str(guiScreenHeight())
}

fn createWindow(title, width, height) {
return guiCreateWindow(title, width, height)
}

fn showWindow(windowId) {
return guiShowWindow(windowId)
}

fn hideWindow(windowId) {
return guiHideWindow(windowId)
}

fn closeWindow(windowId) {
return guiCloseWindow(windowId)
}

fn centerWindow(windowId) {
return guiCenterWindow(windowId)
}

fn setWindowTitle(windowId, title) {
return guiSetWindowTitle(windowId, title)
}

fn setWindowSize(windowId, width, height) {
return guiSetWindowSize(windowId, width, height)
}

fn label(windowId, text, x, y, width, height) {
return guiAddLabel(windowId, text, x, y, width, height)
}

fn button(windowId, text, x, y, width, height) {
return guiAddButton(windowId, text, x, y, width, height)
}

fn input(windowId, text, x, y, width, height) {
return guiAddInput(windowId, text, x, y, width, height)
}

fn textArea(windowId, text, x, y, width, height) {
return guiAddTextArea(windowId, text, x, y, width, height)
}

fn checkbox(windowId, text, checked, x, y, width, height) {
return guiAddCheckbox(windowId, text, checked, x, y, width, height)
}

fn listBox(windowId, x, y, width, height) {
return guiAddListBox(windowId, x, y, width, height)
}

fn progressBar(windowId, x, y, width, height) {
return guiAddProgressBar(windowId, x, y, width, height)
}

fn setText(controlId, text) {
return guiSetText(controlId, text)
}

fn getText(controlId) {
return guiGetText(controlId)
}

fn setValue(controlId, value) {
return guiSetValue(controlId, value)
}

fn getValue(controlId) {
return guiGetValue(controlId)
}

fn setChecked(controlId, checked) {
return guiSetChecked(controlId, checked)
}

fn isChecked(controlId) {
return guiGetChecked(controlId)
}

fn addItem(controlId, text) {
return guiAddListItem(controlId, text)
}

fn clearItems(controlId) {
return guiClearList(controlId)
}

fn selectedIndex(controlId) {
return guiGetSelectedIndex(controlId)
}

fn selectIndex(controlId, index) {
return guiSetSelectedIndex(controlId, index)
}

fn setBounds(controlId, x, y, width, height) {
return guiSetBounds(controlId, x, y, width, height)
}

fn showControl(controlId) {
return guiShowControl(controlId)
}

fn hideControl(controlId) {
return guiHideControl(controlId)
}

fn enableControl(controlId) {
return guiEnableControl(controlId)
}

fn disableControl(controlId) {
return guiDisableControl(controlId)
}

fn pollEvent() {
return guiPollEvent()
}

fn waitEvent() {
return guiWaitEvent()
}

fn eventType() {
return guiEventType()
}

fn eventWindow() {
return guiEventWindow()
}

fn eventControl() {
return guiEventControl()
}

fn eventText() {
return guiEventText()
}

fn eventChecked() {
return guiEventChecked()
}

fn eventIndex() {
return guiEventIndex()
}
