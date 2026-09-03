import system

// Test F-String
let name = "Dünya"
let greeting = f"Merhaba, {name}!"
print(greeting)

let a = 10
let b = 20
print(f"Toplam: {a + b}")
print(f"Nested: {f"inner {a}"}")

// Test R-String
let raw = R"(Bu bir "raw" \n string'dir, escape edilmez.)"
print(raw)

// Test Type Properties
print(a.is_int)       // 0
print(greeting.is_int) // 1
print(greeting.is_string) // 0
print(raw.is_string)  // 0
print(a.is_string)    // 1

// Test system library
print("System cmd testi:")
let result = system.cmd("echo Hello from shell")
print(result.exitCode)
