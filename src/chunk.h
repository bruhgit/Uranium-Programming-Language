#ifndef uranium_chunk_h
#define uranium_chunk_h

#include "common.h"
#include "value.h"
#include <iosfwd>
#include <vector>

enum OpCode {
    OP_CONSTANT,
    OP_CONSTANT_LONG,
    OP_UNSET,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_ARRAY,
    OP_MAP,
    OP_CLASS,
    OP_CLASS_LONG,
    OP_INHERIT,
    OP_METHOD,
    OP_METHOD_LONG,
    OP_POP,
    OP_DEFINE_GLOBAL,
    OP_DEFINE_CONST_GLOBAL,
    OP_DEFINE_GLOBAL_LONG,
    OP_DEFINE_CONST_GLOBAL_LONG,
    OP_GET_GLOBAL,
    OP_GET_GLOBAL_LONG,
    OP_SET_GLOBAL,
    OP_SET_GLOBAL_LONG,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_INDEX,
    OP_SET_INDEX,
    OP_GET_PROPERTY,
    OP_GET_PROPERTY_LONG,
    OP_GET_SUPER,
    OP_GET_SUPER_LONG,
    OP_SET_PROPERTY,
    OP_SET_PROPERTY_LONG,
    OP_CALL,
    OP_CALL_NAMED,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_BITAND,
    OP_BITOR,
    OP_BITXOR,
    OP_BITNOT,
    OP_SHL,
    OP_SHR,
    OP_NOT,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_NEGATE,
    OP_PRINT,
    OP_RETURN,
    OP_CLOSURE,
    OP_CLOSURE_LONG,
    OP_CLOSE_UPVALUE,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_PUSH_EXCEPTION_HANDLER,
    OP_POP_EXCEPTION_HANDLER,
    OP_THROW,
    OP_AWAIT,
    OP_BREAKPOINT,
    OP_NOP,
};

class Chunk {
public:
    std::vector<uint8_t> code;
    std::vector<int> lines;
    ValueArray constants;

    Chunk() = default;
    ~Chunk() = default;

    void writeChunk(uint8_t byte, int line);
    int addConstant(Value value);
};

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(Chunk* chunk, int offset);
void disassembleChunkToStream(std::ostream& stream, Chunk* chunk, const char* name);
int disassembleInstructionToStream(std::ostream& stream, Chunk* chunk, int offset);

#endif
