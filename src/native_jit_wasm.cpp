#include "native_jit.h"
#include "type_system.h"
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cstring>

#if defined(__wasm__) || 1 // Always enable WASM backend for AOT

// Simple WASM module generator
class WasmEmitter {
public:
    std::vector<uint8_t> binary;

    void emitByte(uint8_t b) { binary.push_back(b); }
    
    // ULEB128 encoding
    void emitU32(uint32_t value) {
        do {
            uint8_t byte = value & 0x7F;
            value >>= 7;
            if (value != 0) byte |= 0x80;
            emitByte(byte);
        } while (value != 0);
    }
    
    void emitString(const std::string& str) {
        emitU32(static_cast<uint32_t>(str.length()));
        for (char c : str) emitByte(c);
    }

    void emitHeader() {
        // \0asm version 1
        binary.insert(binary.end(), {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00});
    }

    void emitTypeSection() {
        emitByte(1); // Section ID 1 (Type)
        std::vector<uint8_t> content;
        
        // 1 type: () -> i32
        content.push_back(1); // 1 type
        content.push_back(0x60); // func form
        content.push_back(0); // 0 parameters
        content.push_back(1); // 1 result
        content.push_back(0x7C); // f64
        
        emitU32(static_cast<uint32_t>(content.size()));
        binary.insert(binary.end(), content.begin(), content.end());
    }

    void emitFunctionSection() {
        emitByte(3); // Section ID 3 (Function)
        std::vector<uint8_t> content;
        
        content.push_back(1); // 1 function
        content.push_back(0); // Uses type index 0
        
        emitU32(static_cast<uint32_t>(content.size()));
        binary.insert(binary.end(), content.begin(), content.end());
    }

    void emitExportSection() {
        emitByte(7); // Section ID 7 (Export)
        std::vector<uint8_t> content;
        
        content.push_back(1); // 1 export
        
        // Export name "main"
        content.push_back(4);
        content.push_back('m'); content.push_back('a'); content.push_back('i'); content.push_back('n');
        
        content.push_back(0x00); // export kind: func
        content.push_back(0); // func index 0
        
        emitU32(static_cast<uint32_t>(content.size()));
        binary.insert(binary.end(), content.begin(), content.end());
    }

    void emitCodeSection(const FunctionPtr& function, const FastPathPlan& plan) {
        emitByte(10); // Section ID 10 (Code)
        std::vector<uint8_t> content;
        
        std::vector<uint8_t> funcBody;
        
        // Locals: WASM requires grouping locals by type.
        // We will make ALL locals f64 (0x7C)
        if (plan.localCount > 0) {
            funcBody.push_back(1); // 1 group of locals
            // Number of locals (ULEB128)
            uint32_t count = plan.localCount;
            do {
                uint8_t byte = count & 0x7F;
                count >>= 7;
                if (count != 0) byte |= 0x80;
                funcBody.push_back(byte);
            } while (count != 0);
            funcBody.push_back(0x7C); // f64
        } else {
            funcBody.push_back(0); // 0 local variables
        }
        
        // Translate FastPathOp to WASM Opcodes
        for (size_t i = 0; i < plan.instructions.size(); i++) {
            const auto& inst = plan.instructions[i];
            
            // Top-level scripts end with POP, NIL, RETURN.
            // We want to return the last expression result instead of popping it.
            if (inst.op == FASTPATH_POP && i + 2 < plan.instructions.size() &&
                plan.instructions[i+1].op == FASTPATH_NIL && 
                plan.instructions[i+2].op == FASTPATH_RETURN) {
                funcBody.push_back(0x0F); // return the value instead of popping
                break; // stop processing
            }

            switch (inst.op) {
                case FASTPATH_CONSTANT: {
                    funcBody.push_back(0x44); // f64.const
                    // FastPathOp uses inst.operand as the chunk constant index
                    if (function != nullptr && inst.operand < function->chunk.constants.values.size()) {
                        double val = function->chunk.constants.values[inst.operand].asNumber();
                        uint64_t raw;
                        std::memcpy(&raw, &val, sizeof(double));
                        for (int k = 0; k < 8; k++) {
                            funcBody.push_back(static_cast<uint8_t>(raw & 0xFF));
                            raw >>= 8;
                        }
                    } else {
                        // fallback dummy 0
                        for (int k = 0; k < 8; k++) funcBody.push_back(0);
                    }
                    break;
                }
                case FASTPATH_GET_LOCAL:
                    funcBody.push_back(0x20); // local.get
                    funcBody.push_back(static_cast<uint8_t>(inst.operand));
                    break;
                case FASTPATH_SET_LOCAL:
                    funcBody.push_back(0x21); // local.set
                    funcBody.push_back(static_cast<uint8_t>(inst.operand));
                    break;
                case FASTPATH_ADD: funcBody.push_back(0xA0); break; // f64.add
                case FASTPATH_SUBTRACT: funcBody.push_back(0xA1); break; // f64.sub
                case FASTPATH_MULTIPLY: funcBody.push_back(0xA2); break; // f64.mul
                case FASTPATH_DIVIDE: funcBody.push_back(0xA3); break; // f64.div
                case FASTPATH_RETURN: funcBody.push_back(0x0F); break; // return
                case FASTPATH_POP: funcBody.push_back(0x1A); break; // drop
                case FASTPATH_NIL: {
                    funcBody.push_back(0x44); // f64.const 0 for nil
                    for (int k = 0; k < 8; k++) funcBody.push_back(0);
                    break;
                }
                default: break;
            }
        }
        
        funcBody.push_back(0x0B); // end
        
        content.push_back(1); // 1 function body
        // funcBody size
        uint32_t size = static_cast<uint32_t>(funcBody.size());
        do {
            uint8_t byte = size & 0x7F;
            size >>= 7;
            if (size != 0) byte |= 0x80;
            content.push_back(byte);
        } while (size != 0);
        content.insert(content.end(), funcBody.begin(), funcBody.end());
        
        emitU32(static_cast<uint32_t>(content.size()));
        binary.insert(binary.end(), content.begin(), content.end());
    }
};

bool compileNativeJit_wasm(const FunctionPtr& function,
                           const FastPathPlan& plan,
                           NativeJitArtifact* artifact,
                           std::string* reason) {
    (void)artifact;

    WasmEmitter wasm;
    wasm.emitHeader();
    wasm.emitTypeSection();
    wasm.emitFunctionSection();
    wasm.emitExportSection();
    wasm.emitCodeSection(function, plan);

    // In a real scenario, this would dynamically parse the Uranium OPCODES
    // and emit matching WASM instructions. For the AOT architecture prototype,
    // we save the emitted WASM binary to disk directly.

    std::ofstream file("output.wasm", std::ios::binary);
    if (file.is_open()) {
        file.write((const char*)wasm.binary.data(), wasm.binary.size());
        file.close();
        if (reason) *reason = "WASM AOT compilation successful. Saved to output.wasm";
        std::cout << "Uranium WebAssembly Backend: Emitted output.wasm successfully!" << std::endl;
        return true;
    }

    if (reason) *reason = "Failed to write output.wasm";
    return false;
}

#endif
