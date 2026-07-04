#include "chunk.h"
#include "object.h"
#include <iomanip>
#include <iostream>
#include <ostream>

void Chunk::writeChunk(uint8_t byte, int line) {
    code.push_back(byte);
    lines.push_back(line);
}

int Chunk::addConstant(Value value) {
    constants.write(value);
    return static_cast<int>(constants.values.size() - 1);
}

static int simpleInstruction(std::ostream& stream, const char* name, int offset) {
    stream << name << std::endl;
    return offset + 1;
}

static int byteInstruction(std::ostream& stream, const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    stream << std::left << std::setw(16) << name << " "
           << std::right << std::setw(4) << static_cast<int>(slot) << std::endl;
    return offset + 2;
}

static int namedCallInstruction(std::ostream& stream, Chunk* chunk, int offset) {
    uint8_t argCount = chunk->code[offset + 1];
    stream << std::left << std::setw(16) << "OP_CALL_NAMED" << " "
           << std::right << std::setw(4) << static_cast<int>(argCount);

    int cursor = offset + 2;
    for (uint8_t index = 0; index < argCount; ++index) {
        uint16_t encoded =
            static_cast<uint16_t>((chunk->code[cursor] << 8) | chunk->code[cursor + 1]);
        cursor += 2;
        if (encoded == 0) {
            stream << " _";
            continue;
        }

        std::size_t constantIndex = static_cast<std::size_t>(encoded - 1);
        stream << " ";
        if (constantIndex < chunk->constants.values.size()) {
            printValue(chunk->constants.values[constantIndex]);
        } else {
            stream << "<invalid>";
        }
    }

    stream << std::endl;
    return cursor;
}

