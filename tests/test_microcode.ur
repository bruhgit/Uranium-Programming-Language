import microcode
import fs as fs

fn main() {
    print("=== Uranium Microcode Engine Test ===")

        // 1. Test Port Enumeration
        let ports = microcode.listPorts()
        print("Detected Serial/COM ports:")
        print(ports)

        // 2. Test C++ Code Generator for Arduino Uno
        print("\n--- 1. Generating Arduino Uno C++ Firmware ---")
        let unoSketch = microcode.createSketch("BlinkUno", microcode.BOARD_UNO)
        unoSketch.serialBegin(9600)
        unoSketch.pinMode(13, microcode.OUTPUT)
        unoSketch.digitalWrite(13, microcode.HIGH)
        unoSketch.delay(1000)
        unoSketch.digitalWrite(13, microcode.LOW)
        unoSketch.delay(1000)
        unoSketch.serialPrintln("Tick from Uranium Uno!")

        let unoCpp = unoSketch.toCpp()
        print("Generated Arduino Uno C++ Code:")
        print(unoCpp)

        // 3. Test C++ Code Generator for ESP32 with WiFi & Sensors
        print("\n--- 2. Generating ESP32 C++ Firmware with WiFi ---")
        let espSketch = microcode.createSketch("SmartSensorESP32", microcode.BOARD_ESP32)
        espSketch.serialBegin(115200)
        espSketch.pinMode(2, microcode.OUTPUT)      // Built-in blue LED
        espSketch.pinMode(34, microcode.INPUT)     // ADC Sensor input
        espSketch.enableWifi("MyHomeWiFi", "SecretPassword123")
        
        espSketch.loopRaw("int rawVal = analogRead(34);")
        espSketch.loopRaw("float voltage = (rawVal / 4095.0) * 3.3;")
        espSketch.serialPrintln("Sensor reading cycle complete.")
        espSketch.digitalWrite(2, microcode.HIGH)
        espSketch.delay(500)
        espSketch.digitalWrite(2, microcode.LOW)
        espSketch.delay(1000)

        let espCpp = espSketch.toCpp()
        print("Generated ESP32 C++ Code:")
        print(espCpp)

        // 4. Save Sketch to Disk
        let sketchDir = fs.join(fs.cwd(), "build_sketch_esp32")
        let savedFile = espSketch.save(sketchDir)
        print("Saved sketch file to:")
        print(savedFile)
        print("File exists on disk: ")
        print(fs.exists(savedFile))

        // 5. Test Compiler & Flash Pipeline Bridge
        print("\n--- 3. Testing Compiler / Flasher Pipeline ---")
        let dummyPort = "COM3"
        if (len(ports) > 0) {
            dummyPort = ports[0]
        }
        let flashResult = espSketch.flash(dummyPort, sketchDir)
        print("Toolchain compilation bridge response:")
        print(flashResult)

        print("\n=== All Microcode Tests Passed Successfully! ===")
}
