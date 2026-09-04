import microcode
import fs as fs

// ==============================================================================
// Uranium Microcode - ESP32 & Arduino Embedded Firmware Generator & Flasher
// ==============================================================================

fn main() {
    print("=================================================================")
    print(" ⚡ URANIUM MICROCODE: ESP32 & ARDUINO EMBEDDED ENGINE ⚡")
    print("=================================================================")

    // 1. Detect attached microcontrollers
    let ports = microcode.listPorts()
    print("Connected Hardware Serial Ports:")
    if (len(ports) == 0) {
        print("  [No hardware serial devices currently connected]")
    } else {
        let i = 0
        while (i < len(ports)) {
            print("  Port: " + ports[i])
            i = i + 1
        }
    }

    // 2. Define an ESP32 IoT Temperature & LED Controller in Uranium
    print("\n[1] Defining ESP32 Firmware in Uranium...")
    let esp = microcode.createSketch("Esp32IotBlink", microcode.BOARD_ESP32)

    // Hardware Configuration (Setup)
    esp.serialBegin(115200)
    esp.pinMode(2, microcode.OUTPUT)   // ESP32 Onboard LED
    esp.pinMode(4, microcode.INPUT)    // Button / Digital Sensor
    esp.enableWifi("HomeNetwork", "SecurePassword123")

    // Main Execution Cycle (Loop)
    esp.loopRaw("int buttonState = digitalRead(4);")
    esp.loopRaw("if (buttonState == HIGH) {")
    esp.loopRaw("    digitalWrite(2, HIGH);")
    esp.loopRaw(R"(    Serial.println("Alert: Trigger detected!");)")
    esp.loopRaw("} else {")
    esp.loopRaw("    digitalWrite(2, LOW);")
    esp.loopRaw("}")
    esp.delay(200)

    // 3. Transpile Uranium definition to Standard C++ Arduino Firmware
    print("[2] Transpiling Uranium code to C++ (Arduino/ESP32)...")
    let cppCode = esp.toCpp()
    print("\n--- Transpiled C++ Output ---")
    print(cppCode)

    // 4. Save the sketch
    let outDir = fs.join(fs.cwd(), "esp32_sketch")
    let sketchFile = esp.save(outDir)
    print("Saved sketch to: " + sketchFile)

    // 5. Automatic Flash via Arduino CLI
    let portToFlash = "COM3"
    if (len(ports) > 0) {
        portToFlash = ports[0]
    }
    print("\n[3] Triggering Compiler & Flasher for " + portToFlash + "...")
    let res = esp.flash(portToFlash, outDir)
    print("Flasher Result:")
    print(res)

    print("\n⚡ Uranium Microcode Pipeline Complete! ⚡")
}
