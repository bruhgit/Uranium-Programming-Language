#ifndef uranium_vm_h
#define uranium_vm_h

#include "object.h"
#include "value.h"
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr std::size_t INITIAL_STACK_CAPACITY = 256;
constexpr std::size_t INITIAL_FRAME_CAPACITY = 64;
constexpr std::size_t INITIAL_HANDLER_CAPACITY = 256;

struct CallFrame {
    ClosurePtr closure;
    FunctionPtr function;
    uint8_t* ip;
    Value* slots;
    Value* base;
};

struct ExceptionHandler {
    int frameIndex;
    uint16_t catchOffset;
    Value* stackLevel;
};

enum TaskState {
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING,
    TASK_SLEEPING,
    TASK_COMPLETED,
    TASK_FAILED,
};

struct TaskHandle {
    int id;
    std::string name;
    std::vector<Value> stack;
    Value* stackTop;
    std::vector<CallFrame> frames;
    int frameCount;
    std::vector<ExceptionHandler> handlers;
    int handlerCount;
    UpvaluePtr openUpvalues;
    TaskState state;
    TaskPtr waitingOn;
    std::vector<TaskPtr> awaiters;
    bool hasResumeValue;
    bool resumeIsException;
    Value resumeValue;
    long long wakeAtMillis;
    Value result;
    Value failure;
    bool observed;
    long long loopBrokerWindowStartMillis;
    long long loopBrokerInstructionCount;
    long long loopBrokerLoopCount;

    TaskHandle(int taskId, std::string taskName)
        : id(taskId),
          name(std::move(taskName)),
          stack(INITIAL_STACK_CAPACITY),
          stackTop(stack.data()),
          frames(INITIAL_FRAME_CAPACITY),
          frameCount(0),
          handlers(INITIAL_HANDLER_CAPACITY),
          handlerCount(0),
          openUpvalues(nullptr),
          state(TASK_READY),
          waitingOn(nullptr),
          hasResumeValue(false),
          resumeIsException(false),
          wakeAtMillis(0),
          result(Value::nilValue()),
          failure(Value::nilValue()),
          observed(false),
          loopBrokerWindowStartMillis(0),
          loopBrokerInstructionCount(0),
          loopBrokerLoopCount(0) {
    }
};

enum InterpretResult {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
};

class VM {
public:
    std::unordered_map<std::string, Value> globals;
    std::unordered_set<std::string> constantGlobals;

    VM();
    ~VM();

    InterpretResult interpret(const FunctionPtr& function);
    InterpretResult interpret(const char* source);

    void push(const Value& value);
    Value pop();
    Value peek(int distance) const;
    TaskPtr createTimerTask(long long wakeAtMillis,
                            const Value& result,
                            const std::string& name);
    void forceGarbageCollection();
    bool jitCompile(FunctionPtr function);
    int compiledFastPathCount() const;
    int unsupportedFastPathCount() const;
    int nativeCompiledCount() const;
    int fastPathCompiledOnlyCount() const;
    void setDebugTraceEnabled(bool enabled);

private:
    TaskPtr currentTask;
    TaskPtr rootTask;
    std::vector<std::unique_ptr<TaskHandle>> tasks;
    std::deque<TaskPtr> readyQueue;
    int nextTaskId;
    int fastPathCompiledCount;
    int fastPathUnsupportedCount;
    int nativeJitCompiledCount;
    int bytecodeFastPathCompiledCount;
    bool debugTraceEnabled;

    void markRoots();
    void collectGarbage(bool fullCollection);
    void maybeCollectGarbage();
    void resetScheduler();
    void defineNative(const std::string& name, int arity, NativeCallback callback);
    void defineNumberConstant(const std::string& name, double value);
    void defineStringConstant(const std::string& name, const std::string& value);
    void registerStandardLibrary();
    bool ensureTaskStackCapacity(TaskPtr task, std::size_t neededSlots);
    bool ensureFrameCapacity(TaskPtr task, int neededCount);
    bool ensureHandlerCapacity(TaskPtr task, int neededCount);
    bool propagateException(const Value& exception);
    UpvaluePtr captureUpvalue(Value* local);
    void closeUpvalues(Value* last);
    bool rewriteCallSlice(int providedArgCount,
                          int finalArgCount,
                          int minArgCount,
                          const std::vector<std::string>& parameterNames,
                          const std::vector<std::string>* providedArgNames,
                          const std::string& callableName);
    bool prepareFunctionArguments(const FunctionPtr& function,
                                  int providedArgCount,
                                  const std::vector<std::string>* providedArgNames,
                                  const std::string& callableName,
                                  int* actualArgCount);
    bool validateFunctionArguments(const FunctionPtr& function,
                                   int visibleArgCount,
                                   const Value* visibleArgs,
                                   const std::string& callableName);
    bool validateReturnValue(const FunctionPtr& function,
                             const Value& value,
                             const std::string& callableName);
    FunctionPtr maybeSpecializeFunction(const FunctionPtr& function,
                                        int visibleArgCount,
                                        const Value* visibleArgs);
    bool call(const ClosurePtr& closure, int argCount);
    bool callNative(const NativeFunctionPtr& function, int argCount);
    bool callValue(const Value& callee,
                   int argCount,
                   const std::vector<std::string>* providedArgNames = nullptr);
    bool callNamedValue(const Value& callee,
                        int argCount,
                        const std::vector<std::string>& argNames);
    bool maybeCompileFastPath(FunctionPtr function, bool force);
    bool executeNativeJit(FunctionPtr function,
                          const Value* initialSlots,
                          int initialSlotCount,
                          Value* result);
    bool executeFastPath(FunctionPtr function,
                         const Value* initialSlots,
                         int initialSlotCount,
                         Value* result);
    InterpretResult runtimeError(const std::string& message);
    InterpretResult runTaskSlice();
    InterpretResult run();

    TaskPtr createTaskHandle(const std::string& name);
    TaskPtr createAsyncClosureTask(const ClosurePtr& closure,
                                   int argCount,
                                   const Value* args);
    TaskPtr createAsyncBoundMethodTask(const BoundMethodPtr& boundMethod,
                                       int argCount,
                                       const Value* args);
    void enqueueReadyTask(TaskPtr task);
    void resetLoopBroker(TaskPtr task);
    bool noteLoopBrokerInstruction(TaskPtr task, std::string* errorMessage);
    bool noteLoopBrokerLoop(TaskPtr task, std::string* errorMessage);
    void resolveTask(TaskPtr task, const Value& value);
    void failTask(TaskPtr task, const Value& error);
    void wakeAwaiters(TaskPtr task, const Value& value, bool isException);
    bool applyPendingResume(TaskPtr task);
    void pollSleepingTasks();
    bool hasPendingLiveTasks() const;
    bool hasReadyTask() const;
    long long nextWakeDelayMillis() const;
    std::string buildTaskTrace(TaskPtr task, const std::string& message) const;
};

#endif
