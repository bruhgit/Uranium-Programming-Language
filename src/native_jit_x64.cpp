#include "native_jit.h"
#include "native_jit_mem.h"
#include "type_system.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if (defined(_WIN32) && defined(_M_X64)) || defined(__x86_64__)

#ifdef _WIN32
#include <windows.h>
#endif

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
        return NATIVE_KIND_NUMBER; // VAL_INT promoted to double inside JIT
    }
    return NATIVE_KIND_UNKNOWN;
}

class X64Assembler {
public:
    void emitByte(uint8_t byte) {
        code.push_back(byte);
    }

    void emitBytes(std::initializer_list<uint8_t> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    }

    void emitInt32(int32_t value) {
        uint8_t bytes[sizeof(value)];
        std::memcpy(bytes, &value, sizeof(value));
        code.insert(code.end(), bytes, bytes + sizeof(value));
    }

    void emitDouble(double value) {
        uint8_t bytes[sizeof(value)];
        std::memcpy(bytes, &value, sizeof(value));
        code.insert(code.end(), bytes, bytes + sizeof(value));
    }

    void emitSubRsp(int bytes) {
        if (bytes <= 0) {
            return;
        }

        if (bytes <= 127) {
            emitBytes({0x48, 0x83, 0xEC, static_cast<uint8_t>(bytes)});
            return;
        }

        emitBytes({0x48, 0x81, 0xEC});
        emitInt32(bytes);
    }

    void emitAddRsp(int bytes) {
        if (bytes <= 0) {
            return;
        }

        if (bytes <= 127) {
            emitBytes({0x48, 0x83, 0xC4, static_cast<uint8_t>(bytes)});
            return;
        }

        emitBytes({0x48, 0x81, 0xC4});
        emitInt32(bytes);
    }

    void emitJmp32(int32_t val) {
        emitByte(0xE9);
        emitInt32(val);
    }

    void emitJz32(int32_t val) {
        emitBytes({0x0F, 0x84});
        emitInt32(val);
    }

    std::size_t emitJmpPlaceholder() {
        emitByte(0xE9);
        std::size_t patchOffset = code.size();
        emitInt32(0);
        return patchOffset;
    }

    std::size_t emitJzPlaceholder() {
        emitBytes({0x0F, 0x84});
        std::size_t patchOffset = code.size();
        emitInt32(0);
        return patchOffset;
    }

    void patchJump(std::size_t patchOffset, std::size_t targetOffset) {
        int32_t displacement = static_cast<int32_t>(targetOffset - (patchOffset + sizeof(int32_t)));
        std::memcpy(code.data() + patchOffset, &displacement, sizeof(displacement));
    }

    void emitReturn() {
        emitByte(0xC3);
    }

    void emitMovEaxImm32(int value) {
        emitByte(0xB8);
        emitInt32(value);
    }

    void emitZeroEax() {
        emitMovEaxImm32(0);
    }

    void emitAddEaxImm8(uint8_t value) {
        emitBytes({0x83, 0xC0, value});
    }

    void emitSetaAl() {
        emitBytes({0x0F, 0x97, 0xC0});
    }

    void emitSeteAl() {
        emitBytes({0x0F, 0x94, 0xC0});
    }

    void emitSetnpDl() {
        emitBytes({0x0F, 0x9B, 0xC2});
    }

    void emitAndAlDl() {
        emitBytes({0x20, 0xD0});
    }

    void emitCvtsi2sdXmm0Eax() {
        emitBytes({0xF2, 0x0F, 0x2A, 0xC0});
    }

    void emitMovsdXmm0FromArg(int argIndex) {
#ifdef _WIN32
        // Windows x64: args pointer is in rcx.
        // ModR/M byte 0x81 represents [rcx + offset] with 32-bit displacement
        emitBytes({0xF2, 0x0F, 0x10, 0x81});
#else
        // System V ABI: args pointer is in rdi.
        // ModR/M byte 0x87 represents [rdi + offset] with 32-bit displacement
        emitBytes({0xF2, 0x0F, 0x10, 0x87});
#endif
        emitInt32(argIndex * static_cast<int>(sizeof(double)));
    }

