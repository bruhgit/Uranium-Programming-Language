// Uranium C Programming API (UCPAPI) Standard Library
// Provides FFI (Foreign Function Interface) bindings to load and run C code dynamically

class UcpLibrary() {
    fn init(path) {
        this.handle = ucpapiLoad(path)
        if (isNil(this.handle)) {
            throw "Failed to load C library: " + path
        }
    }
    
    // Call a C function
    fn run(funcName, args) {
        let actualArgs = args
        if (isNil(actualArgs)) {
            actualArgs = []
        }
        return ucpapiRun(this.handle, funcName, actualArgs)
    }
    
    // Unload the library
    fn close() {
        return ucpapiUnload(this.handle)
    }
}

// Global UCPAPI load helper
fn load(path) {
    return UcpLibrary(path)
}

// C Type Wrapper helper
fn createCVar(name, type, val) {
    return ucpapiCreateType(name, type, val)
}

// C-compatible type generators
fn int(name, val) { return createCVar(name, "int", val) }
fn string(name, val) { return createCVar(name, "string", val) }
fn double(name, val) { return createCVar(name, "double", val) }
fn float(name, val) { return createCVar(name, "float", val) }
fn char(name, val) { return createCVar(name, "char", val) }

// Specific C integer types
fn c_uint8(name, val) { return createCVar(name, "uint8", val) }
fn c_uint16(name, val) { return createCVar(name, "uint16", val) }
fn c_uint32(name, val) { return createCVar(name, "uint32", val) }
fn c_uint64(name, val) { return createCVar(name, "uint64", val) }
fn c_int8(name, val) { return createCVar(name, "int8", val) }
fn c_int16(name, val) { return createCVar(name, "int16", val) }
fn c_int32(name, val) { return createCVar(name, "int32", val) }
fn c_int64(name, val) { return createCVar(name, "int64", val) }
fn c_size_t(name, val) { return createCVar(name, "size_t", val) }
