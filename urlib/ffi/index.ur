// True Dynamic Foreign Function Interface (FFI)

// Loads a native C/C++ library (.dll or .so)
fn load(path) {
    return ffiLoad(path)
}

// Dynamically calls a function inside a loaded library.
// lib: The library object returned by load()
// funcName: String name of the function to call
// returnType: String ("int", "long", "string", "bool", "void")
// args: Array of arguments to pass to the C function
fn call(lib, funcName, returnType, args) {
    return ffiCall(lib, funcName, returnType, args)
}

// Unloads the library from memory.
fn unload(lib) {
    return ffiUnload(lib)
}