    void emitMovsdXmm0FromRsp(int slotIndex) {
        emitBytes({0xF2, 0x0F, 0x10, 0x84, 0x24});
        emitInt32(slotIndex * static_cast<int>(sizeof(double)));
    }

    void emitMovsdXmm1FromRsp(int slotIndex) {
        emitBytes({0xF2, 0x0F, 0x10, 0x8C, 0x24});
        emitInt32(slotIndex * static_cast<int>(sizeof(double)));
    }

    void emitMovsdRspFromXmm0(int slotIndex) {
        emitBytes({0xF2, 0x0F, 0x11, 0x84, 0x24});
        emitInt32(slotIndex * static_cast<int>(sizeof(double)));
    }

    void emitMovsdRspFromXmm1(int slotIndex) {
        emitBytes({0xF2, 0x0F, 0x11, 0x8C, 0x24});
        emitInt32(slotIndex * static_cast<int>(sizeof(double)));
    }

    void emitMovsdResultFromXmm0() {
#ifdef _WIN32
        // Windows x64: result pointer is in rdx.
        // ModR/M byte 0x02 represents [rdx]
        emitBytes({0xF2, 0x0F, 0x11, 0x02});
#else
        // System V ABI: result pointer is in rsi.
        // ModR/M byte 0x06 represents [rsi]
        emitBytes({0xF2, 0x0F, 0x11, 0x06});
#endif
    }

    void emitMovsdXmm0FromConstant(double value) {
        emitBytes({0xF2, 0x0F, 0x10, 0x05});
        std::size_t patchOffset = code.size();
        emitInt32(0);
        constantPatches.push_back({patchOffset, internConstant(value)});
    }

    void emitAddsdXmm0Xmm1() {
        emitBytes({0xF2, 0x0F, 0x58, 0xC1});
    }

    void emitSubsdXmm0Xmm1() {
        emitBytes({0xF2, 0x0F, 0x5C, 0xC1});
    }

    void emitMulsdXmm0Xmm1() {
        emitBytes({0xF2, 0x0F, 0x59, 0xC1});
    }

    void emitDivsdXmm0Xmm1() {
        emitBytes({0xF2, 0x0F, 0x5E, 0xC1});
    }

    void emitXorpdXmm1Xmm1() {
        emitBytes({0x66, 0x0F, 0x57, 0xC9});
    }

    void emitSubsdXmm1Xmm0() {
        emitBytes({0xF2, 0x0F, 0x5C, 0xC8});
    }

    void emitUcomisdXmm0Xmm1() {
        emitBytes({0x66, 0x0F, 0x2E, 0xC1});
    }

    bool finalize(NativeJitArtifact* artifact, 
                  const std::vector<std::size_t>& instructionOffsets,
                  const std::vector<std::pair<std::size_t, uint16_t>>& jumpPatches) {
        if (artifact == nullptr) {
            return false;
        }

        for (const auto& jumpPatch : jumpPatches) {
            if (jumpPatch.second >= instructionOffsets.size()) return false;
            int32_t displacement = static_cast<int32_t>(
                instructionOffsets[jumpPatch.second] -
                (jumpPatch.first + sizeof(int32_t)));
            std::memcpy(code.data() + jumpPatch.first,
                        &displacement,
                        sizeof(displacement));
        }

        std::vector<std::size_t> constantOffsets(constants.size());
        for (std::size_t index = 0; index < constants.size(); ++index) {
            constantOffsets[index] = code.size();
            emitDouble(constants[index]);
        }

        for (const ConstantPatch& patch : constantPatches) {
            int32_t displacement = static_cast<int32_t>(
                constantOffsets[patch.constantIndex] -
                (patch.displacementOffset + sizeof(int32_t)));
            std::memcpy(code.data() + patch.displacementOffset,
                        &displacement,
                        sizeof(displacement));
        }

        // Use our new platform-independent memory allocator!
        void* region = jit_alloc_executable(code.size());
        if (region == nullptr) {
            return false;
        }

        std::memcpy(region, code.data(), code.size());
        jit_make_executable(region, code.size()); // Make pages executable/read-only if hardened
        
        artifact->region.reset(region);
        artifact->entry = region;
        artifact->size = code.size();
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
        for (std::size_t index = 0; index < constants.size(); ++index) {
            if (constants[index] == value) {
                return index;
            }
        }

        constants.push_back(value);
        return constants.size() - 1;
    }

