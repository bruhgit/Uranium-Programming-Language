#include "optimizer.h"
#include "chunk.h"
#include "object.h"
#include <algorithm>
#include <unordered_set>

namespace {

int instructionSize(const Chunk& chunk, int offset) {
    if (offset < 0 || offset >= static_cast<int>(chunk.code.size())) {
        return 1;
    }

    switch (chunk.code[static_cast<std::size_t>(offset)]) {
        case OP_NOP:
        case OP_UNSET:
        case OP_NIL:
        case OP_TRUE:
        case OP_FALSE:
        case OP_INHERIT:
        case OP_POP:
        case OP_GET_INDEX:
        case OP_SET_INDEX:
        case OP_ADD:
        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_NOT:
        case OP_EQUAL:
        case OP_GREATER:
        case OP_LESS:
        case OP_NEGATE:
        case OP_PRINT:
        case OP_RETURN:
        case OP_CLOSE_UPVALUE:
        case OP_POP_EXCEPTION_HANDLER:
        case OP_THROW:
        case OP_AWAIT:
            return 1;
        case OP_CONSTANT:
        case OP_ARRAY:
        case OP_MAP:
        case OP_DEFINE_GLOBAL:
        case OP_DEFINE_CONST_GLOBAL:
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE:
        case OP_GET_PROPERTY:
        case OP_GET_SUPER:
        case OP_SET_PROPERTY:
        case OP_CALL:
        case OP_CLASS:
        case OP_METHOD:
            return 2;
        case OP_CALL_NAMED:
            if (offset + 1 >= static_cast<int>(chunk.code.size())) {
                return 2;
            }
            return 2 + static_cast<int>(chunk.code[static_cast<std::size_t>(offset + 1)]) * 2;
        case OP_CONSTANT_LONG:
        case OP_DEFINE_GLOBAL_LONG:
        case OP_DEFINE_CONST_GLOBAL_LONG:
        case OP_GET_GLOBAL_LONG:
        case OP_SET_GLOBAL_LONG:
        case OP_GET_PROPERTY_LONG:
        case OP_GET_SUPER_LONG:
        case OP_SET_PROPERTY_LONG:
        case OP_CLASS_LONG:
        case OP_METHOD_LONG:
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_LOOP:
        case OP_PUSH_EXCEPTION_HANDLER:
            return 3;
        case OP_CLOSURE: {
            if (offset + 1 >= static_cast<int>(chunk.code.size())) {
                return 2;
            }
            int constantIndex = chunk.code[static_cast<std::size_t>(offset + 1)];
            int upvalueCount = 0;
            if (constantIndex >= 0 &&
                constantIndex < static_cast<int>(chunk.constants.values.size())) {
                const Value& constant = chunk.constants.values[static_cast<std::size_t>(constantIndex)];
                if (constant.isFunction() && constant.asFunction() != nullptr) {
                    upvalueCount = constant.asFunction()->upvalueCount;
                }
            }
            return 2 + upvalueCount * 2;
        }
        case OP_CLOSURE_LONG: {
            if (offset + 2 >= static_cast<int>(chunk.code.size())) {
                return 3;
            }
            int constantIndex =
                static_cast<int>((chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                                 chunk.code[static_cast<std::size_t>(offset + 2)]);
            int upvalueCount = 0;
            if (constantIndex >= 0 &&
                constantIndex < static_cast<int>(chunk.constants.values.size())) {
                const Value& constant = chunk.constants.values[static_cast<std::size_t>(constantIndex)];
                if (constant.isFunction() && constant.asFunction() != nullptr) {
                    upvalueCount = constant.asFunction()->upvalueCount;
                }
            }
            return 3 + upvalueCount * 2;
        }
        default:
            return 1;
    }
}

int nextMeaningfulOffset(const Chunk& chunk, int offset) {
    while (offset < static_cast<int>(chunk.code.size()) &&
           chunk.code[static_cast<std::size_t>(offset)] == OP_NOP) {
        offset++;
    }
    return offset;
}

void collectProtectedOffsets(const Chunk& chunk, std::unordered_set<int>* protectedOffsets) {
    protectedOffsets->clear();
    protectedOffsets->insert(0);

    for (int offset = 0; offset < static_cast<int>(chunk.code.size());) {
        uint8_t opcode = chunk.code[static_cast<std::size_t>(offset)];
        if (opcode == OP_JUMP || opcode == OP_JUMP_IF_FALSE || opcode == OP_LOOP) {
            if (offset + 2 < static_cast<int>(chunk.code.size())) {
                int jump =
                    static_cast<int>((chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                                     chunk.code[static_cast<std::size_t>(offset + 2)]);
                int target = offset + 3;
                if (opcode == OP_LOOP) {
                    target -= jump;
                } else {
                    target += jump;
                }
                if (target >= 0 && target < static_cast<int>(chunk.code.size())) {
                    protectedOffsets->insert(target);
                }
            }
        } else if (opcode == OP_PUSH_EXCEPTION_HANDLER) {
            if (offset + 2 < static_cast<int>(chunk.code.size())) {
                int target =
                    static_cast<int>((chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                                     chunk.code[static_cast<std::size_t>(offset + 2)]);
                if (target >= 0 && target < static_cast<int>(chunk.code.size())) {
                    protectedOffsets->insert(target);
                }
            }
        }

        offset += instructionSize(chunk, offset);
    }
}

bool decodeConstantValue(const Chunk& chunk, int offset, Value* value, int* size) {
    if (offset < 0 || offset >= static_cast<int>(chunk.code.size())) {
        return false;
    }

    uint8_t opcode = chunk.code[static_cast<std::size_t>(offset)];
    switch (opcode) {
        case OP_NIL:
            *value = Value::nilValue();
            *size = 1;
            return true;
        case OP_TRUE:
            *value = Value::boolValue(true);
            *size = 1;
            return true;
        case OP_FALSE:
            *value = Value::boolValue(false);
            *size = 1;
            return true;
        case OP_CONSTANT: {
            if (offset + 1 >= static_cast<int>(chunk.code.size())) {
                return false;
            }
            int constantIndex = chunk.code[static_cast<std::size_t>(offset + 1)];
            if (constantIndex < 0 ||
                constantIndex >= static_cast<int>(chunk.constants.values.size())) {
                return false;
            }
            *value = chunk.constants.values[static_cast<std::size_t>(constantIndex)];
            *size = 2;
            return true;
        }
        case OP_CONSTANT_LONG: {
            if (offset + 2 >= static_cast<int>(chunk.code.size())) {
                return false;
            }
            int constantIndex =
                static_cast<int>((chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                                 chunk.code[static_cast<std::size_t>(offset + 2)]);
            if (constantIndex < 0 ||
                constantIndex >= static_cast<int>(chunk.constants.values.size())) {
                return false;
            }
            *value = chunk.constants.values[static_cast<std::size_t>(constantIndex)];
            *size = 3;
            return true;
        }
        default:
            return false;
    }
}

void writeNops(Chunk* chunk, int start, int end) {
    for (int offset = start; offset < end; ++offset) {
        chunk->code[static_cast<std::size_t>(offset)] = OP_NOP;
    }
}

bool writeValueAsConstantLike(Chunk* chunk, int start, int capacity, const Value& value) {
    if (capacity <= 0) {
        return false;
    }

    auto fillTrailingNops = [&](int usedBytes) {
        for (int offset = start + usedBytes; offset < start + capacity; ++offset) {
            chunk->code[static_cast<std::size_t>(offset)] = OP_NOP;
        }
    };

    if (value.isNil()) {
        chunk->code[static_cast<std::size_t>(start)] = OP_NIL;
        fillTrailingNops(1);
        return true;
    }

    if (value.isBool()) {
        chunk->code[static_cast<std::size_t>(start)] = value.asBool() ? OP_TRUE : OP_FALSE;
        fillTrailingNops(1);
        return true;
    }

    int constantIndex = chunk->addConstant(value);
    if (constantIndex <= UINT8_MAX) {
        if (capacity < 2) {
            return false;
        }
        chunk->code[static_cast<std::size_t>(start)] = OP_CONSTANT;
        chunk->code[static_cast<std::size_t>(start + 1)] =
            static_cast<uint8_t>(constantIndex);
        fillTrailingNops(2);
        return true;
    }

    if (constantIndex > UINT16_MAX || capacity < 3) {
        return false;
    }

    chunk->code[static_cast<std::size_t>(start)] = OP_CONSTANT_LONG;
    chunk->code[static_cast<std::size_t>(start + 1)] =
        static_cast<uint8_t>((constantIndex >> 8) & 0xff);
    chunk->code[static_cast<std::size_t>(start + 2)] =
        static_cast<uint8_t>(constantIndex & 0xff);
    fillTrailingNops(3);
    return true;
}

bool evalUnaryFold(uint8_t opcode, const Value& input, Value* result) {
    if (opcode == OP_NOT) {
        *result = Value::boolValue(isFalsey(input));
        return true;
    }

    if (opcode == OP_NEGATE && input.isNumber()) {
        *result = Value::numberValue(-input.asNumber());
        return true;
    }

    return false;
}

bool evalBinaryFold(uint8_t opcode, const Value& left, const Value& right, Value* result) {
    switch (opcode) {
        case OP_ADD:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::numberValue(left.asNumber() + right.asNumber());
                return true;
            }
            if (left.isString() && right.isString()) {
                *result = Value::stringValue(left.asString() + right.asString());
                return true;
            }
            return false;
        case OP_SUBTRACT:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::numberValue(left.asNumber() - right.asNumber());
                return true;
            }
            return false;
        case OP_MULTIPLY:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::numberValue(left.asNumber() * right.asNumber());
                return true;
            }
            return false;
        case OP_DIVIDE:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::numberValue(left.asNumber() / right.asNumber());
                return true;
            }
            return false;
        case OP_EQUAL:
            *result = Value::boolValue(valuesEqual(left, right));
            return true;
        case OP_GREATER:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::boolValue(left.asNumber() > right.asNumber());
                return true;
            }
            return false;
        case OP_LESS:
            if (left.isNumber() && right.isNumber()) {
                *result = Value::boolValue(left.asNumber() < right.asNumber());
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool optimizeChunk(Chunk* chunk) {
    if (chunk == nullptr || chunk->code.empty()) {
        return false;
    }

    bool changed = false;
    bool passChanged = false;
    do {
        passChanged = false;
        std::unordered_set<int> protectedOffsets;
        collectProtectedOffsets(*chunk, &protectedOffsets);

        for (int offset = 0; offset < static_cast<int>(chunk->code.size());) {
            int size = instructionSize(*chunk, offset);
            uint8_t opcode = chunk->code[static_cast<std::size_t>(offset)];

            Value firstValue;
            int firstSize = 0;
            if (decodeConstantValue(*chunk, offset, &firstValue, &firstSize)) {
                int secondOffset = nextMeaningfulOffset(*chunk, offset + firstSize);
                if (secondOffset < static_cast<int>(chunk->code.size())) {
                    if (chunk->code[static_cast<std::size_t>(secondOffset)] == OP_POP &&
                        protectedOffsets.find(secondOffset) == protectedOffsets.end()) {
                        writeNops(chunk, offset, secondOffset + 1);
                        passChanged = true;
                        changed = true;
                        offset += firstSize;
                        continue;
                    }

                    Value foldedUnary;
                    if (evalUnaryFold(chunk->code[static_cast<std::size_t>(secondOffset)],
                                      firstValue, &foldedUnary) &&
                        protectedOffsets.find(secondOffset) == protectedOffsets.end()) {
                        int end = secondOffset + instructionSize(*chunk, secondOffset);
                        if (writeValueAsConstantLike(chunk, offset, end - offset, foldedUnary)) {
                            passChanged = true;
                            changed = true;
                            offset += firstSize;
                            continue;
                        }
                    }

                    Value secondValue;
                    int secondSize = 0;
                    int thirdOffset = nextMeaningfulOffset(*chunk, secondOffset);
                    if (decodeConstantValue(*chunk, secondOffset, &secondValue, &secondSize)) {
                        thirdOffset = nextMeaningfulOffset(*chunk, secondOffset + secondSize);
                        if (thirdOffset < static_cast<int>(chunk->code.size()) &&
                            protectedOffsets.find(secondOffset) == protectedOffsets.end() &&
                            protectedOffsets.find(thirdOffset) == protectedOffsets.end()) {
                            Value foldedBinary;
                            if (evalBinaryFold(
                                    chunk->code[static_cast<std::size_t>(thirdOffset)],
                                    firstValue, secondValue, &foldedBinary)) {
                                int end = thirdOffset + instructionSize(*chunk, thirdOffset);
                                if (writeValueAsConstantLike(chunk, offset, end - offset,
                                                             foldedBinary)) {
                                    passChanged = true;
                                    changed = true;
                                    offset += firstSize;
                                    continue;
                                }
                            }
                        }
                    }
                }
            }

            offset += std::max(1, size);
            if (opcode == OP_NOP) {
                offset = std::max(offset, nextMeaningfulOffset(*chunk, offset));
            }
        }
    } while (passChanged);

    return changed;
}

bool optimizeChunkO2(Chunk* chunk) {
    bool changed = false;
    bool passChanged;
    do {
        passChanged = false;
        for (int offset = 0; offset < static_cast<int>(chunk->code.size());) {
            uint8_t opcode = chunk->code[offset];
            int size = instructionSize(*chunk, offset);
            
            // 1. Jump Threading
            if (opcode == OP_JUMP) {
                int jump = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
                int target = offset + 3 + jump;
                int currentTarget = target;
                bool threaded = false;
                std::unordered_set<int> visited;
                while (currentTarget >= 0 && currentTarget < static_cast<int>(chunk->code.size()) && 
                       chunk->code[currentTarget] == OP_JUMP) {
                    if (visited.count(currentTarget)) break;
                    visited.insert(currentTarget);
                    int nextJump = (chunk->code[currentTarget + 1] << 8) | chunk->code[currentTarget + 2];
                    currentTarget = currentTarget + 3 + nextJump;
                    threaded = true;
                }
                if (threaded && currentTarget != target) {
                    int newJump = currentTarget - (offset + 3);
                    if (newJump >= 0 && newJump <= 0xffff) {
                        chunk->code[offset + 1] = (newJump >> 8) & 0xff;
                        chunk->code[offset + 2] = newJump & 0xff;
                        passChanged = true;
                        changed = true;
                    }
                }
            }
            
            // 2. Branch-on-Constant (BOC) Elimination
            if (opcode == OP_TRUE || opcode == OP_FALSE) {
                int nextOffset = offset + size;
                if (nextOffset < static_cast<int>(chunk->code.size()) && chunk->code[nextOffset] == OP_JUMP_IF_FALSE) {
                    if (opcode == OP_TRUE) {
                        chunk->code[offset] = OP_NOP;
                        chunk->code[nextOffset] = OP_NOP;
                        chunk->code[nextOffset + 1] = OP_NOP;
                        chunk->code[nextOffset + 2] = OP_NOP;
                    } else {
                        chunk->code[offset] = OP_NOP;
                        chunk->code[nextOffset] = OP_JUMP;
                    }
                    passChanged = true;
                    changed = true;
                }
            }
            
            offset += std::max(1, size);
        }
    } while (passChanged);
    return changed;
}

void compactNops(Chunk* chunk) {
    std::vector<int> oldToNew(chunk->code.size(), -1);
    std::vector<uint8_t> newCode;
    std::vector<int> newLines;
    
    int newOffset = 0;
    for (int offset = 0; offset < static_cast<int>(chunk->code.size());) {
        uint8_t opcode = chunk->code[offset];
        int size = instructionSize(*chunk, offset);
        if (opcode == OP_NOP) {
            for (int i = 0; i < size; ++i) {
                oldToNew[offset + i] = -1;
            }
        } else {
            for (int i = 0; i < size; ++i) {
                oldToNew[offset + i] = newOffset + i;
            }
            for (int i = 0; i < size; ++i) {
                newCode.push_back(chunk->code[offset + i]);
                newLines.push_back(chunk->lines[offset + i]);
            }
            newOffset += size;
        }
        offset += std::max(1, size);
    }
    
    for (int offset = 0; offset < static_cast<int>(newCode.size());) {
        uint8_t opcode = newCode[offset];
        int size = instructionSize(*chunk, offset);
        if (opcode == OP_JUMP || opcode == OP_JUMP_IF_FALSE || opcode == OP_LOOP) {
            int oldOffset = -1;
            for (int i = 0; i < static_cast<int>(oldToNew.size()); ++i) {
                if (oldToNew[i] == offset) {
                    oldOffset = i;
                    break;
                }
            }
            
            if (oldOffset != -1) {
                int jump = (chunk->code[oldOffset + 1] << 8) | chunk->code[oldOffset + 2];
                int oldTarget = oldOffset + 3;
                if (opcode == OP_LOOP) {
                    oldTarget -= jump;
                } else {
                    oldTarget += jump;
                }
                
                if (oldTarget >= 0 && oldTarget < static_cast<int>(oldToNew.size())) {
                    int newTarget = oldToNew[oldTarget];
                    if (newTarget != -1) {
                        int newJump = 0;
                        if (opcode == OP_LOOP) {
                            newJump = (offset + 3) - newTarget;
                        } else {
                            newJump = newTarget - (offset + 3);
                        }
                        newCode[offset + 1] = (newJump >> 8) & 0xff;
                        newCode[offset + 2] = newJump & 0xff;
                    }
                }
            }
        }
        offset += std::max(1, size);
    }
    
    chunk->code = newCode;
    chunk->lines = newLines;
}

bool optimizeChunkO3(Chunk* chunk) {
    bool changed = false;
    for (int offset = 0; offset < static_cast<int>(chunk->code.size());) {
        uint8_t opcode = chunk->code[offset];
        int size = instructionSize(*chunk, offset);
        
        // Strength Reduction: Division by constant -> Multiplication by reciprocal
        if (opcode == OP_CONSTANT) {
            int constIndex = chunk->code[offset + 1];
            Value val = chunk->constants.values[constIndex];
            int nextOffset = offset + size;
            if (nextOffset < static_cast<int>(chunk->code.size()) && val.isNumber()) {
                uint8_t nextOp = chunk->code[nextOffset];
                if (nextOp == OP_DIVIDE && val.asNumber() != 0.0) {
                    chunk->constants.values[constIndex] = Value::numberValue(1.0 / val.asNumber());
                    chunk->code[nextOffset] = OP_MULTIPLY;
                    changed = true;
                }
            }
        }
        offset += std::max(1, size);
    }
    return changed;
}

void optimizeNestedFunctions(const FunctionPtr& function, int level) {
    if (function == nullptr || function->optimized) {
        return;
    }

    for (const Value& constant : function->chunk.constants.values) {
        if (constant.isFunction() && constant.asFunction() != nullptr) {
            optimizeFunctionTree(constant.asFunction(), level);
        }
    }
}

} // namespace

void optimizeFunctionTree(const FunctionPtr& function, int level) {
    if (function == nullptr || function->optimized) {
        return;
    }

    optimizeNestedFunctions(function, level);
    
    if (level >= 1) {
        optimizeChunk(&function->chunk);
    }
    if (level >= 2) {
        optimizeChunkO2(&function->chunk);
    }
    if (level >= 3) {
        optimizeChunkO3(&function->chunk);
        compactNops(&function->chunk);
    }
    
    function->optimized = true;
}

bool buildFastPathPlan(const FunctionPtr& function,
                       FastPathPlan* plan,
                       std::string* reason) {
    if (reason != nullptr) {
        reason->clear();
    }

    if (function == nullptr || plan == nullptr) {
        if (reason != nullptr) {
            *reason = "invalid";
        }
        return false;
    }

    if (function->isAsync) {
        if (reason != nullptr) {
            *reason = "async";
        }
        return false;
    }

    if (function->upvalueCount != 0) {
        if (reason != nullptr) {
            *reason = "upvalues";
        }
        return false;
    }

    plan->instructions.clear();
    plan->entryStackDepths.clear();
    plan->localCount = static_cast<uint16_t>(std::max(0, function->arity));
    plan->maxStack = 0;

    const Chunk& chunk = function->chunk;
    std::vector<int> reachableOffsets;
    reachableOffsets.reserve(chunk.code.size());
    std::unordered_map<int, int> allMeaningfulOffsets;

    for (int offset = 0; offset < static_cast<int>(chunk.code.size());) {
        int size = instructionSize(chunk, offset);
        if (chunk.code[static_cast<std::size_t>(offset)] != OP_NOP) {
            allMeaningfulOffsets[offset] = static_cast<int>(reachableOffsets.size());
            reachableOffsets.push_back(offset);
        }
        offset += size;
    }

    int startOffset = nextMeaningfulOffset(chunk, 0);
    if (startOffset >= static_cast<int>(chunk.code.size())) {
        if (reason != nullptr) {
            *reason = "no_return";
        }
        return false;
    }

    std::unordered_map<int, int> entryDepths;
    std::vector<int> worklist;
    worklist.push_back(startOffset);
    entryDepths[startOffset] = 0;

    bool sawReturn = false;
    int maxStackDepth = 0;

    auto pushSuccessor = [&](int rawTarget, int successorDepth) -> bool {
        if (successorDepth < 0) {
            if (reason != nullptr) {
                *reason = "stack_underflow";
            }
            return false;
        }
        if (successorDepth > 256) {
            if (reason != nullptr) {
                *reason = "stack_limit";
            }
            return false;
        }

        int target = nextMeaningfulOffset(chunk, rawTarget);
        if (target >= static_cast<int>(chunk.code.size())) {
            return true;
        }

        if (allMeaningfulOffsets.find(target) == allMeaningfulOffsets.end()) {
            if (reason != nullptr) {
                *reason = "invalid_jump_target";
            }
            return false;
        }

        auto existing = entryDepths.find(target);
        if (existing == entryDepths.end()) {
            entryDepths[target] = successorDepth;
            worklist.push_back(target);
            return true;
        }

        if (existing->second != successorDepth) {
            if (reason != nullptr) {
                *reason = "stack_merge";
            }
            return false;
        }

        return true;
    };

    while (!worklist.empty()) {
        int offset = worklist.back();
        worklist.pop_back();

        uint8_t opcode = chunk.code[static_cast<std::size_t>(offset)];
        int size = instructionSize(chunk, offset);
        int entryDepth = entryDepths[offset];
        int exitDepth = entryDepth;

        maxStackDepth = std::max(maxStackDepth, entryDepth);

        switch (opcode) {
            case OP_CONSTANT:
            case OP_CONSTANT_LONG:
            case OP_NIL:
            case OP_TRUE:
            case OP_FALSE:
            case OP_GET_LOCAL:
                if (opcode == OP_GET_LOCAL) {
                    uint16_t slot = chunk.code[static_cast<std::size_t>(offset + 1)];
                    plan->localCount = std::max<uint16_t>(
                        plan->localCount, static_cast<uint16_t>(slot + 1));
                }
                exitDepth = entryDepth + 1;
                maxStackDepth = std::max(maxStackDepth, exitDepth);
                if (!pushSuccessor(offset + size, exitDepth)) {
                    return false;
                }
                break;
            case OP_SET_LOCAL: {
                uint16_t slot = chunk.code[static_cast<std::size_t>(offset + 1)];
                plan->localCount = std::max<uint16_t>(
                    plan->localCount, static_cast<uint16_t>(slot + 1));
                if (entryDepth < 1) {
                    if (reason != nullptr) {
                        *reason = "stack_underflow";
                    }
                    return false;
                }
                if (!pushSuccessor(offset + size, entryDepth)) {
                    return false;
                }
                break;
            }
            case OP_ADD:
            case OP_SUBTRACT:
            case OP_MULTIPLY:
            case OP_DIVIDE:
            case OP_EQUAL:
            case OP_GREATER:
            case OP_LESS:
                if (entryDepth < 2) {
                    if (reason != nullptr) {
                        *reason = "stack_underflow";
                    }
                    return false;
                }
                exitDepth = entryDepth - 1;
                if (!pushSuccessor(offset + size, exitDepth)) {
                    return false;
                }
                break;
            case OP_NOT:
            case OP_NEGATE:
                if (entryDepth < 1) {
                    if (reason != nullptr) {
                        *reason = "stack_underflow";
                    }
                    return false;
                }
                if (!pushSuccessor(offset + size, entryDepth)) {
                    return false;
                }
                break;
            case OP_POP:
                if (entryDepth < 1) {
                    if (reason != nullptr) {
                        *reason = "stack_underflow";
                    }
                    return false;
                }
                exitDepth = entryDepth - 1;
                if (!pushSuccessor(offset + size, exitDepth)) {
                    return false;
                }
                break;
            case OP_JUMP: {
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                if (!pushSuccessor(offset + 3 + jump, entryDepth)) {
                    return false;
                }
                break;
            }
            case OP_JUMP_IF_FALSE: {
                if (entryDepth < 1) {
                    if (reason != nullptr) {
                        *reason = "stack_underflow";
                    }
                    return false;
                }
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                if (!pushSuccessor(offset + size, entryDepth) ||
                    !pushSuccessor(offset + 3 + jump, entryDepth)) {
                    return false;
                }
                break;
            }
            case OP_LOOP: {
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                if (!pushSuccessor(offset + 3 - jump, entryDepth)) {
                    return false;
                }
                break;
            }
            case OP_RETURN:
                if (entryDepth < 1) {
                    if (reason != nullptr) {
                        *reason = "missing_return_value";
                    }
                    return false;
                }
                sawReturn = true;
                break;
            default:
                if (reason != nullptr) {
                    *reason = "unsupported_opcode";
                }
                return false;
        }
    }

    if (!sawReturn) {
        if (reason != nullptr) {
            *reason = "no_return";
        }
        return false;
    }

    std::unordered_map<int, int> bytecodeToInstIndex;
    auto pushInstruction = [&](FastPathOp op, uint16_t operand = 0) {
        plan->instructions.emplace_back(op, operand);
    };

    for (int offset : reachableOffsets) {
        auto depthIt = entryDepths.find(offset);
        if (depthIt == entryDepths.end()) {
            continue;
        }

        bytecodeToInstIndex[offset] = static_cast<int>(plan->instructions.size());
        plan->entryStackDepths.push_back(static_cast<uint16_t>(depthIt->second));

        uint8_t opcode = chunk.code[static_cast<std::size_t>(offset)];
        switch (opcode) {
            case OP_CONSTANT: {
                int constantIndex = chunk.code[static_cast<std::size_t>(offset + 1)];
                pushInstruction(FASTPATH_CONSTANT, static_cast<uint16_t>(constantIndex));
                break;
            }
            case OP_CONSTANT_LONG: {
                int constantIndex =
                    static_cast<int>((chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                                     chunk.code[static_cast<std::size_t>(offset + 2)]);
                pushInstruction(FASTPATH_CONSTANT, static_cast<uint16_t>(constantIndex));
                break;
            }
            case OP_NIL:
                pushInstruction(FASTPATH_NIL);
                break;
            case OP_TRUE:
                pushInstruction(FASTPATH_TRUE);
                break;
            case OP_FALSE:
                pushInstruction(FASTPATH_FALSE);
                break;
            case OP_GET_LOCAL:
                pushInstruction(
                    FASTPATH_GET_LOCAL,
                    static_cast<uint16_t>(chunk.code[static_cast<std::size_t>(offset + 1)]));
                break;
            case OP_SET_LOCAL:
                pushInstruction(
                    FASTPATH_SET_LOCAL,
                    static_cast<uint16_t>(chunk.code[static_cast<std::size_t>(offset + 1)]));
                break;
            case OP_ADD:
                pushInstruction(FASTPATH_ADD);
                break;
            case OP_SUBTRACT:
                pushInstruction(FASTPATH_SUBTRACT);
                break;
            case OP_MULTIPLY:
                pushInstruction(FASTPATH_MULTIPLY);
                break;
            case OP_DIVIDE:
                pushInstruction(FASTPATH_DIVIDE);
                break;
            case OP_NOT:
                pushInstruction(FASTPATH_NOT);
                break;
            case OP_EQUAL:
                pushInstruction(FASTPATH_EQUAL);
                break;
            case OP_GREATER:
                pushInstruction(FASTPATH_GREATER);
                break;
            case OP_LESS:
                pushInstruction(FASTPATH_LESS);
                break;
            case OP_JUMP: {
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                pushInstruction(FASTPATH_JUMP, static_cast<uint16_t>(offset + 3 + jump));
                break;
            }
            case OP_JUMP_IF_FALSE: {
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                pushInstruction(FASTPATH_JUMP_IF_FALSE,
                                static_cast<uint16_t>(offset + 3 + jump));
                break;
            }
            case OP_LOOP: {
                int jump = static_cast<int>(
                    (chunk.code[static_cast<std::size_t>(offset + 1)] << 8) |
                    chunk.code[static_cast<std::size_t>(offset + 2)]);
                pushInstruction(FASTPATH_LOOP, static_cast<uint16_t>(offset + 3 - jump));
                break;
            }
            case OP_NEGATE:
                pushInstruction(FASTPATH_NEGATE);
                break;
            case OP_POP:
                pushInstruction(FASTPATH_POP);
                break;
            case OP_RETURN:
                pushInstruction(FASTPATH_RETURN);
                break;
            default:
                if (reason != nullptr) {
                    *reason = "unsupported_opcode";
                }
                return false;
        }
    }

    plan->maxStack = static_cast<uint16_t>(std::max(1, maxStackDepth));

    if (plan->entryStackDepths.size() != plan->instructions.size()) {
        if (reason != nullptr) {
            *reason = "plan_shape";
        }
        return false;
    }

    for (auto& inst : plan->instructions) {
        if (inst.op == FASTPATH_JUMP || inst.op == FASTPATH_JUMP_IF_FALSE || inst.op == FASTPATH_LOOP) {
            int target = inst.operand;
            target = nextMeaningfulOffset(chunk, target);
            if (bytecodeToInstIndex.find(target) == bytecodeToInstIndex.end()) {
                if (reason != nullptr) *reason = "unresolved_jump_target";
                return false;
            }
            inst.operand = static_cast<uint16_t>(bytecodeToInstIndex[target]);
        }
    }

    return true;
}
