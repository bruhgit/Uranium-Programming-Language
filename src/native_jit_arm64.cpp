#include "native_jit.h"
#include "native_jit_mem.h"
#include "type_system.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(_M_ARM64) || defined(__aarch64__)

namespace {

enum NativeValueKind {
    NATIVE_KIND_UNKNOWN,
    NATIVE_KIND_NUMBER,
    NATIVE_KIND_BOOL,
};

NativeValueKind parameterNativeKind(const std::string& annotation) {
    std::string normalized = normalizeTypeAnnotation(annotation);
    if (normalized == "Bool") {
        return NATIVE_KIND_BOOL;
    }
    if (normalized.empty() || normalized == "Any" || normalized == "Number" || normalized == "Int") {
        return NATIVE_KIND_NUMBER;
    }
    return NATIVE_KIND_UNKNOWN;
}

class Arm64Assembler {
public:
    void emitInstruction(uint32_t inst) {
        code.push_back(inst);
    }

    void emitDouble(double value) {
        uint64_t raw;
        std::memcpy(&raw, &value, sizeof(value));
        constants.push_back(raw);
    }

    // sub sp, sp, #imm
    void emitSubSp(int imm) {
        // imm must be divisible by 16 for SP alignment in ARM64
        int aligned = (imm + 15) & ~15;
        if (aligned <= 4095) {
            emitInstruction(0xD10003FF | (aligned << 10)); // sub sp, sp, #aligned
        }
    }

    // add sp, sp, #imm
    void emitAddSp(int imm) {
        int aligned = (imm + 15) & ~15;
        if (aligned <= 4095) {
            emitInstruction(0x910003FF | (aligned << 10)); // add sp, sp, #aligned
        }
    }

    // ldr d(Rd), [x0, #offset] (loads double from args pointer x0)
    void emitLdrDFromArgs(int Rd, int offset) {
        int scaled = offset / 8;
        emitInstruction(0xFD400000 | (scaled << 10) | (0 << 5) | Rd); // x0 is register 0
    }

    // ldr d(Rd), [sp, #offset]
    void emitLdrDFromSp(int Rd, int offset) {
        int scaled = offset / 8;
        emitInstruction(0xFD400000 | (scaled << 10) | (31 << 5) | Rd); // sp is register 31
    }

    // str d(Rd), [sp, #offset]
    void emitStrDToSp(int Rd, int offset) {
        int scaled = offset / 8;
        emitInstruction(0xFC400000 | (scaled << 10) | (31 << 5) | Rd); // sp is register 31
    }

    // str d(Rd), [x1] (saves result to resultNumber pointer x1)
    void emitStrDToResult(int Rd) {
        emitInstruction(0xFC400020 | Rd); // x1 is register 1, offset 0
    }

    // fadd d(Rd), d(Rn), d(Rm)
    void emitFaddD(int Rd, int Rn, int Rm) {
        emitInstruction(0x1E602800 | (Rm << 16) | (Rn << 5) | Rd);
    }

    // fsub d(Rd), d(Rn), d(Rm)
    void emitFsubD(int Rd, int Rn, int Rm) {
        emitInstruction(0x1E603800 | (Rm << 16) | (Rn << 5) | Rd);
    }

    // fmul d(Rd), d(Rn), d(Rm)
    void emitFmulD(int Rd, int Rn, int Rm) {
        emitInstruction(0x1E600800 | (Rm << 16) | (Rn << 5) | Rd);
    }

    // fdiv d(Rd), d(Rn), d(Rm)
    void emitFdivD(int Rd, int Rn, int Rm) {
        emitInstruction(0x1E601800 | (Rm << 16) | (Rn << 5) | Rd);
    }

    // fcmp d(Rn), d(Rm)
    void emitFcmpD(int Rn, int Rm) {
        emitInstruction(0x1E602000 | (Rm << 16) | (Rn << 5));
    }

    // fneg d(Rd), d(Rn)
    void emitFnegD(int Rd, int Rn) {
        emitInstruction(0x1E614000 | (Rn << 5) | Rd);
    }

    // mov w0, #imm (sets return value code in w0)
    void emitMovW0(int imm) {
        emitInstruction(0x52800000 | ((imm & 0xFFFF) << 5));
    }

