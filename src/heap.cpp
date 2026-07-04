#include "heap.h"
#include "common.h"
#include <algorithm>
#include <new>

namespace {

template <typename T>
T* linkObject(HeapObject** objects,
              std::size_t* liveObjects,
              std::size_t* bytesAllocated,
              std::size_t* youngObjects,
              std::size_t* youngBytesAllocated,
              T* object) {
    if (g_maxHeapBytes > 0 && (*bytesAllocated) + sizeof(T) > g_maxHeapBytes) {
        std::cerr << "Runtime Error: Heap limit of " << g_maxHeapBytes << " bytes exceeded." << std::endl;
        std::exit(70);
    }
    static_cast<HeapObject*>(object)->next = *objects;
    *objects = object;
    (*liveObjects)++;
    (*bytesAllocated) += sizeof(T);
    (*youngObjects)++;
    (*youngBytesAllocated) += sizeof(T);
    return object;
}

} // namespace

Heap::Heap()
    : objects(nullptr),
      liveObjects(0),
      bytesAllocated(0),
      youngObjects(0),
      oldObjects(0),
      youngBytesAllocated(0),
      oldBytesAllocated(0),
      nextYoungCollectionBytes(g_baseYoungBytes),
      nextFullCollectionBytes(g_baseFullBytes),
      totalCollections(0),
      minorCollections(0),
      fullCollections(0),
      minorCollectionsSinceFull(0),
      lastMode(HEAP_COLLECT_FULL) {
    grayStack.reserve(256);
}

Heap::~Heap() {
    HeapObject* object = objects;
    while (object != nullptr) {
        HeapObject* next = object->next;
        delete object;
        object = next;
    }

    auto clearPool = [](std::vector<void*>& pool, auto castType) {
        for (void* p : pool) {
            delete static_cast<decltype(castType)>(p);
        }
        pool.clear();
    };

    clearPool(poolFunction, static_cast<FunctionObject*>(nullptr));
    clearPool(poolClosure, static_cast<ClosureObject*>(nullptr));
    clearPool(poolUpvalue, static_cast<UpvalueObject*>(nullptr));
    clearPool(poolNativeFunction, static_cast<NativeFunctionObject*>(nullptr));
    clearPool(poolArray, static_cast<ArrayObject*>(nullptr));
    clearPool(poolMap, static_cast<MapObject*>(nullptr));
    clearPool(poolClass, static_cast<ClassObject*>(nullptr));
    clearPool(poolInstance, static_cast<InstanceObject*>(nullptr));
    clearPool(poolBoundMethod, static_cast<BoundMethodObject*>(nullptr));
}