static int jumpInstruction(std::ostream& stream,
                           const char* name,
                           int sign,
                           Chunk* chunk,
                           int offset) {
    uint16_t jump =
        static_cast<uint16_t>((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    stream << std::left << std::setw(16) << name << " "
           << std::right << std::setw(4) << offset << " -> "
           << (offset + 3 + sign * jump) << std::endl;
    return offset + 3;
}

static int absoluteInstruction(std::ostream& stream, const char* name, Chunk* chunk, int offset) {
    uint16_t target =
        static_cast<uint16_t>((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    stream << std::left << std::setw(24) << name << " "
           << std::right << std::setw(4) << target << std::endl;
    return offset + 3;
}

static int constantInstruction(std::ostream& stream,
                               const char* name,
                               Chunk* chunk,
                               int offset) {
    uint8_t constant = chunk->code[offset + 1];
    stream << std::left << std::setw(16) << name << " "
           << std::right << std::setw(4) << static_cast<int>(constant) << " '";
    printValue(chunk->constants.values[constant]);
    stream << "'" << std::endl;
    return offset + 2;
}

static int constantLongInstruction(std::ostream& stream,
                                   const char* name,
                                   Chunk* chunk,
                                   int offset) {
    uint16_t constant =
        static_cast<uint16_t>((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    stream << std::left << std::setw(16) << name << " "
           << std::right << std::setw(6) << constant << " '";
    printValue(chunk->constants.values[constant]);
    stream << "'" << std::endl;
    return offset + 3;
}

static int closureInstruction(std::ostream& stream,
                              const char* name,
                              Chunk* chunk,
                              int offset,
                              bool isLong) {
    int constantIndex = 0;
    int nextOffset = 0;

    if (isLong) {
        constantIndex =
            static_cast<int>((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
        nextOffset = offset + 3;
    } else {
        constantIndex = chunk->code[offset + 1];
        nextOffset = offset + 2;
    }

    stream << std::left << std::setw(16) << name << " "
           << std::right << std::setw(isLong ? 6 : 4) << constantIndex << " '";
    printValue(chunk->constants.values[static_cast<std::size_t>(constantIndex)]);
    stream << "'" << std::endl;

    int upvalueCount = 0;
    const Value& constant = chunk->constants.values[static_cast<std::size_t>(constantIndex)];
    if (constant.isFunction() && constant.asFunction() != nullptr) {
        upvalueCount = constant.asFunction()->upvalueCount;
    }

    for (int index = 0; index < upvalueCount; ++index) {
        uint8_t isLocal = chunk->code[nextOffset++];
        uint8_t slot = chunk->code[nextOffset++];
        stream << std::setfill(' ') << std::setw(4) << chunk->lines[offset] << "      | "
               << std::left << std::setw(14)
               << (isLocal == 0 ? "upvalue" : "local")
               << std::right << std::setw(4) << static_cast<int>(slot) << std::endl;
    }

    return nextOffset;
}

void disassembleChunk(Chunk* chunk, const char* name) {
    disassembleChunkToStream(std::cout, chunk, name);
}

int disassembleInstruction(Chunk* chunk, int offset) {
    return disassembleInstructionToStream(std::cout, chunk, offset);
}

void disassembleChunkToStream(std::ostream& stream, Chunk* chunk, const char* name) {
    stream << "== " << name << " ==" << std::endl;
    for (int offset = 0; offset < static_cast<int>(chunk->code.size());) {
        offset = disassembleInstructionToStream(stream, chunk, offset);
    }
}

int disassembleInstructionToStream(std::ostream& stream, Chunk* chunk, int offset) {
    stream << std::setfill('0') << std::setw(4) << offset << " ";
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        stream << "   | ";
    } else {
        stream << std::setfill(' ') << std::setw(4) << chunk->lines[offset] << " ";
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_NOP:
            return simpleInstruction(stream, "OP_NOP", offset);
        case OP_CONSTANT:
            return constantInstruction(stream, "OP_CONSTANT", chunk, offset);
        case OP_CONSTANT_LONG:
            return constantLongInstruction(stream, "OP_CONSTANT_LONG", chunk, offset);
        case OP_UNSET:
            return simpleInstruction(stream, "OP_UNSET", offset);
        case OP_NIL:
            return simpleInstruction(stream, "OP_NIL", offset);
        case OP_TRUE:
            return simpleInstruction(stream, "OP_TRUE", offset);
        case OP_FALSE:
            return simpleInstruction(stream, "OP_FALSE", offset);
        case OP_ARRAY:
            return byteInstruction(stream, "OP_ARRAY", chunk, offset);
        case OP_MAP:
            return byteInstruction(stream, "OP_MAP", chunk, offset);
        case OP_CLASS:
            return constantInstruction(stream, "OP_CLASS", chunk, offset);
        case OP_CLASS_LONG:
            return constantLongInstruction(stream, "OP_CLASS_LONG", chunk, offset);
        case OP_INHERIT:
            return simpleInstruction(stream, "OP_INHERIT", offset);
        case OP_METHOD:
            return constantInstruction(stream, "OP_METHOD", chunk, offset);
        case OP_METHOD_LONG:
            return constantLongInstruction(stream, "OP_METHOD_LONG", chunk, offset);
        case OP_CLOSURE:
            return closureInstruction(stream, "OP_CLOSURE", chunk, offset, false);
        case OP_CLOSURE_LONG:
            return closureInstruction(stream, "OP_CLOSURE_LONG", chunk, offset, true);
        case OP_POP:
            return simpleInstruction(stream, "OP_POP", offset);
        case OP_CLOSE_UPVALUE:
            return simpleInstruction(stream, "OP_CLOSE_UPVALUE", offset);
        case OP_DEFINE_GLOBAL:
            return constantInstruction(stream, "OP_DEFINE_GLOBAL", chunk, offset);
        case OP_DEFINE_CONST_GLOBAL:
            return constantInstruction(stream, "OP_DEFINE_CONST_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL_LONG:
            return constantLongInstruction(stream, "OP_DEFINE_GLOBAL_LONG", chunk, offset);
        case OP_DEFINE_CONST_GLOBAL_LONG:
            return constantLongInstruction(stream, "OP_DEFINE_CONST_GLOBAL_LONG", chunk, offset);
        case OP_GET_GLOBAL:
            return constantInstruction(stream, "OP_GET_GLOBAL", chunk, offset);
        case OP_GET_GLOBAL_LONG:
            return constantLongInstruction(stream, "OP_GET_GLOBAL_LONG", chunk, offset);
        case OP_SET_GLOBAL:
            return constantInstruction(stream, "OP_SET_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL_LONG:
            return constantLongInstruction(stream, "OP_SET_GLOBAL_LONG", chunk, offset);
        case OP_GET_LOCAL:
            return byteInstruction(stream, "OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:
            return byteInstruction(stream, "OP_SET_LOCAL", chunk, offset);
        case OP_GET_UPVALUE:
            return byteInstruction(stream, "OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:
            return byteInstruction(stream, "OP_SET_UPVALUE", chunk, offset);
        case OP_PUSH_EXCEPTION_HANDLER:
            return absoluteInstruction(stream, "OP_PUSH_EXCEPTION_HANDLER", chunk, offset);
        case OP_POP_EXCEPTION_HANDLER:
            return simpleInstruction(stream, "OP_POP_EXCEPTION_HANDLER", offset);
        case OP_THROW:
            return simpleInstruction(stream, "OP_THROW", offset);
        case OP_AWAIT:
            return simpleInstruction(stream, "OP_AWAIT", offset);
        case OP_BREAKPOINT:
            return simpleInstruction(stream, "OP_BREAKPOINT", offset);
        case OP_GET_INDEX:
            return simpleInstruction(stream, "OP_GET_INDEX", offset);
        case OP_SET_INDEX:
            return simpleInstruction(stream, "OP_SET_INDEX", offset);
        case OP_GET_PROPERTY:
            return constantInstruction(stream, "OP_GET_PROPERTY", chunk, offset);
        case OP_GET_PROPERTY_LONG:
            return constantLongInstruction(stream, "OP_GET_PROPERTY_LONG", chunk, offset);
        case OP_GET_SUPER:
            return constantInstruction(stream, "OP_GET_SUPER", chunk, offset);
        case OP_GET_SUPER_LONG:
            return constantLongInstruction(stream, "OP_GET_SUPER_LONG", chunk, offset);
        case OP_SET_PROPERTY:
            return constantInstruction(stream, "OP_SET_PROPERTY", chunk, offset);
        case OP_SET_PROPERTY_LONG:
            return constantLongInstruction(stream, "OP_SET_PROPERTY_LONG", chunk, offset);
        case OP_CALL:
            return byteInstruction(stream, "OP_CALL", chunk, offset);
        case OP_CALL_NAMED:
            return namedCallInstruction(stream, chunk, offset);
        case OP_ADD:
            return simpleInstruction(stream, "OP_ADD", offset);
        case OP_SUBTRACT:
            return simpleInstruction(stream, "OP_SUBTRACT", offset);
        case OP_MULTIPLY:
            return simpleInstruction(stream, "OP_MULTIPLY", offset);
        case OP_DIVIDE:
            return simpleInstruction(stream, "OP_DIVIDE", offset);
        case OP_MODULO:
            return simpleInstruction(stream, "OP_MODULO", offset);
        case OP_BITAND:
            return simpleInstruction(stream, "OP_BITAND", offset);
        case OP_BITOR:
            return simpleInstruction(stream, "OP_BITOR", offset);
        case OP_BITXOR:
            return simpleInstruction(stream, "OP_BITXOR", offset);
        case OP_BITNOT:
            return simpleInstruction(stream, "OP_BITNOT", offset);
        case OP_SHL:
            return simpleInstruction(stream, "OP_SHL", offset);
        case OP_SHR:
            return simpleInstruction(stream, "OP_SHR", offset);
        case OP_NOT:
            return simpleInstruction(stream, "OP_NOT", offset);
        case OP_EQUAL:
            return simpleInstruction(stream, "OP_EQUAL", offset);
        case OP_GREATER:
            return simpleInstruction(stream, "OP_GREATER", offset);
        case OP_LESS:
            return simpleInstruction(stream, "OP_LESS", offset);
        case OP_JUMP:
            return jumpInstruction(stream, "OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:
            return jumpInstruction(stream, "OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LOOP:
            return jumpInstruction(stream, "OP_LOOP", -1, chunk, offset);
        case OP_NEGATE:
            return simpleInstruction(stream, "OP_NEGATE", offset);
        case OP_PRINT:
            return simpleInstruction(stream, "OP_PRINT", offset);
        case OP_RETURN:
            return simpleInstruction(stream, "OP_RETURN", offset);
        default:
            stream << "Unknown opcode " << static_cast<int>(instruction) << std::endl;
            return offset + 1;
    }
}
