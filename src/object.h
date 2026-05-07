#ifndef uranium_object_h
#define uranium_object_h

#include "chunk.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Value;
using NativeCallback = Value (*)(int argCount, const Value* args, std::string* errorMessage);

enum ObjType {
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_NATIVE_FUNCTION,
    OBJ_ARRAY,
    OBJ_MAP,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD,
};

enum FastPathStatus {
    FASTPATH_UNCHECKED,
    FASTPATH_COMPILED,
    FASTPATH_UNSUPPORTED,
};

enum JitBackendKind {
    JIT_BACKEND_NONE,
    JIT_BACKEND_FASTPATH,
    JIT_BACKEND_NATIVE,
};

struct NativeJitRegionDeleter {
    void operator()(void* region) const;
};

using NativeJitRegionPtr = std::unique_ptr<void, NativeJitRegionDeleter>;

enum FastPathOp {
    FASTPATH_CONSTANT,
    FASTPATH_NIL,
    FASTPATH_TRUE,
    FASTPATH_FALSE,
    FASTPATH_GET_LOCAL,
    FASTPATH_SET_LOCAL,
    FASTPATH_ADD,
    FASTPATH_SUBTRACT,
    FASTPATH_MULTIPLY,
    FASTPATH_DIVIDE,
    FASTPATH_NOT,
    FASTPATH_EQUAL,
    FASTPATH_GREATER,
    FASTPATH_LESS,
    FASTPATH_NEGATE,
    FASTPATH_POP,
    FASTPATH_RETURN,
    FASTPATH_JUMP,
    FASTPATH_JUMP_IF_FALSE,
    FASTPATH_LOOP,
};

struct FastPathInstruction {
    FastPathOp op;
    uint16_t operand;

    FastPathInstruction(FastPathOp opCode, uint16_t opOperand = 0)
        : op(opCode), operand(opOperand) {
    }
};

struct FastPathPlan {
    uint16_t localCount;
    uint16_t maxStack;
    std::vector<FastPathInstruction> instructions;
    std::vector<uint16_t> entryStackDepths;

    FastPathPlan()
        : localCount(0), maxStack(0) {
    }
};

struct HeapObject {
    ObjType objType;
    bool isMarked;
    bool isOldGeneration;
    bool isRemembered;
    uint8_t age;
    HeapObject* next;

    explicit HeapObject(ObjType type)
        : objType(type),
          isMarked(false),
          isOldGeneration(false),
          isRemembered(false),
          age(0),
          next(nullptr) {
    }

    virtual ~HeapObject() = default;
};

struct FunctionObject : HeapObject {
    std::string name;
    int arity;
    int minArity;
    int upvalueCount;
    bool isAsync;
    bool hasReceiverSlot;
    bool optimized;
    uint32_t callCount;
    FastPathStatus fastPathStatus;
    JitBackendKind jitBackend;
    std::vector<std::string> parameterNames;
    std::vector<std::string> parameterTypes;
    std::vector<std::string> genericParameters;
    std::string returnType;
    bool isGenericSpecialization;
    FunctionPtr genericSource;
    std::vector<std::string> specializationTypes;
    std::vector<std::string> specializationKeys;
    std::vector<FunctionPtr> specializations;
    std::unique_ptr<FastPathPlan> fastPath;
    void* nativeJitEntry;
    std::size_t nativeJitSize;
    NativeJitRegionPtr nativeJitRegion;
    Chunk chunk;

    FunctionObject(std::string functionName, int functionArity = 0)
        : HeapObject(OBJ_FUNCTION),
          name(std::move(functionName)),
          arity(functionArity),
          minArity(functionArity),
          upvalueCount(0),
          isAsync(false),
          hasReceiverSlot(false),
          optimized(false),
          callCount(0),
          fastPathStatus(FASTPATH_UNCHECKED),
          jitBackend(JIT_BACKEND_NONE),
          isGenericSpecialization(false),
          genericSource(nullptr),
          nativeJitEntry(nullptr),
          nativeJitSize(0),
          nativeJitRegion(nullptr) {
    }
};

struct UpvalueObject : HeapObject {
    Value* location;
    Value closed;
    UpvaluePtr next;

    explicit UpvalueObject(Value* slot)
        : HeapObject(OBJ_UPVALUE), location(slot), closed(Value::nilValue()), next(nullptr) {
    }
};

struct ClosureObject : HeapObject {
    FunctionPtr function;
    std::vector<UpvaluePtr> upvalues;

    explicit ClosureObject(FunctionPtr functionValue)
        : HeapObject(OBJ_CLOSURE), function(functionValue) {
        if (function != nullptr && function->upvalueCount > 0) {
            upvalues.resize(static_cast<std::size_t>(function->upvalueCount));
        }
    }
};

struct NativeFunctionObject : HeapObject {
    std::string name;
    int arity;
    NativeCallback callback;

    NativeFunctionObject(std::string functionName, int functionArity, NativeCallback nativeCallback)
        : HeapObject(OBJ_NATIVE_FUNCTION),
          name(std::move(functionName)),
          arity(functionArity),
          callback(nativeCallback) {
    }
};

struct ArrayObject : HeapObject {
    std::vector<Value> elements;

    ArrayObject()
        : HeapObject(OBJ_ARRAY) {
    }
};

struct MapObject : HeapObject {
    std::unordered_map<std::string, Value> entries;

    MapObject()
        : HeapObject(OBJ_MAP) {
    }
};

struct ClassObject : HeapObject {
    std::string name;
    ClassPtr superclass;
    std::unordered_map<std::string, ClosurePtr> methods;

    explicit ClassObject(std::string className)
        : HeapObject(OBJ_CLASS), name(std::move(className)), superclass(nullptr) {
    }
};

struct InstanceObject : HeapObject {
    ClassPtr klass;
    std::unordered_map<std::string, Value> fields;

    explicit InstanceObject(ClassPtr classValue)
        : HeapObject(OBJ_INSTANCE), klass(classValue) {
    }
};

struct BoundMethodObject : HeapObject {
    Value receiver;
    ClosurePtr method;

    BoundMethodObject(Value receiverValue, ClosurePtr methodValue)
        : HeapObject(OBJ_BOUND_METHOD), receiver(std::move(receiverValue)), method(methodValue) {
    }
};

#endif
