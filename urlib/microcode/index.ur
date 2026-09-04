import fs as fs

// Supported Board FQBNs (Fully Qualified Board Names)
let BOARD_UNO = "arduino:avr:uno"
let BOARD_NANO = "arduino:avr:nano"
let BOARD_MEGA = "arduino:avr:mega"
let BOARD_ESP32 = "esp32:esp32:esp32"
let BOARD_ESP8266 = "esp8266:esp8266:generic"
let BOARD_RP2040 = "rp2040:rp2040:rpipico"
let BOARD_STM32 = "STMicroelectronics:stm32:GenF1"

// Hardware Pin Constants
let INPUT = 0
let OUTPUT = 1
let INPUT_PULLUP = 2
let LOW = 0
let HIGH = 1

// List all connected serial/COM ports
fn listPorts() {
    return microcodeListPorts()
}

// Low-level compilation and flashing helper
fn compileAndFlash(sketchPath, fqbn, port, customCli = "") {
    return microcodeCompileAndFlash(sketchPath, fqbn, port, customCli)
}

// Direct interactive serial device connection
class Device() {
    fn init(port, baudRate = 115200) {
        this.port = port
        this.baudRate = baudRate
        this.handle = -1
        this.isOpen = false
    }

    fn open() {
        this.handle = microcodeOpen(this.port, this.baudRate)
        if (this.handle != -1) {
            this.isOpen = true
            return true
        }
        return false
    }

    fn close() {
        if (this.isOpen) {
            microcodeClose(this.handle)
            this.isOpen = false
            this.handle = -1
            return true
        }
        return false
    }

    fn write(text) {
        if (!this.isOpen) {
            return 0
        }
        return microcodeWrite(this.handle, text)
    }

    fn println(text) {
        return this.write(text + "\r\n")
    }

    fn read(maxBytes = 1024, timeoutMs = 100) {
        if (!this.isOpen) {
            return ""
        }
        return microcodeRead(this.handle, maxBytes, timeoutMs)
    }

    fn readLine(timeoutMs = 1000) {
        if (!this.isOpen) {
            return ""
        }
        return microcodeReadLine(this.handle, timeoutMs)
    }

    fn execute(command, timeoutMs = 2000) {
        if (!this.isOpen) {
            return ""
        }
        return microcodeExecute(this.handle, command, timeoutMs)
    }

    fn resetEsp32() {
        if (!this.isOpen) {
            return false
        }
        return microcodeResetEsp32(this.handle)
    }

    fn setDTR(state) {
        if (!this.isOpen) {
            return false
        }
        return microcodeSetDTR(this.handle, state)
    }

    fn setRTS(state) {
        if (!this.isOpen) {
            return false
        }
        return microcodeSetRTS(this.handle, state)
    }
}

// High-level Sketch builder & C++ code generator for Arduino / ESP32
class Sketch() {
    fn init(name = "UraniumSketch", board = "arduino:avr:uno") {
        this.name = name
        this.board = board
        this.includes = ["#include <Arduino.h>"]
        this.globals = []
        this.setupCode = []
        this.loopCode = []
        this.customFunctions = []
    }

    fn addInclude(header) {
        this.includes = this.includes + [header]
        return this
    }

    fn addGlobal(decl) {
        this.globals = this.globals + [decl]
        return this
    }

    fn addFunction(code) {
        this.customFunctions = this.customFunctions + [code]
        return this
    }

    // Hardware GPIO methods for setup & loop
    fn pinMode(pin, mode) {
        let mStr = "INPUT"
        if (mode == 1) {
            mStr = "OUTPUT"
        }
        if (mode == 2) {
            mStr = "INPUT_PULLUP"
        }
        this.setupCode = this.setupCode + ["  pinMode(" + str(pin) + ", " + mStr + ");"]
        return this
    }

    fn serialBegin(baud = 115200) {
        this.setupCode = this.setupCode + ["  Serial.begin(" + str(baud) + ");"]
        return this
    }

