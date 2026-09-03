import ffi

printn("=== DYNAMIC FFI TEST ===")

// Load user32.dll on Windows to call MessageBoxA
let lib = ffi.load("user32.dll")
if (lib != nil) {
    printn("user32.dll loaded successfully!")
    
    // MessageBoxA signature: int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
    // Arguments: 0 (NULL), "Hello from Uranium JIT FFI!", "Success", 0 (MB_OK)
    printn("Calling MessageBoxA dynamically...")
    let result = ffi.call(lib, "MessageBoxA", "int", [0, "Hello from Uranium JIT FFI!", "Success", 0])
    
    printn("MessageBoxA returned: " + result)
    ffi.unload(lib)
} else {
    printn("Failed to load user32.dll")
}