FunctionPtr Heap::allocateFunction(const std::string& name, int arity) {
    FunctionObject* obj;
    if (!poolFunction.empty()) {
        obj = new (poolFunction.back()) FunctionObject(name, arity);
        poolFunction.pop_back();
    } else {
        obj = new FunctionObject(name, arity);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

ClosurePtr Heap::allocateClosure(FunctionPtr function) {
    ClosureObject* obj;
    if (!poolClosure.empty()) {
        obj = new (poolClosure.back()) ClosureObject(function);
        poolClosure.pop_back();
    } else {
        obj = new ClosureObject(function);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

UpvaluePtr Heap::allocateUpvalue(Value* slot) {
    UpvalueObject* obj;
    if (!poolUpvalue.empty()) {
        obj = new (poolUpvalue.back()) UpvalueObject(slot);
        poolUpvalue.pop_back();
    } else {
        obj = new UpvalueObject(slot);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

NativeFunctionPtr Heap::allocateNativeFunction(const std::string& name,
                                               int arity,
                                               NativeCallback callback) {
    NativeFunctionObject* obj;
    if (!poolNativeFunction.empty()) {
        obj = new (poolNativeFunction.back()) NativeFunctionObject(name, arity, callback);
        poolNativeFunction.pop_back();
    } else {
        obj = new NativeFunctionObject(name, arity, callback);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated,
                      &youngObjects, &youngBytesAllocated, obj);
}

ArrayPtr Heap::allocateArray() {
    ArrayObject* obj;
    if (!poolArray.empty()) {
        obj = new (poolArray.back()) ArrayObject();
        poolArray.pop_back();
    } else {
        obj = new ArrayObject();
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

MapPtr Heap::allocateMap() {
    MapObject* obj;
    if (!poolMap.empty()) {
        obj = new (poolMap.back()) MapObject();
        poolMap.pop_back();
    } else {
        obj = new MapObject();
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

ClassPtr Heap::allocateClass(const std::string& name) {
    ClassObject* obj;
    if (!poolClass.empty()) {
        obj = new (poolClass.back()) ClassObject(name);
        poolClass.pop_back();
    } else {
        obj = new ClassObject(name);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

InstancePtr Heap::allocateInstance(ClassPtr klass) {
    InstanceObject* obj;
    if (!poolInstance.empty()) {
        obj = new (poolInstance.back()) InstanceObject(klass);
        poolInstance.pop_back();
    } else {
        obj = new InstanceObject(klass);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated, &youngObjects,
                      &youngBytesAllocated, obj);
}

BoundMethodPtr Heap::allocateBoundMethod(const Value& receiver, ClosurePtr method) {
    BoundMethodObject* obj;
    if (!poolBoundMethod.empty()) {
        obj = new (poolBoundMethod.back()) BoundMethodObject(receiver, method);
        poolBoundMethod.pop_back();
    } else {
        obj = new BoundMethodObject(receiver, method);
    }
    return linkObject(&objects, &liveObjects, &bytesAllocated,
                      &youngObjects, &youngBytesAllocated, obj);
}

void Heap::markValue(const Value& value) {
    switch (value.type) {
        case VAL_FUNCTION:
            markObject(value.asFunction());
            break;
        case VAL_CLOSURE:
            markObject(value.asClosure());
            break;
        case VAL_NATIVE_FUNCTION:
            markObject(value.asNativeFunction());
            break;
        case VAL_ARRAY:
            markObject(value.asArray());
            break;
        case VAL_MAP:
            markObject(value.asMap());
            break;
        case VAL_CLASS:
            markObject(value.asClass());
            break;
        case VAL_INSTANCE:
            markObject(value.asInstance());
            break;
        case VAL_BOUND_METHOD:
            markObject(value.asBoundMethod());
            break;
        default:
            break;
    }
}

void Heap::markObject(HeapObject* object) {
    if (object == nullptr || object->isMarked) {
        return;
    }

    object->isMarked = true;
    grayStack.push_back(object);
}

void Heap::blackenObject(HeapObject* object) {
    if (object == nullptr) {
        return;
    }

    switch (object->objType) {
        case OBJ_FUNCTION: {
            FunctionObject* function = static_cast<FunctionObject*>(object);
            markObject(function->genericSource);
            for (FunctionPtr specialization : function->specializations) {
                markObject(specialization);
            }
            for (const Value& constant : function->chunk.constants.values) {
                markValue(constant);
            }
            break;
        }
        case OBJ_CLOSURE: {
            ClosureObject* closure = static_cast<ClosureObject*>(object);
            markObject(closure->function);
            for (UpvaluePtr upvalue : closure->upvalues) {
                markObject(upvalue);
            }
            break;
        }
        case OBJ_UPVALUE: {
            UpvalueObject* upvalue = static_cast<UpvalueObject*>(object);
            markValue(upvalue->closed);
            markObject(upvalue->next);
            break;
        }
        case OBJ_NATIVE_FUNCTION:
            break;
        case OBJ_ARRAY: {
            ArrayObject* array = static_cast<ArrayObject*>(object);
            for (const Value& element : array->elements) {
                markValue(element);
            }
            break;
        }
        case OBJ_MAP: {
            MapObject* map = static_cast<MapObject*>(object);
            for (const auto& entry : map->entries) {
                markValue(entry.second);
            }
            break;
        }
        case OBJ_CLASS: {
            ClassObject* klass = static_cast<ClassObject*>(object);
            markObject(klass->superclass);
            for (const auto& entry : klass->methods) {
                markObject(entry.second);
            }
            break;
        }
        case OBJ_INSTANCE: {
            InstanceObject* instance = static_cast<InstanceObject*>(object);
            markObject(instance->klass);
            for (const auto& field : instance->fields) {
                markValue(field.second);
            }
            break;
        }
        case OBJ_BOUND_METHOD: {
            BoundMethodObject* boundMethod = static_cast<BoundMethodObject*>(object);
            markValue(boundMethod->receiver);
            markObject(boundMethod->method);
            break;
        }
    }
}

void Heap::traceReferences() {
    while (!grayStack.empty()) {
        HeapObject* object = grayStack.back();
        grayStack.pop_back();
        blackenObject(object);
    }
}

void Heap::markRememberedObjects() {
    for (HeapObject* object : rememberedSet) {
        if (object == nullptr || !object->isRemembered || !object->isOldGeneration) {
            continue;
        }
        markObject(object);
    }
}

void Heap::sweep(HeapCollectionMode mode) {
    HeapObject* previous = nullptr;
    HeapObject* object = objects;

    while (object != nullptr) {
        if (object->isMarked) {
            object->isMarked = false;
            if (!object->isOldGeneration) {
                if (object->age < 255) {
                    object->age++;
                }
                if (object->age >= 2) {
                    object->isOldGeneration = true;
                }
            }
            previous = object;
            object = object->next;
            continue;
        }

        if (mode == HEAP_COLLECT_YOUNG && object->isOldGeneration) {
            previous = object;
            object = object->next;
            continue;
        }

        HeapObject* unreached = object;
        object = object->next;

        if (previous == nullptr) {
            objects = object;
        } else {
            previous->next = object;
        }

        switch (unreached->objType) {
            case OBJ_FUNCTION:
                static_cast<FunctionObject*>(unreached)->~FunctionObject();
                poolFunction.push_back(unreached);
                break;
            case OBJ_CLOSURE:
                static_cast<ClosureObject*>(unreached)->~ClosureObject();
                poolClosure.push_back(unreached);
                break;
            case OBJ_UPVALUE:
                static_cast<UpvalueObject*>(unreached)->~UpvalueObject();
                poolUpvalue.push_back(unreached);
                break;
            case OBJ_NATIVE_FUNCTION:
                static_cast<NativeFunctionObject*>(unreached)->~NativeFunctionObject();
                poolNativeFunction.push_back(unreached);
                break;
            case OBJ_ARRAY:
                static_cast<ArrayObject*>(unreached)->~ArrayObject();
                poolArray.push_back(unreached);
                break;
            case OBJ_MAP:
                static_cast<MapObject*>(unreached)->~MapObject();
                poolMap.push_back(unreached);
                break;
            case OBJ_CLASS:
                static_cast<ClassObject*>(unreached)->~ClassObject();
                poolClass.push_back(unreached);
                break;
            case OBJ_INSTANCE:
                static_cast<InstanceObject*>(unreached)->~InstanceObject();
                poolInstance.push_back(unreached);
                break;
            case OBJ_BOUND_METHOD:
                static_cast<BoundMethodObject*>(unreached)->~BoundMethodObject();
                poolBoundMethod.push_back(unreached);
                break;
        }

        liveObjects--;
    }
}

void Heap::collectGarbage(HeapCollectionMode mode) {
    if (mode == HEAP_COLLECT_YOUNG) {
        markRememberedObjects();
    }
    traceReferences();
    sweep(mode);
    grayStack.clear();

    if (mode == HEAP_COLLECT_FULL) {
        for (HeapObject* object : rememberedSet) {
            if (object != nullptr) {
                object->isRemembered = false;
            }
        }
        rememberedSet.clear();
    } else {
        rememberedSet.erase(
            std::remove_if(
                rememberedSet.begin(),
                rememberedSet.end(),
                [](HeapObject* object) {
                    return object == nullptr || !object->isRemembered || !object->isOldGeneration;
                }),
            rememberedSet.end());
    }

    refreshAccounting();
    lastMode = mode;
    totalCollections++;

    if (mode == HEAP_COLLECT_FULL) {
        fullCollections++;
        minorCollectionsSinceFull = 0;
    } else {
        minorCollections++;
        minorCollectionsSinceFull++;
    }
}

bool Heap::shouldCollectYoung() const {
    std::size_t approximateYoungBytes =
        bytesAllocated > oldBytesAllocated ? bytesAllocated - oldBytesAllocated : bytesAllocated;
    return approximateYoungBytes >= nextYoungCollectionBytes;
}

bool Heap::shouldCollectFull() const {
    return oldBytesAllocated >= nextFullCollectionBytes || minorCollectionsSinceFull >= 8;
}

std::size_t Heap::objectCount() const {
    return liveObjects;
}

std::size_t Heap::allocatedBytes() const {
    return bytesAllocated;
}

std::size_t Heap::nextCollectionThreshold() const {
    return nextYoungCollectionBytes;
}

std::size_t Heap::collectionCount() const {
    return totalCollections;
}

std::size_t Heap::youngObjectCount() const {
    return youngObjects;
}

std::size_t Heap::oldObjectCount() const {
    return oldObjects;
}

std::size_t Heap::youngAllocatedBytes() const {
    return youngBytesAllocated;
}

std::size_t Heap::oldAllocatedBytes() const {
    return oldBytesAllocated;
}

std::size_t Heap::nextYoungCollectionThreshold() const {
    return nextYoungCollectionBytes;
}

std::size_t Heap::nextFullCollectionThreshold() const {
    return nextFullCollectionBytes;
}

std::size_t Heap::minorCollectionCount() const {
    return minorCollections;
}

std::size_t Heap::fullCollectionCount() const {
    return fullCollections;
}

std::size_t Heap::rememberedObjectCount() const {
    return rememberedSet.size();
}

HeapCollectionMode Heap::lastCollectionMode() const {
    return lastMode;
}

void Heap::writeBarrier(HeapObject* owner, const Value& value) {
    if (owner == nullptr || !owner->isOldGeneration) {
        return;
    }

    HeapObject* child = nullptr;
    switch (value.type) {
        case VAL_FUNCTION:
            child = value.asFunction();
            break;
        case VAL_CLOSURE:
            child = value.asClosure();
            break;
        case VAL_NATIVE_FUNCTION:
            child = value.asNativeFunction();
            break;
        case VAL_ARRAY:
            child = value.asArray();
            break;
        case VAL_MAP:
            child = value.asMap();
            break;
        case VAL_CLASS:
            child = value.asClass();
            break;
        case VAL_INSTANCE:
            child = value.asInstance();
            break;
        case VAL_BOUND_METHOD:
            child = value.asBoundMethod();
            break;
        default:
            break;
    }

    if (child == nullptr || child->isOldGeneration) {
        return;
    }

    if (!owner->isRemembered) {
        owner->isRemembered = true;
        rememberedSet.push_back(owner);
    }
}

std::size_t Heap::estimateObjectSize(const HeapObject* object) const {
    if (object == nullptr) {
        return 0;
    }

    switch (object->objType) {
        case OBJ_FUNCTION: {
            const FunctionObject* function = static_cast<const FunctionObject*>(object);
            std::size_t total = sizeof(FunctionObject) +
                                function->name.capacity() +
                                function->chunk.code.capacity() * sizeof(uint8_t) +
                                function->chunk.lines.capacity() * sizeof(int) +
                                function->chunk.constants.values.capacity() * sizeof(Value);
            if (function->fastPath != nullptr) {
                total += sizeof(FastPathPlan) +
                         function->fastPath->instructions.capacity() *
                             sizeof(FastPathInstruction) +
                         function->fastPath->entryStackDepths.capacity() *
                             sizeof(uint16_t);
            }
            total += function->nativeJitSize;
            return total;
        }
        case OBJ_CLOSURE: {
            const ClosureObject* closure = static_cast<const ClosureObject*>(object);
            return sizeof(ClosureObject) +
                   closure->upvalues.capacity() * sizeof(UpvaluePtr);
        }
        case OBJ_UPVALUE:
            return sizeof(UpvalueObject);
        case OBJ_NATIVE_FUNCTION: {
            const NativeFunctionObject* native = static_cast<const NativeFunctionObject*>(object);
            return sizeof(NativeFunctionObject) + native->name.capacity();
        }
        case OBJ_ARRAY: {
            const ArrayObject* array = static_cast<const ArrayObject*>(object);
            return sizeof(ArrayObject) + array->elements.capacity() * sizeof(Value);
        }
        case OBJ_MAP: {
            const MapObject* map = static_cast<const MapObject*>(object);
            std::size_t total = sizeof(MapObject);
            for (const auto& entry : map->entries) {
                total += sizeof(entry);
                total += entry.first.capacity();
            }
            return total;
        }
        case OBJ_CLASS: {
            const ClassObject* klass = static_cast<const ClassObject*>(object);
            std::size_t total = sizeof(ClassObject) + klass->name.capacity();
            for (const auto& entry : klass->methods) {
                total += sizeof(entry);
                total += entry.first.capacity();
            }
            return total;
        }
        case OBJ_INSTANCE: {
            const InstanceObject* instance = static_cast<const InstanceObject*>(object);
            std::size_t total = sizeof(InstanceObject);
            for (const auto& entry : instance->fields) {
                total += sizeof(entry);
                total += entry.first.capacity();
            }
            return total;
        }
        case OBJ_BOUND_METHOD:
            return sizeof(BoundMethodObject);
    }

    return sizeof(HeapObject);
}

void Heap::refreshAccounting() {
    std::size_t total = 0;
    std::size_t youngBytes = 0;
    std::size_t oldBytes = 0;
    std::size_t youngCount = 0;
    std::size_t oldCount = 0;
    for (HeapObject* object = objects; object != nullptr; object = object->next) {
        std::size_t size = estimateObjectSize(object);
        total += size;
        if (object->isOldGeneration) {
            oldBytes += size;
            oldCount++;
        } else {
            youngBytes += size;
            youngCount++;
        }
    }

    bytesAllocated = total;
    youngBytesAllocated = youngBytes;
    oldBytesAllocated = oldBytes;
    youngObjects = youngCount;
    oldObjects = oldCount;
    liveObjects = youngCount + oldCount;
    nextYoungCollectionBytes = std::max<std::size_t>(g_baseYoungBytes, youngBytesAllocated * 2 + 4096);
    nextFullCollectionBytes = std::max<std::size_t>(g_baseFullBytes, oldBytesAllocated * 2 + 65536);
}

Heap& uraniumHeap() {
    thread_local Heap heap;
    return heap;
}
