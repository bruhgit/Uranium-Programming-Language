#include "native_jit.h"
#include "native_jit_mem.h"
#include <vector>
#include <cstring>
#include <cstdint>
#include <stdexcept>

#if defined(_M_ARM) || defined(__arm__)

namespace {

class Arm32Assembler {
public:
    std::vector<uint32_t> code;

    struct Patch {
        size_t offsetToPatch;
        size_t targetIndex;
    };
    std::vector<Patch> jumpPatches;
    std::vector<Patch> jumpIfFalsePatches;
    std::vector<Patch> loopPatches;

    struct ConstPoolEntry {
        double value;
        size_t offsetToPatch;
    };
    std::vector<ConstPoolEntry> constants;

    void emit(uint32_t inst) {
        code.push_back(inst);
    }

    void emitPush(uint32_t regList) {
        emit(0xE92D0000 | (regList & 0xFFFF));
    }

    void emitPop(uint32_t regList) {
        emit(0xE8BD0000 | (regList & 0xFFFF));
    }

    void emitAddSp(uint32_t bytes) {
        while (bytes > 0) {
            uint32_t chunk = bytes > 255 ? 255 : bytes;
            emit(0xE28DD000 | chunk);
            bytes -= chunk;
        }
    }

    void emitSubSp(uint32_t bytes) {
        while (bytes > 0) {
            uint32_t chunk = bytes > 255 ? 255 : bytes;
            emit(0xE24DD000 | chunk);
            bytes -= chunk;
        }
    }

    void emitVldrSp(uint32_t dReg, uint32_t offset) {
        uint32_t imm8 = (offset / 4) & 0xFF;
        emit(0xED9D0B00 | (dReg << 12) | imm8); // VLDR dReg, [sp, #offset]
    }

    void emitVstrSp(uint32_t dReg, uint32_t offset) {
        uint32_t imm8 = (offset / 4) & 0xFF;
        emit(0xED8D0B00 | (dReg << 12) | imm8); // VSTR dReg, [sp, #offset]
    }

    void emitVldrR0(uint32_t dReg, uint32_t offset) {
        uint32_t imm8 = (offset / 4) & 0xFF;
        emit(0xED900B00 | (dReg << 12) | imm8); // VLDR dReg, [r0, #offset]
    }

    void emitVstrR1(uint32_t dReg, uint32_t offset) {
        uint32_t imm8 = (offset / 4) & 0xFF;
        emit(0xED810B00 | (dReg << 12) | imm8); // VSTR dReg, [r1, #offset]
    }

    void emitLoadDoubleConst(uint32_t dReg, double val) {
        constants.push_back({val, code.size()});
        emit(0xED9F0B00 | (dReg << 12)); // VLDR dReg, [pc, #...]
    }

    void emitVadd(uint32_t dDest, uint32_t dN, uint32_t dM) {
        emit(0xEE300B00 | (dN << 16) | (dDest << 12) | dM); // VADD.F64
    }

    void emitVsub(uint32_t dDest, uint32_t dN, uint32_t dM) {
        emit(0xEE300B40 | (dN << 16) | (dDest << 12) | dM); // VSUB.F64
    }

    void emitVmul(uint32_t dDest, uint32_t dN, uint32_t dM) {
        emit(0xEE200B00 | (dN << 16) | (dDest << 12) | dM); // VMUL.F64
    }

    void emitVdiv(uint32_t dDest, uint32_t dN, uint32_t dM) {
        emit(0xEE800B00 | (dN << 16) | (dDest << 12) | dM); // VDIV.F64
    }

    void emitVneg(uint32_t dDest, uint32_t dM) {
        emit(0xEEB10B40 | (dDest << 12) | dM); // VNEG.F64
    }

    void emitVcmp(uint32_t dN, uint32_t dM) {
        emit(0xEEB40B40 | (dN << 12) | dM); // VCMP.F64
    }

    void emitVmrs() {
        emit(0xEEF1FA10); // VMRS APSR_nzcv, FPSCR
    }

