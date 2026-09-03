#ifndef uranium_heap_h
#define uranium_heap_h

#include "object.h"
#include "value.h"
#include <cstddef>
#include <cstdint>
#include <vector>

enum HeapCollectionMode {
    HEAP_COLLECT_YOUNG,
    HEAP_COLLECT_FULL,
};
class Heap {
public:
    enum GcState {
        GC_STATE_IDLE,
        GC_STATE_MARKING,
        GC_STATE_SWEEPING
    };

    Heap();
    ~Heap();

    FunctionPtr allocateFunction(const std::string& name, int arity = 0);
    ClosurePtr allocateClosure(FunctionPtr function);
    UpvaluePtr allocateUpvalue(Value* slot);
    NativeFunctionPtr allocateNativeFunction(const std::string& name, int arity, NativeCallback callback);
    ArrayPtr allocateArray();
    MapPtr allocateMap();
    ClassPtr allocateClass(const std::string& name);
    InstancePtr allocateInstance(ClassPtr klass);
    BoundMethodPtr allocateBoundMethod(const Value& receiver, ClosurePtr method);
    BoundMethodPtr allocateBoundNativeMethod(const Value& receiver, NativeFunctionPtr method);

    void markValue(const Value& value);
    void markObject(HeapObject* object);
    void writeBarrier(HeapObject* owner, const Value& value);
    void collectGarbage(HeapCollectionMode mode = HEAP_COLLECT_FULL);
    void collectGarbageStep(std::size_t workLimit = 10); // Incremental step function
    bool shouldCollectYoung() const;
    bool shouldCollectFull() const;
    std::size_t objectCount() const;
    std::size_t allocatedBytes() const;
    std::size_t nextCollectionThreshold() const;
    std::size_t collectionCount() const;
    std::size_t youngObjectCount() const;
    std::size_t oldObjectCount() const;
    std::size_t youngAllocatedBytes() const;
    std::size_t oldAllocatedBytes() const;
    std::size_t nextYoungCollectionThreshold() const;
    std::size_t nextFullCollectionThreshold() const;
    std::size_t minorCollectionCount() const;
    std::size_t fullCollectionCount() const;
    std::size_t rememberedObjectCount() const;
    HeapCollectionMode lastCollectionMode() const;
    GcState currentGcState() const { return gcState; }

private:
    HeapObject* objects;
    std::size_t liveObjects;
    std::size_t bytesAllocated;
    std::size_t youngObjects;
    std::size_t oldObjects;
    std::size_t youngBytesAllocated;
    std::size_t oldBytesAllocated;
    std::size_t nextYoungCollectionBytes;
    std::size_t nextFullCollectionBytes;
    std::size_t totalCollections;
    std::size_t minorCollections;
    std::size_t fullCollections;
    std::size_t minorCollectionsSinceFull;
    HeapCollectionMode lastMode;
    std::vector<HeapObject*> grayStack;
    std::vector<HeapObject*> rememberedSet;
    
    // Incremental Sweep state trackers
    GcState gcState;
    HeapCollectionMode currentStepMode;
    HeapObject* sweepPointer;
    HeapObject* sweepPrevious;

    std::vector<void*> poolFunction;
    std::vector<void*> poolClosure;
    std::vector<void*> poolUpvalue;
    std::vector<void*> poolNativeFunction;
    std::vector<void*> poolArray;
    std::vector<void*> poolMap;
    std::vector<void*> poolClass;
    std::vector<void*> poolInstance;
    std::vector<void*> poolBoundMethod;

    void blackenObject(HeapObject* object);
    void markRememberedObjects();
    void traceReferences();
    void sweep(HeapCollectionMode mode);
    std::size_t estimateObjectSize(const HeapObject* object) const;
    void refreshAccounting();
};

Heap& uraniumHeap();

#endif