    std::vector<uint8_t> code;
    std::vector<double> constants;
    std::vector<ConstantPatch> constantPatches;
};

bool isNumericNativeCandidate(const FunctionPtr& function,
                              const FastPathPlan& plan,
                              std::string* reason,
                              bool* returnsBoolean) {
    if (function == nullptr) {
        if (reason != nullptr) {
            *reason = "invalid";
        }
        return false;
    }

    if (function->hasReceiverSlot) {
        if (reason != nullptr) {
            *reason = "receiver_slot";
        }
        return false;
    }

    std::vector<NativeValueKind> slotKinds(
        std::max<std::size_t>(
            std::max<std::size_t>(plan.localCount, static_cast<std::size_t>(function->arity)) +
                static_cast<std::size_t>(std::max<uint16_t>(1, plan.maxStack)) + 4,
            static_cast<std::size_t>(8)),
        NATIVE_KIND_UNKNOWN);
    if (plan.entryStackDepths.size() != plan.instructions.size()) {
        if (reason != nullptr) {
            *reason = "plan_stack_depths";
        }
        return false;
    }

    struct NativeFlowState {
        bool initialized;
        int stackTop;
        std::vector<NativeValueKind> slotKinds;

        NativeFlowState()
            : initialized(false), stackTop(0) {
        }
    };

    auto mergeState = [&](NativeFlowState* destination,
                          int stackTop,
                          const std::vector<NativeValueKind>& incomingKinds) -> int {
        if (destination == nullptr) {
            return -1;
        }

        if (!destination->initialized) {
            destination->initialized = true;
            destination->stackTop = stackTop;
            destination->slotKinds = incomingKinds;
            return 1;
        }

        if (destination->stackTop != stackTop) {
            if (reason != nullptr) {
                *reason = "stack_merge";
            }
            return -1;
        }

        bool changed = false;
        for (int slot = 0; slot < stackTop; ++slot) {
            NativeValueKind existing = destination->slotKinds[static_cast<std::size_t>(slot)];
            NativeValueKind incoming = incomingKinds[static_cast<std::size_t>(slot)];
            if (existing == NATIVE_KIND_UNKNOWN && incoming != NATIVE_KIND_UNKNOWN) {
                destination->slotKinds[static_cast<std::size_t>(slot)] = incoming;
                changed = true;
                continue;
            }
            if (incoming == NATIVE_KIND_UNKNOWN || existing == incoming) {
                continue;
            }
            if (reason != nullptr) {
                *reason = "type_merge";
            }
            return -1;
        }
        return changed ? 1 : 0;
    };

    std::vector<NativeFlowState> entryStates(plan.instructions.size());
    NativeFlowState initialState;
    initialState.initialized = true;
    initialState.stackTop = function->arity;
    initialState.slotKinds = slotKinds;

    for (int index = 0; index < function->arity; ++index) {
        NativeValueKind kind = NATIVE_KIND_NUMBER;
        if (static_cast<std::size_t>(index) < function->parameterTypes.size()) {
            kind = parameterNativeKind(function->parameterTypes[static_cast<std::size_t>(index)]);
            if (kind == NATIVE_KIND_UNKNOWN) {
                if (reason != nullptr) {
                    *reason = "parameter_type";
                }
                return false;
            }
        }
        initialState.slotKinds[static_cast<std::size_t>(index)] = kind;
    }

    entryStates[0] = initialState;
    std::vector<std::size_t> worklist = {0};
    bool sawReturn = false;
    NativeValueKind returnKind = NATIVE_KIND_UNKNOWN;

    while (!worklist.empty()) {
        std::size_t index = worklist.back();
        worklist.pop_back();

        const NativeFlowState& entry = entryStates[index];
        if (!entry.initialized) {
            continue;
        }

        const FastPathInstruction& instruction = plan.instructions[index];
        if (entry.stackTop != function->arity +
                                  static_cast<int>(plan.entryStackDepths[index])) {
            if (reason != nullptr) {
                *reason = "stack_depth_mismatch";
            }
            return false;
        }

        int stackTop = entry.stackTop;
        std::vector<NativeValueKind> stateKinds = entry.slotKinds;
        std::vector<std::size_t> successors;

        auto pushSuccessor = [&](std::size_t target) -> bool {
            if (target >= plan.instructions.size()) {
                if (reason != nullptr) {
                    *reason = "jump_target";
                }
                return false;
            }

            int mergeResult = mergeState(&entryStates[target], stackTop, stateKinds);
            if (mergeResult < 0) {
                return false;
            }
            if (mergeResult > 0) {
                worklist.push_back(target);
            }
            return true;
        };

        switch (instruction.op) {
            case FASTPATH_CONSTANT: {
                if (instruction.operand >= function->chunk.constants.values.size()) {
                    if (reason != nullptr) {
                        *reason = "non_numeric_constant";
                    }
                    return false;
                }
                const Value& constant = function->chunk.constants.values[instruction.operand];
                if (!constant.isNumber() && !constant.isBool() && !constant.isInt()) {
                    if (reason != nullptr) {
                        *reason = "non_numeric_constant";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop++)] =
                    constant.isBool() ? NATIVE_KIND_BOOL : NATIVE_KIND_NUMBER;
                successors.push_back(index + 1);
                break;
            }
            case FASTPATH_NIL:
                if (reason != nullptr) {
                    *reason = "nil_constant";
                }
                return false;
            case FASTPATH_TRUE:
            case FASTPATH_FALSE:
                stateKinds[static_cast<std::size_t>(stackTop++)] = NATIVE_KIND_BOOL;
                successors.push_back(index + 1);
                break;
            case FASTPATH_GET_LOCAL:
                if (instruction.operand >= static_cast<uint16_t>(stackTop) ||
                    stateKinds[static_cast<std::size_t>(instruction.operand)] == NATIVE_KIND_UNKNOWN) {
                    if (reason != nullptr) {
                        *reason = "local_kind";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop++)] =
                    stateKinds[static_cast<std::size_t>(instruction.operand)];
                successors.push_back(index + 1);
                break;
            case FASTPATH_SET_LOCAL:
                if (instruction.operand >= stateKinds.size() ||
                    stackTop <= 0 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] == NATIVE_KIND_UNKNOWN) {
                    if (reason != nullptr) {
                        *reason = "local_write";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(instruction.operand)] =
                    stateKinds[static_cast<std::size_t>(stackTop - 1)];
                successors.push_back(index + 1);
                break;
            case FASTPATH_ADD:
            case FASTPATH_SUBTRACT:
            case FASTPATH_MULTIPLY:
            case FASTPATH_DIVIDE:
                if (stackTop < 2 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER ||
                    stateKinds[static_cast<std::size_t>(stackTop - 2)] != NATIVE_KIND_NUMBER) {
                    if (reason != nullptr) {
                        *reason = "numeric_stack";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_NUMBER;
                stackTop--;
                successors.push_back(index + 1);
                break;
            case FASTPATH_NEGATE:
                if (stackTop < 1 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER) {
                    if (reason != nullptr) {
                        *reason = "numeric_stack";
                    }
                    return false;
                }
                successors.push_back(index + 1);
                break;
            case FASTPATH_EQUAL:
                if (stackTop < 2 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 2)] == NATIVE_KIND_UNKNOWN ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] == NATIVE_KIND_UNKNOWN ||
                    stateKinds[static_cast<std::size_t>(stackTop - 2)] !=
                        stateKinds[static_cast<std::size_t>(stackTop - 1)]) {
                    if (reason != nullptr) {
                        *reason = "numeric_stack";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_BOOL;
                stackTop--;
                successors.push_back(index + 1);
                break;
            case FASTPATH_GREATER:
            case FASTPATH_LESS:
                if (stackTop < 2 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 2)] != NATIVE_KIND_NUMBER ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER) {
                    if (reason != nullptr) {
                        *reason = "numeric_stack";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop - 2)] = NATIVE_KIND_BOOL;
                stackTop--;
                successors.push_back(index + 1);
                break;
            case FASTPATH_NOT:
                if (stackTop < 1 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_BOOL) {
                    if (reason != nullptr) {
                        *reason = "bool_stack";
                    }
                    return false;
                }
                stateKinds[static_cast<std::size_t>(stackTop - 1)] = NATIVE_KIND_BOOL;
                successors.push_back(index + 1);
                break;
            case FASTPATH_POP:
                if (stackTop <= 0) {
                    if (reason != nullptr) {
                        *reason = "stack";
                    }
                    return false;
                }
                stackTop--;
                successors.push_back(index + 1);
                break;
            case FASTPATH_JUMP:
            case FASTPATH_LOOP:
                successors.push_back(instruction.operand);
                break;
            case FASTPATH_JUMP_IF_FALSE:
                if (stackTop < 1 ||
                    stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_BOOL) {
                    if (reason != nullptr) {
                        *reason = "branch_kind";
                    }
                    return false;
                }
                if (instruction.operand >= plan.instructions.size()) {
                    if (reason != nullptr) {
                        *reason = "jump_target";
                    }
                    return false;
                }
                if (index + 1 < plan.instructions.size()) {
                    successors.push_back(index + 1);
                }
                successors.push_back(instruction.operand);
                break;
            case FASTPATH_RETURN: {
                if (stackTop < 1 ||
                    (stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_NUMBER &&
                     stateKinds[static_cast<std::size_t>(stackTop - 1)] != NATIVE_KIND_BOOL)) {
                    if (reason != nullptr) {
                        *reason = "return_shape";
                    }
                    return false;
                }
                NativeValueKind currentReturnKind =
                    stateKinds[static_cast<std::size_t>(stackTop - 1)];
                if (!sawReturn) {
                    returnKind = currentReturnKind;
                    sawReturn = true;
                } else if (returnKind != currentReturnKind) {
                    if (reason != nullptr) {
                        *reason = "return_kind";
                    }
                    return false;
                }
                continue;
            }
            default:
                if (reason != nullptr) {
                    *reason = "unsupported_op";
                }
                return false;
        }

        for (std::size_t successor : successors) {
            if (successor >= plan.instructions.size()) {
                continue;
            }
            if (!pushSuccessor(successor)) {
                return false;
            }
        }
    }

    if (!sawReturn) {
        if (reason != nullptr) {
            *reason = "return_shape";
        }
        return false;
    }

    *returnsBoolean = returnKind == NATIVE_KIND_BOOL;
    return true;
}

} // namespace