    // ret
    void emitRet() {
        emitInstruction(0xD65F03C0);
    }

    // ldr d(Rd), [pc, #offset] (loads double constant relative to Program Counter)
    void emitLdrDFromPc(int Rd) {
        emitInstruction(0xFC000000 | Rd); // offset relative to PC will be patched in finalize
        std::size_t patchOffset = code.size() - 1;
        constantPatches.push_back({patchOffset, 0});
    }

    void emitLdrDFromConstant(int Rd, double value) {
        emitLdrDFromPc(Rd);
        constantPatches.back().constantIndex = internConstant(value);
    }

    // Unconditional branch (b offset)
    std::size_t emitBPlaceholder() {
        emitInstruction(0x14000000);
        return code.size() - 1;
    }

    // Conditional branch if EQ (b.eq offset)
    std::size_t emitBeqPlaceholder() {
        emitInstruction(0x54000000); // EQ cond is 0
        return code.size() - 1;
    }

    // Conditional branch if NE (b.ne offset)
    std::size_t emitBnePlaceholder() {
        emitInstruction(0x54000001); // NE cond is 1
        return code.size() - 1;
    }

    // Conditional branch if MI (b.mi offset - minus/less than)
    std::size_t emitBmiPlaceholder() {
        emitInstruction(0x54000004); // MI cond is 4
        return code.size() - 1;
    }

    // Conditional branch if GT (b.gt offset - greater than)
    std::size_t emitBgtPlaceholder() {
        emitInstruction(0x5400000C); // GT cond is 12
        return code.size() - 1;
    }

    void patchJump(std::size_t patchOffset, std::size_t targetOffset) {
        int32_t diff = static_cast<int32_t>(targetOffset - patchOffset);
        uint32_t inst = code[patchOffset];
        if ((inst & 0xFF000000) == 0x54000000) {
            // b.cond has 19 bits signed immediate
            inst |= ((diff & 0x7FFFF) << 5);
        } else {
            // b has 26 bits signed immediate
            inst |= (diff & 0x3FFFFFF);
        }
        code[patchOffset] = inst;
    }

    bool finalize(NativeJitArtifact* artifact,
                  const std::vector<std::size_t>& instructionOffsets,
                  const std::vector<std::pair<std::size_t, uint16_t>>& jumpPatches) {
        if (artifact == nullptr) return false;

        // Apply jump patches
        for (const auto& jumpPatch : jumpPatches) {
            if (jumpPatch.second >= instructionOffsets.size()) return false;
            patchJump(jumpPatch.first, instructionOffsets[jumpPatch.second]);
        }

        // Write constant pool at the end
        std::vector<std::size_t> constantOffsets(constants.size());
        for (std::size_t index = 0; index < constants.size(); ++index) {
            constantOffsets[index] = code.size();
            uint64_t raw = constants[index];
            code.push_back(static_cast<uint32_t>(raw & 0xFFFFFFFF));
            code.push_back(static_cast<uint32_t>(raw >> 32));
        }

        // Patch PC-relative loads of constants
        for (const auto& patch : constantPatches) {
            std::size_t constOffset = constantOffsets[patch.constantIndex];
            int32_t diffBytes = static_cast<int32_t>((constOffset - patch.displacementOffset) * 4);
            int32_t diffInst = diffBytes / 4; // PC relative is in instruction count or byte count
            // ARM64 LDR (literal) offset is in 32-bit words (signed 19-bit imm)
            uint32_t inst = code[patch.displacementOffset];
            inst |= ((diffInst & 0x7FFFF) << 5);
            code[patch.displacementOffset] = inst;
        }

        // Allocate and write to memory
        std::size_t codeSizeInBytes = code.size() * sizeof(uint32_t);
        void* region = jit_alloc_executable(codeSizeInBytes);
        if (region == nullptr) return false;

        std::memcpy(region, code.data(), codeSizeInBytes);
        jit_make_executable(region, codeSizeInBytes);

        artifact->region.reset(region);
        artifact->entry = region;
        artifact->size = codeSizeInBytes;
        return true;
    }

    std::size_t getOffset() const {
        return code.size();
    }

private:
    struct ConstantPatch {
        std::size_t displacementOffset;
        std::size_t constantIndex;
    };