    fn setupRaw(code) {
        this.setupCode = this.setupCode + ["  " + code]
        return this
    }

    fn loopRaw(code) {
        this.loopCode = this.loopCode + ["  " + code]
        return this
    }

    fn digitalWrite(pin, value) {
        let vStr = "LOW"
        if (value == 1 or value == true) {
            vStr = "HIGH"
        }
        this.loopCode = this.loopCode + ["  digitalWrite(" + str(pin) + ", " + vStr + ");"]
        return this
    }

    fn delay(ms) {
        this.loopCode = this.loopCode + ["  delay(" + str(ms) + ");"]
        return this
    }

    fn delayMicros(us) {
        this.loopCode = this.loopCode + ["  delayMicroseconds(" + str(us) + ");"]
        return this
    }

    fn serialPrint(text) {
        this.loopCode = this.loopCode + [R"(  Serial.print(")" + text + R"(");)"]
        return this
    }

    fn serialPrintln(text) {
        this.loopCode = this.loopCode + [R"(  Serial.println(")" + text + R"(");)"]
        return this
    }

    // ESP32 WiFi setup helper
    fn enableWifi(ssid, password) {
        this.addInclude("#include <WiFi.h>")
        this.setupCode = this.setupCode + [
            R"(  WiFi.begin(")" + ssid + R"(", ")" + password + R"(");)",
            R"(  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); })",
            R"(  Serial.println(""); Serial.println(WiFi.localIP());)"
        ]
        return this
    }

    // Transpile & generate full Arduino C++ source
    fn toCpp() {
        let out = "// ========================================================\n"
        out = out + "// Generated by Uranium Microcode Engine\n"
        out = out + "// Target Board: " + this.board + "\n"
        out = out + "// Sketch Name : " + this.name + "\n"
        out = out + "// ========================================================\n\n"

        // 1. Includes
        let i = 0
        while (i < len(this.includes)) {
            out = out + this.includes[i] + "\n"
            i = i + 1
        }
        out = out + "\n"

        // 2. Globals
        i = 0
        while (i < len(this.globals)) {
            out = out + this.globals[i] + "\n"
            i = i + 1
        }
        if (len(this.globals) > 0) {
            out = out + "\n"
        }

        // 3. Custom Functions
        i = 0
        while (i < len(this.customFunctions)) {
            out = out + this.customFunctions[i] + "\n\n"
            i = i + 1
        }

        // 4. setup()
        out = out + "void setup() {\n"
        i = 0
        while (i < len(this.setupCode)) {
            out = out + this.setupCode[i] + "\n"
            i = i + 1
        }
        out = out + "}\n\n"

        // 5. loop()
        out = out + "void loop() {\n"
        i = 0
        while (i < len(this.loopCode)) {
            out = out + this.loopCode[i] + "\n"
            i = i + 1
        }
        out = out + "}\n"

        return out
    }

    // Save sketch as an Arduino project directory (.ino + .cpp)
    fn save(directoryPath) {
        if (!fs.exists(directoryPath)) {
            fs.createDirs(directoryPath)
        }
        let cppContent = this.toCpp()
        let inoPath = fs.join(directoryPath, this.name + ".ino")
        fs.writeText(inoPath, cppContent)
        return inoPath
    }

    // Compile and upload to microcontroller
    fn flash(port, directoryPath = "", customCli = "") {
        let targetDir = directoryPath
        if (targetDir == "") {
            targetDir = fs.join(fs.cwd(), this.name)
        }
        this.save(targetDir)
        return compileAndFlash(targetDir, this.board, port, customCli)
    }
}

// Convenience factory function: create a new sketch
fn createSketch(name = "UraniumSketch", board = BOARD_UNO) {
    let s = Sketch()
    s.name = name
    s.board = board
    return s
}

// Convenience function to open a serial device
fn open(port, baudRate = 115200) {
    let dev = Device()
    dev.init(port, baudRate)
    if (dev.open()) {
        return dev
    }
    return nil
}