bool compileNativeJit_x64(const FunctionPtr& function,
                          const FastPathPlan& plan,
                          NativeJitArtifact* artifact,
                          std::string* reason) {
    if (reason != nullptr) {
        reason->clear();
    }

    if (artifact == nullptr) {
        if (reason != nullptr) {
            *reason = "invalid";
        }
        return false;
    }

    bool returnsBoolean = false;
    if (!isNumericNativeCandidate(function, plan, reason, &returnsBoolean)) {
        return false;
    }

    X64Assembler assembler;
    std::size_t frameSlotCount =
        std::max<std::size_t>(plan.localCount, static_cast<std::size_t>(function->arity)) +
        static_cast<std::size_t>(std::max<uint16_t>(1, plan.maxStack)) + 4;
    int stackBytes = static_cast<int>(frameSlotCount * sizeof(double));
    assembler.emitSubRsp(stackBytes);

    for (int index = 0; index < function->arity; ++index) {
        assembler.emitMovsdXmm0FromArg(index);
        assembler.emitMovsdRspFromXmm0(index);
    }

    std::vector<std::size_t> instructionOffsets(plan.instructions.size());
    std::vector<std::pair<std::size_t, uint16_t>> jumpPatches;

    for (std::size_t index = 0; index < plan.instructions.size(); ++index) {
        instructionOffsets[index] = assembler.getOffset();
        const FastPathInstruction& instruction = plan.instructions[index];
        int stackTop =
            function->arity + static_cast<int>(plan.entryStackDepths[index]);
        switch (instruction.op) {
            case FASTPATH_CONSTANT: {
                const Value& constant = function->chunk.constants.values[instruction.operand];
                if (constant.isBool()) {
                    assembler.emitMovsdXmm0FromConstant(constant.asBool() ? 1.0 : 0.0);
                } else if (constant.isInt()) {
                    assembler.emitMovsdXmm0FromConstant(static_cast<double>(constant.asInt()));
                } else {
                    assembler.emitMovsdXmm0FromConstant(constant.asNumber());
                }
                assembler.emitMovsdRspFromXmm0(stackTop);
                break;
            }
            case FASTPATH_TRUE:
                assembler.emitMovsdXmm0FromConstant(1.0);
                assembler.emitMovsdRspFromXmm0(stackTop);
                break;
            case FASTPATH_FALSE:
                assembler.emitMovsdXmm0FromConstant(0.0);
                assembler.emitMovsdRspFromXmm0(stackTop);
                break;
            case FASTPATH_GET_LOCAL:
                assembler.emitMovsdXmm0FromRsp(static_cast<int>(instruction.operand));
                assembler.emitMovsdRspFromXmm0(stackTop);
                break;
            case FASTPATH_SET_LOCAL:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                assembler.emitMovsdRspFromXmm0(static_cast<int>(instruction.operand));
                break;
            case FASTPATH_ADD:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitAddsdXmm0Xmm1();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_SUBTRACT:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitSubsdXmm0Xmm1();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_MULTIPLY:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitMulsdXmm0Xmm1();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_DIVIDE:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitDivsdXmm0Xmm1();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_NEGATE:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                assembler.emitXorpdXmm1Xmm1();
                assembler.emitSubsdXmm1Xmm0();
                assembler.emitMovsdRspFromXmm1(stackTop - 1);
                break;
            case FASTPATH_NOT:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                assembler.emitXorpdXmm1Xmm1();
                assembler.emitUcomisdXmm0Xmm1();
                assembler.emitZeroEax();
                assembler.emitSeteAl();
                assembler.emitCvtsi2sdXmm0Eax();
                assembler.emitMovsdRspFromXmm0(stackTop - 1);
                break;
            case FASTPATH_EQUAL:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitUcomisdXmm0Xmm1();
                assembler.emitZeroEax();
                assembler.emitSeteAl();
                assembler.emitSetnpDl();
                assembler.emitAndAlDl();
                assembler.emitCvtsi2sdXmm0Eax();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_GREATER:
                assembler.emitMovsdXmm0FromRsp(stackTop - 2);
                assembler.emitMovsdXmm1FromRsp(stackTop - 1);
                assembler.emitUcomisdXmm0Xmm1();
                assembler.emitZeroEax();
                assembler.emitSetaAl();
                assembler.emitCvtsi2sdXmm0Eax();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_LESS:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                assembler.emitMovsdXmm1FromRsp(stackTop - 2);
                assembler.emitUcomisdXmm0Xmm1();
                assembler.emitZeroEax();
                assembler.emitSetaAl();
                assembler.emitCvtsi2sdXmm0Eax();
                assembler.emitMovsdRspFromXmm0(stackTop - 2);
                break;
            case FASTPATH_JUMP:
            case FASTPATH_LOOP:
                assembler.emitJmp32(0);
                jumpPatches.push_back({assembler.getOffset() - 4, instruction.operand});
                break;
            case FASTPATH_JUMP_IF_FALSE:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                assembler.emitXorpdXmm1Xmm1();
                assembler.emitUcomisdXmm0Xmm1();
                assembler.emitJz32(0);
                jumpPatches.push_back({assembler.getOffset() - 4, instruction.operand});
                break;
            case FASTPATH_POP:
                break;
            case FASTPATH_RETURN:
                assembler.emitMovsdXmm0FromRsp(stackTop - 1);
                if (returnsBoolean) {
                    assembler.emitXorpdXmm1Xmm1();
                    assembler.emitUcomisdXmm0Xmm1();
                    std::size_t falsePatch = assembler.emitJzPlaceholder();
                    assembler.emitMovEaxImm32(3);
                    std::size_t endPatch = assembler.emitJmpPlaceholder();
                    std::size_t falseOffset = assembler.getOffset();
                    assembler.emitMovEaxImm32(2);
                    std::size_t endOffset = assembler.getOffset();
                    assembler.patchJump(falsePatch, falseOffset);
                    assembler.patchJump(endPatch, endOffset);
                } else {
                    assembler.emitMovsdResultFromXmm0();
                    assembler.emitMovEaxImm32(1);
                }
                assembler.emitAddRsp(stackBytes);
                assembler.emitReturn();
                break;
            default:
                if (reason != nullptr) {
                    *reason = "unsupported_op";
                }
                return false;
        }
    }

    return assembler.finalize(artifact, instructionOffsets, jumpPatches);
}

#endif
