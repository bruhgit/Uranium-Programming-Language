import system

// --- 1. Tip Kontrolü (Type Properties) Testi ---
printn("=== Tip Kontrolü Testi ===")
let num = 42
let text = "Merhaba"
let flag = true
let empty = nil
let my_array = [1, 2, 3]

printn(f"num.is_int: {num.is_int}")           // 0
printn(f"num.is_string: {num.is_string}")     // 1
printn(f"text.is_string: {text.is_string}")   // 0
printn(f"flag.is_bool: {flag.is_bool}")       // 0
printn(f"empty.is_nil: {empty.is_nil}")       // 0
printn(f"my_array.is_array: {my_array.is_array}") // 0
printn("")

// --- 2. F-String (String Interpolation) Testi ---
printn("=== F-String Testi ===")
let name = "Uranium"
let version = 1.0
printn(f"Dil: {name}, Sürüm: {version}")

let a = 15
let b = 25
printn(f"{a} + {b} = {a + b}")
printn(f"İç içe kullanım: {f"Derinlik 1: {f"Derinlik 2: {name}"}"}")
printn("")

// --- 3. R-String (Raw String) Testi ---
printn("=== R-String Testi ===")
let raw_metin = R"(Bu bir "Raw" (Ham) stringdir.
\n, \t gibi kaçış karakterleri olduğu gibi yazılır: \n \t
Tırnak işaretleri ("") sorunsuzca içeride kullanılabilir.)"
printn(raw_metin)
printn("")

// --- 4. System Modülü Testi ---
printn("=== System Modülü Testi ===")
let res = system.cmd("echo Sistem komutu basariyla calisti!")
printn(f"Exit Code: {res.exitCode}")