    std::size_t internConstant(double value) {
        uint64_t raw;
        std::memcpy(&raw, &value, sizeof(value));
        for (std::size_t index = 0; index < constants.size(); ++index) {
            if (constants[index] == raw) {
                return index;
            }
        }
        constants.push_back(raw);
        return constants.size() - 1;
    }

    std::vector<uint32_t> code;
    std::vector<uint64_t> constants;
    std::vector<ConstantPatch> constantPatches;
};

bool isNumericNativeCandidate(const FunctionPtr& function,
                              const FastPathPlan& plan,
                              std::string* reason,
                              bool* returnsBoolean) {
    if (function == nullptr) {
        if (reason) *reason = "invalid";
        return false;
    }
    if (function->hasReceiverSlot) {
        if (reason) *reason = "receiver_slot";
        return false;
    }
    std::vector<NativeValueKind> slotKinds(
        std::max<std::size_t>(
            std::max<std::size_t>(plan.localCount, static_cast<std::size_t>(function->arity)) +
                static_cast<std::size_t>(std::max<uint16_t>(1, plan.maxStack)) + 4,
            static_cast<std::size_t>(8)),
        NATIVE_KIND_UNKNOWN);
        
    std::vector<bool> initializedSlots(slotKinds.size(), false);
    for (int index = 0; index < function->arity; ++index) {
        NativeValueKind kind = NATIVE_KIND_NUMBER;
        if (static_cast<std::size_t>(index) < function->parameterTypes.size()) {
            kind = parameterNativeKind(function->parameterTypes[static_cast<std::size_t>(index)]);
            if (kind == NATIVE_KIND_UNKNOWN) {
                if (reason) *reason = "parameter_type";
                return false;
            }
        }
        slotKinds[static_cast<std::size_t>(index)] = kind;
    }

    // Check if the plan is numeric candidate
    bool sawReturn = false;
    NativeValueKind returnKind = NATIVE_KIND_UNKNOWN;

    for (std::size_t index = 0; index < plan.instructions.size(); ++index) {
        const FastPathInstruction& inst = plan.instructions[index];
        int stackTop = function->arity + static_cast<int>(plan.entryStackDepths[index]);
        
        switch (inst.op) {
            case FASTPATH_CONSTANT: {
                const Value& val = function->chunk.constants.values[inst.operand];
                if (!val.isNumber() && !val.isBool() && !val.isInt()) {
                    if (reason) *reason = "non_numeric_constant";
                    return false;
                }
                slotKinds[static_cast<std::size_t>(stackTop)] = val.isBool() ? NATIVE_KIND_BOOL : NATIVE_KIND_NUMBER;
                break;
            }
            case FASTPATH_TRUE:
            case FASTPATH_FALSE:
                slotKinds[static_cast<std::size_t>(stackTop)] = NATIVE_KIND_BOOL;
                break;
            case FASTPATH_GET_LOCAL:
                slotKinds[static_cast<std::size_t>(stackTop)] = slotKinds[static_cast<std::size_t>(inst.operand)];
                break;
            case FASTPATH_SET_LOCAL:
                slotKinds[static_cast<std::size_t>(inst.operand)] = slotKinds[static_cast<std::size_t>(stackTop - 1)];
                break;
            case FASTPATH_ADD:
            case FASTPATH_SUBTRACT:
            case FASTPATH_MULTIPLY:
            case FASTPATH_DIVIDE:
                if (slotKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER ||
                    slotKinds[static_cast<std::size_t>(stackTop - 2)] != NATIVE_KIND_NUMBER) {
                    if (reason) *reason = "numeric_stack";
                    return false;
                }
                slotKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_NUMBER;
                break;
            case FASTPATH_NEGATE:
                if (slotKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER) {
                    if (reason) *reason = "numeric_stack";
                    return false;
                }
                break;
            case FASTPATH_EQUAL:
                slotKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_BOOL;
                break;
            case FASTPATH_GREATER:
            case FASTPATH_LESS:
                slotKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_BOOL;
                break;
            case FASTPATH_NOT:
                slotKinds[static_cast<std::size_t>(stackTop - 1)] = NATIVE_KIND_BOOL;
                break;
            case FASTPATH_POP:
                break;
            case FASTPATH_JUMP:
            case FASTPATH_LOOP:
            case FASTPATH_JUMP_IF_FALSE:
                break;
            case FASTPATH_RETURN: {
                NativeValueKind kind = slotKinds[static_cast<std::size_t>(stackTop - 1)];
                if (!sawReturn) {
                    returnKind = kind;
                    sawReturn = true;
                } else if (returnKind != kind) {
                    if (reason) *reason = "return_kind";
                    return false;
                }
                break;
            }
            default:
                if (reason) *reason = "unsupported_op";
                return false;
        }
    }
    if (!sawReturn) {
        if (reason) *reason = "return_shape";
        return false;
    }
    *returnsBoolean = returnKind == NATIVE_KIND_BOOL;
    return true;
}

} // namespace