    void emitMovImm(uint32_t reg, uint32_t imm) {
        emit(0xE3A00000 | (reg << 12) | (imm & 0xFF)); // MOV reg, #imm
    }

    void emitBPlaceholder(size_t targetIndex) {
        jumpPatches.push_back({code.size(), targetIndex});
        emit(0xEA000000); // B +0
    }

    void emitBeqPlaceholder(size_t targetIndex) {
        jumpIfFalsePatches.push_back({code.size(), targetIndex});
        emit(0x0A000000); // BEQ +0 (EQ condition code 0000)
    }

    void finalize(NativeJitArtifact* artifact, const std::vector<size_t>& instOffsets) {
        // Resolve jumps
        for (auto& p : jumpPatches) {
            uint32_t targetOffset = instOffsets[p.targetIndex] * 4;
            uint32_t pcOffset = p.offsetToPatch * 4 + 8;
            int32_t diff = (int32_t)targetOffset - (int32_t)pcOffset;
            uint32_t inst = code[p.offsetToPatch];
            code[p.offsetToPatch] = (inst & 0xFF000000) | ((diff / 4) & 0x00FFFFFF);
        }
        
        for (auto& p : jumpIfFalsePatches) {
            uint32_t targetOffset = instOffsets[p.targetIndex] * 4;
            uint32_t pcOffset = p.offsetToPatch * 4 + 8;
            int32_t diff = (int32_t)targetOffset - (int32_t)pcOffset;
            uint32_t inst = code[p.offsetToPatch];
            code[p.offsetToPatch] = (inst & 0xFF000000) | ((diff / 4) & 0x00FFFFFF);
        }
        
        for (auto& p : loopPatches) {
            uint32_t targetOffset = instOffsets[p.targetIndex] * 4;
            uint32_t pcOffset = p.offsetToPatch * 4 + 8;
            int32_t diff = (int32_t)targetOffset - (int32_t)pcOffset;
            uint32_t inst = code[p.offsetToPatch];
            code[p.offsetToPatch] = (inst & 0xFF000000) | ((diff / 4) & 0x00FFFFFF);
        }

        // Build unique constant pool
        struct UniqueConst {
            double value;
            size_t poolOffset; // offset in 'code' array
        };
        std::vector<UniqueConst> uniqueConsts;

        for (auto& entry : constants) {
            int index = -1;
            for (size_t i = 0; i < uniqueConsts.size(); ++i) {
                if (uniqueConsts[i].value == entry.value) {
                    index = (int)i;
                    break;
                }
            }
            if (index == -1) {
                uniqueConsts.push_back({entry.value, 0});
            }
        }
        
        // Append unique constants to code and record their offsets
        for (size_t i = 0; i < uniqueConsts.size(); ++i) {
            uniqueConsts[i].poolOffset = code.size();
            uint32_t* ptr = (uint32_t*)&uniqueConsts[i].value;
            code.push_back(ptr[0]);
            code.push_back(ptr[1]);
        }

        // Now patch the VLDR instructions
        for (auto& entry : constants) {
            size_t targetCodeIndex = 0;
            for (auto& uc : uniqueConsts) {
                if (uc.value == entry.value) {
                    targetCodeIndex = uc.poolOffset;
                    break;
                }
            }
            
            uint32_t targetOffset = targetCodeIndex * 4;
            uint32_t pcOffset = entry.offsetToPatch * 4 + 8;
            int32_t diff = targetOffset - pcOffset;
            
            uint32_t inst = code[entry.offsetToPatch];
            code[entry.offsetToPatch] = inst | ((diff / 4) & 0xFF);
        }

        size_t sizeBytes = code.size() * 4;
        void* execMem = jit_alloc_executable(sizeBytes);
        std::memcpy(execMem, code.data(), sizeBytes);
        jit_make_executable(execMem, sizeBytes);
        
        artifact->region.reset(execMem);
        artifact->entry = execMem;
        artifact->size = sizeBytes;
    }
};

} // namespace