bool compileNativeJit_arm64(const FunctionPtr& function,
                            const FastPathPlan& plan,
                            NativeJitArtifact* artifact,
                            std::string* reason) {
    if (reason) reason->clear();
    if (artifact == nullptr) return false;

    bool returnsBoolean = false;
    if (!isNumericNativeCandidate(function, plan, reason, &returnsBoolean)) {
        return false;
    }

    Arm64Assembler assembler;
    std::size_t frameSlotCount =
        std::max<std::size_t>(plan.localCount, static_cast<std::size_t>(function->arity)) +
        static_cast<std::size_t>(std::max<uint16_t>(1, plan.maxStack)) + 4;
    int stackBytes = static_cast<int>(frameSlotCount * sizeof(double));
    assembler.emitSubSp(stackBytes);

    // Save initial double arguments to Stack slots
    for (int index = 0; index < function->arity; ++index) {
        assembler.emitLdrDFromArgs(0, index * 8); // Loads args[index] into d0
        assembler.emitStrDToSp(0, index * 8);    // Saves d0 to Stack slot [sp + index * 8]
    }

    std::vector<std::size_t> instructionOffsets(plan.instructions.size());
    std::vector<std::pair<std::size_t, uint16_t>> jumpPatches;

    for (std::size_t index = 0; index < plan.instructions.size(); ++index) {
        instructionOffsets[index] = assembler.getOffset();
        const FastPathInstruction& instruction = plan.instructions[index];
        int stackTop = function->arity + static_cast<int>(plan.entryStackDepths[index]);
        int offsetTop = stackTop * 8;
        
        switch (instruction.op) {
            case FASTPATH_CONSTANT: {
                const Value& constant = function->chunk.constants.values[instruction.operand];
                double val = 0.0;
                if (constant.isBool()) val = constant.asBool() ? 1.0 : 0.0;
                else if (constant.isInt()) val = static_cast<double>(constant.asInt());
                else val = constant.asNumber();
                assembler.emitLdrDFromConstant(0, val);
                assembler.emitStrDToSp(0, offsetTop);
                break;
            }
            case FASTPATH_TRUE:
                assembler.emitLdrDFromConstant(0, 1.0);
                assembler.emitStrDToSp(0, offsetTop);
                break;
            case FASTPATH_FALSE:
                assembler.emitLdrDFromConstant(0, 0.0);
                assembler.emitStrDToSp(0, offsetTop);
                break;
            case FASTPATH_GET_LOCAL:
                assembler.emitLdrDFromSp(0, static_cast<int>(instruction.operand) * 8);
                assembler.emitStrDToSp(0, offsetTop);
                break;
            case FASTPATH_SET_LOCAL:
                assembler.emitLdrDFromSp(0, (stackTop - 1) * 8);
                assembler.emitStrDToSp(0, static_cast<int>(instruction.operand) * 8);
                break;
            case FASTPATH_ADD:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFaddD(0, 0, 1);
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_SUBTRACT:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFsubD(0, 0, 1);
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_MULTIPLY:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFmulD(0, 0, 1);
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_DIVIDE:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFdivD(0, 0, 1);
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_NEGATE:
                assembler.emitLdrDFromSp(0, (stackTop - 1) * 8);
                assembler.emitFnegD(0, 0);
                assembler.emitStrDToSp(0, (stackTop - 1) * 8);
                break;
            case FASTPATH_NOT:
                // Not: if d0 == 0.0 then 1.0 else 0.0
                assembler.emitLdrDFromSp(0, (stackTop - 1) * 8);
                assembler.emitLdrDFromConstant(1, 0.0);
                assembler.emitFcmpD(0, 1);
                {
                    std::size_t patchEq = assembler.emitBeqPlaceholder();
                    assembler.emitLdrDFromConstant(0, 0.0);
                    std::size_t patchJmp = assembler.emitBPlaceholder();
                    assembler.patchJump(patchEq, assembler.getOffset());
                    assembler.emitLdrDFromConstant(0, 1.0);
                    assembler.patchJump(patchJmp, assembler.getOffset());
                }
                assembler.emitStrDToSp(0, (stackTop - 1) * 8);
                break;
            case FASTPATH_EQUAL:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFcmpD(0, 1);
                {
                    std::size_t patchEq = assembler.emitBeqPlaceholder();
                    assembler.emitLdrDFromConstant(0, 0.0);
                    std::size_t patchJmp = assembler.emitBPlaceholder();
                    assembler.patchJump(patchEq, assembler.getOffset());
                    assembler.emitLdrDFromConstant(0, 1.0);
                    assembler.patchJump(patchJmp, assembler.getOffset());
                }
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_GREATER:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFcmpD(0, 1);
                {
                    std::size_t patchGt = assembler.emitBgtPlaceholder();
                    assembler.emitLdrDFromConstant(0, 0.0);
                    std::size_t patchJmp = assembler.emitBPlaceholder();
                    assembler.patchJump(patchGt, assembler.getOffset());
                    assembler.emitLdrDFromConstant(0, 1.0);
                    assembler.patchJump(patchJmp, assembler.getOffset());
                }
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_LESS:
                assembler.emitLdrDFromSp(0, (stackTop - 2) * 8);
                assembler.emitLdrDFromSp(1, (stackTop - 1) * 8);
                assembler.emitFcmpD(0, 1);
                {
                    std::size_t patchLt = assembler.emitBmiPlaceholder();
                    assembler.emitLdrDFromConstant(0, 0.0);
                    std::size_t patchJmp = assembler.emitBPlaceholder();
                    assembler.patchJump(patchLt, assembler.getOffset());
                    assembler.emitLdrDFromConstant(0, 1.0);
                    assembler.patchJump(patchJmp, assembler.getOffset());
                }
                assembler.emitStrDToSp(0, (stackTop - 2) * 8);
                break;
            case FASTPATH_JUMP:
            case FASTPATH_LOOP:
                assembler.emitBPlaceholder();
                jumpPatches.push_back({assembler.getOffset() - 1, instruction.operand});
                break;
            case FASTPATH_JUMP_IF_FALSE:
                assembler.emitLdrDFromSp(0, (stackTop - 1) * 8);
                assembler.emitLdrDFromConstant(1, 0.0);
                assembler.emitFcmpD(0, 1);
                assembler.emitBeqPlaceholder();
                jumpPatches.push_back({assembler.getOffset() - 1, instruction.operand});
                break;
            case FASTPATH_POP:
                break;
            case FASTPATH_RETURN:
                assembler.emitLdrDFromSp(0, (stackTop - 1) * 8);
                if (returnsBoolean) {
                    assembler.emitLdrDFromConstant(1, 0.0);
                    assembler.emitFcmpD(0, 1);
                    std::size_t patchEq = assembler.emitBeqPlaceholder();
                    assembler.emitMovW0(3); // Code 3 = True
                    std::size_t patchJmp = assembler.emitBPlaceholder();
                    assembler.patchJump(patchEq, assembler.getOffset());
                    assembler.emitMovW0(2); // Code 2 = False
                    assembler.patchJump(patchJmp, assembler.getOffset());
                } else {
                    assembler.emitStrDToResult(0);
                    assembler.emitMovW0(1); // Code 1 = Number
                }
                assembler.emitAddSp(stackBytes);
                assembler.emitRet();
                break;
            default:
                if (reason) *reason = "unsupported_op";
                return false;
        }
    }

    return assembler.finalize(artifact, instructionOffsets, jumpPatches);
}

#endif