bool compileNativeJit_arm32(const FunctionPtr& function,
                            const FastPathPlan& plan,
                            NativeJitArtifact* artifact,
                            std::string* reason) {
    if (plan.maxStack + plan.localCount + function->arity > 200) {
        if (reason) *reason = "stack_frame_too_large";
        return false;
    }

    Arm32Assembler asm32;
    std::vector<size_t> instOffsets(plan.instructions.size(), 0);

    asm32.emitPush(0x4010); // PUSH {r4, lr}
    uint32_t frameSize = (plan.localCount + function->arity + plan.maxStack) * 8;
    if (frameSize > 0) asm32.emitSubSp(frameSize);

    // Load arguments
    for (int i = 0; i < function->arity; ++i) {
        asm32.emitVldrR0(0, i * 8);
        asm32.emitVstrSp(0, i * 8);
    }

    uint32_t stackTop = (plan.localCount + function->arity) * 8;

    for (size_t i = 0; i < plan.instructions.size(); ++i) {
        instOffsets[i] = asm32.code.size();
        const auto& inst = plan.instructions[i];

        switch (inst.op) {
            case FASTPATH_CONSTANT: {
                asm32.emitLoadDoubleConst(0, function->chunk.constants[inst.operand].asNumber());
                asm32.emitVstrSp(0, stackTop);
                stackTop += 8;
                break;
            }
            case FASTPATH_TRUE: {
                asm32.emitLoadDoubleConst(0, 1.0);
                asm32.emitVstrSp(0, stackTop);
                stackTop += 8;
                break;
            }
            case FASTPATH_FALSE:
            case FASTPATH_NIL: {
                asm32.emitLoadDoubleConst(0, 0.0);
                asm32.emitVstrSp(0, stackTop);
                stackTop += 8;
                break;
            }
            case FASTPATH_GET_LOCAL: {
                asm32.emitVldrSp(0, inst.operand * 8);
                asm32.emitVstrSp(0, stackTop);
                stackTop += 8;
                break;
            }
            case FASTPATH_SET_LOCAL: {
                asm32.emitVldrSp(0, stackTop - 8);
                asm32.emitVstrSp(0, inst.operand * 8);
                break;
            }
            case FASTPATH_ADD: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVadd(0, 0, 1);
                asm32.emitVstrSp(0, stackTop - 16);
                stackTop -= 8;
                break;
            }
            case FASTPATH_SUBTRACT: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVsub(0, 0, 1);
                asm32.emitVstrSp(0, stackTop - 16);
                stackTop -= 8;
                break;
            }
            case FASTPATH_MULTIPLY: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVmul(0, 0, 1);
                asm32.emitVstrSp(0, stackTop - 16);
                stackTop -= 8;
                break;
            }
            case FASTPATH_DIVIDE: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVdiv(0, 0, 1);
                asm32.emitVstrSp(0, stackTop - 16);
                stackTop -= 8;
                break;
            }
            case FASTPATH_NEGATE: {
                asm32.emitVldrSp(0, stackTop - 8);
                asm32.emitVneg(0, 0);
                asm32.emitVstrSp(0, stackTop - 8);
                break;
            }
            case FASTPATH_EQUAL: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVcmp(0, 1);
                asm32.emitVmrs();
                
                asm32.emitLoadDoubleConst(0, 1.0);
                asm32.emitLoadDoubleConst(1, 0.0);
                
                uint32_t patch1 = asm32.code.size();
                asm32.emit(0x0A000000); // BEQ +0 (patch later)
                asm32.emitVstrSp(1, stackTop - 16);
                uint32_t patch2 = asm32.code.size();
                asm32.emit(0xEA000000); // B +0 (patch later)
                
                uint32_t target1 = asm32.code.size();
                asm32.emitVstrSp(0, stackTop - 16);
                uint32_t target2 = asm32.code.size();
                
                asm32.code[patch1] = (asm32.code[patch1] & 0xFF000000) | (((target1*4 - (patch1*4+8))/4) & 0x00FFFFFF);
                asm32.code[patch2] = (asm32.code[patch2] & 0xFF000000) | (((target2*4 - (patch2*4+8))/4) & 0x00FFFFFF);
                
                stackTop -= 8;
                break;
            }
            case FASTPATH_GREATER: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVcmp(0, 1);
                asm32.emitVmrs();
                asm32.emitLoadDoubleConst(0, 1.0);
                asm32.emitLoadDoubleConst(1, 0.0);
                
                uint32_t patch1 = asm32.code.size();
                asm32.emit(0xCA000000); // BGT
                asm32.emitVstrSp(1, stackTop - 16);
                uint32_t patch2 = asm32.code.size();
                asm32.emit(0xEA000000); // B
                
                uint32_t target1 = asm32.code.size();
                asm32.emitVstrSp(0, stackTop - 16);
                uint32_t target2 = asm32.code.size();
                
                asm32.code[patch1] = (asm32.code[patch1] & 0xFF000000) | (((target1*4 - (patch1*4+8))/4) & 0x00FFFFFF);
                asm32.code[patch2] = (asm32.code[patch2] & 0xFF000000) | (((target2*4 - (patch2*4+8))/4) & 0x00FFFFFF);
                
                stackTop -= 8;
                break;
            }
            case FASTPATH_LESS: {
                asm32.emitVldrSp(1, stackTop - 8);
                asm32.emitVldrSp(0, stackTop - 16);
                asm32.emitVcmp(0, 1);
                asm32.emitVmrs();
                asm32.emitLoadDoubleConst(0, 1.0);
                asm32.emitLoadDoubleConst(1, 0.0);
                
                uint32_t patch1 = asm32.code.size();
                asm32.emit(0xBA000000); // BLT
                asm32.emitVstrSp(1, stackTop - 16);
                uint32_t patch2 = asm32.code.size();
                asm32.emit(0xEA000000); // B
                
                uint32_t target1 = asm32.code.size();
                asm32.emitVstrSp(0, stackTop - 16);
                uint32_t target2 = asm32.code.size();
                
                asm32.code[patch1] = (asm32.code[patch1] & 0xFF000000) | (((target1*4 - (patch1*4+8))/4) & 0x00FFFFFF);
                asm32.code[patch2] = (asm32.code[patch2] & 0xFF000000) | (((target2*4 - (patch2*4+8))/4) & 0x00FFFFFF);
                
                stackTop -= 8;
                break;
            }
            case FASTPATH_POP: {
                stackTop -= 8;
                break;
            }
            case FASTPATH_JUMP: {
                asm32.emitBPlaceholder(inst.operand);
                break;
            }
            case FASTPATH_JUMP_IF_FALSE: {
                asm32.emitVldrSp(0, stackTop - 8);
                asm32.emitLoadDoubleConst(1, 0.0);
                asm32.emitVcmp(0, 1);
                asm32.emitVmrs();
                asm32.emitBeqPlaceholder(inst.operand); // jump if equal to 0.0
                break;
            }
            case FASTPATH_LOOP: {
                asm32.loopPatches.push_back({asm32.code.size(), inst.operand});
                asm32.emit(0xEA000000); // B
                break;
            }
            case FASTPATH_RETURN: {
                asm32.emitVldrSp(0, stackTop - 8);
                
                // Assume returning a number (r0=1, store in [r1])
                asm32.emitVstrR1(0, 0);
                asm32.emitMovImm(0, 1); // 1 = type Number
                
                if (frameSize > 0) asm32.emitAddSp(frameSize);
                asm32.emitPop(0x8010); // POP {r4, pc}
                break;
            }
            default:
                if (reason) *reason = "unsupported_opcode";
                return false;
        }
    }

    asm32.finalize(artifact, instOffsets);
    return true;
}

#endif
