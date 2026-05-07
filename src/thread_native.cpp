#include "thread_native.h"

#include "http_native.h"
#include "heap.h"
#include "source_loader.h"
#include "vm.h"

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

enum TransferKind {
    TRANSFER_NIL,
    TRANSFER_BOOL,
    TRANSFER_NUMBER,
    TRANSFER_STRING,
    TRANSFER_ARRAY,
    TRANSFER_MAP,
};

struct TransferValue {
    TransferKind kind = TRANSFER_NIL;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<TransferValue> elements;
    std::unordered_map<std::string, TransferValue> entries;
};

enum WorkerJobState {
    WORKER_JOB_QUEUED,
    WORKER_JOB_RUNNING,
    WORKER_JOB_COMPLETED,
    WORKER_JOB_FAILED,
};

struct Channel {
    std::queue<TransferValue> messages;
    bool closed = false;
    std::mutex mutex;
    std::condition_variable cv;
};

struct MutexHandle {
    std::mutex mutex;
};

struct WorkerJob {
    int id = 0;
    WorkerJobState state = WORKER_JOB_QUEUED;
    bool hasResult = false;
    TransferValue result;
    std::string error;
    std::mutex mutex;
    std::condition_variable cv;
};

struct WorkerPool {
    int id = 0;
    bool stopping = false;
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> queue;
    std::mutex mutex;
    std::condition_variable cv;
};

std::vector<std::unique_ptr<std::thread>> g_threads;
std::mutex g_threadMutex;

std::unordered_map<std::string, std::shared_ptr<Channel>> g_channels;
std::mutex g_channelsMutex;

std::unordered_map<int, std::shared_ptr<MutexHandle>> g_mutexes;
std::mutex g_mutexesMutex;
int g_nextMutexId = 1;

std::unordered_map<int, std::shared_ptr<WorkerPool>> g_pools;
std::unordered_map<int, std::shared_ptr<WorkerJob>> g_jobs;
std::mutex g_workersMutex;
int g_nextPoolId = 1;
int g_nextJobId = 1;

bool setError(std::string* errorMessage, const std::string& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool ensureArgCount(int argCount, int expectedCount, std::string* errorMessage) {
    if (argCount == expectedCount) {
        return true;
    }

    return setError(errorMessage,
                    "Expected " + std::to_string(expectedCount) + " argument(s) but got " +
                        std::to_string(argCount) + ".");
}

bool ensureString(const Value& value,
                  const std::string& functionName,
                  int index,
                  std::string* errorMessage) {
    if (value.isString()) {
        return true;
    }

    return setError(errorMessage,
                    functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a string.");
}

bool ensureNumber(const Value& value,
                  const std::string& functionName,
                  int index,
                  std::string* errorMessage) {
    if (value.isNumber()) {
        return true;
    }

    return setError(errorMessage,
                    functionName + " expects argument " + std::to_string(index + 1) +
                        " to be a number.");
}

bool ensureWholeNumber(const Value& value,
                       const std::string& functionName,
                       int index,
                       std::string* errorMessage,
                       int* out) {
    if (!ensureNumber(value, functionName, index, errorMessage)) {
        return false;
    }

    int converted = static_cast<int>(value.asNumber());
    if (value.asNumber() != static_cast<double>(converted)) {
        return setError(errorMessage,
                        functionName + " expects argument " + std::to_string(index + 1) +
                            " to be a whole number.");
    }

    if (out != nullptr) {
        *out = converted;
    }
    return true;
}

bool copyValueToTransfer(const Value& value, TransferValue* out, std::string* errorMessage) {
    if (out == nullptr) {
        return false;
    }

    if (value.isNil()) {
        out->kind = TRANSFER_NIL;
        return true;
    }
    if (value.isBool()) {
        out->kind = TRANSFER_BOOL;
        out->boolean = value.asBool();
        return true;
    }
    if (value.isNumber()) {
        out->kind = TRANSFER_NUMBER;
        out->number = value.asNumber();
        return true;
    }
    if (value.isString()) {
        out->kind = TRANSFER_STRING;
        out->string = value.asString();
        return true;
    }
    if (value.isArray()) {
        out->kind = TRANSFER_ARRAY;
        out->elements.clear();
        ArrayPtr array = value.asArray();
        if (array == nullptr) {
            return true;
        }

        out->elements.reserve(array->elements.size());
        for (const Value& element : array->elements) {
            TransferValue item;
            if (!copyValueToTransfer(element, &item, errorMessage)) {
                return false;
            }
            out->elements.push_back(std::move(item));
        }
        return true;
    }
    if (value.isMap()) {
        out->kind = TRANSFER_MAP;
        out->entries.clear();
        MapPtr map = value.asMap();
        if (map == nullptr) {
            return true;
        }

        for (const auto& entry : map->entries) {
            TransferValue item;
            if (!copyValueToTransfer(entry.second, &item, errorMessage)) {
                return false;
            }
            out->entries[entry.first] = std::move(item);
        }
        return true;
    }

    return setError(errorMessage,
                    "Only nil, bool, number, string, array, and map values are transferable between threads.");
}

Value transferToValue(const TransferValue& value) {
    switch (value.kind) {
        case TRANSFER_NIL:
            return Value::nilValue();
        case TRANSFER_BOOL:
            return Value::boolValue(value.boolean);
        case TRANSFER_NUMBER:
            return Value::numberValue(value.number);
        case TRANSFER_STRING:
            return Value::stringValue(value.string);
        case TRANSFER_ARRAY: {
            ArrayPtr array = uraniumHeap().allocateArray();
            for (const TransferValue& element : value.elements) {
                Value converted = transferToValue(element);
                array->elements.push_back(converted);
                uraniumHeap().writeBarrier(array, converted);
            }
            return Value::arrayValue(array);
        }
        case TRANSFER_MAP: {
            MapPtr map = uraniumHeap().allocateMap();
            for (const auto& entry : value.entries) {
                Value converted = transferToValue(entry.second);
                map->entries[entry.first] = converted;
                uraniumHeap().writeBarrier(map, converted);
            }
            return Value::mapValue(map);
        }
    }

    return Value::nilValue();
}

std::shared_ptr<Channel> getChannel(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_channelsMutex);
    auto it = g_channels.find(name);
    if (it == g_channels.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<MutexHandle> getMutexHandle(int id) {
    std::lock_guard<std::mutex> lock(g_mutexesMutex);
    auto it = g_mutexes.find(id);
    if (it == g_mutexes.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<WorkerPool> getWorkerPool(int id) {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    auto it = g_pools.find(id);
    if (it == g_pools.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<WorkerJob> getWorkerJob(int id) {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    auto it = g_jobs.find(id);
    if (it == g_jobs.end()) {
        return nullptr;
    }
    return it->second;
}

void workerLoop(const std::shared_ptr<WorkerPool>& pool) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(pool->mutex);
            pool->cv.wait(lock, [&pool]() { return pool->stopping || !pool->queue.empty(); });
            if (pool->stopping && pool->queue.empty()) {
                return;
            }

            job = std::move(pool->queue.front());
            pool->queue.pop();
        }

        job();
    }
}

std::shared_ptr<WorkerPool> createWorkerPoolInternal(int workerCount) {
    if (workerCount <= 0) {
        workerCount = 1;
    }

    auto pool = std::make_shared<WorkerPool>();
    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        pool->id = g_nextPoolId++;
        g_pools[pool->id] = pool;
    }

    for (int index = 0; index < workerCount; ++index) {
        pool->threads.emplace_back([pool]() {
            workerLoop(pool);
        });
    }

    return pool;
}

void destroyWorkerPoolInternal(const std::shared_ptr<WorkerPool>& pool) {
    if (pool == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->stopping = true;
    }
    pool->cv.notify_all();
    for (std::thread& thread : pool->threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::lock_guard<std::mutex> lock(g_workersMutex);
    g_pools.erase(pool->id);
}

int enqueueWorkerJob(const std::shared_ptr<WorkerPool>& pool,
                     const std::function<bool(TransferValue*, std::string*)>& action,
                     std::string* errorMessage) {
    if (pool == nullptr) {
        setError(errorMessage, "Worker pool does not exist.");
        return -1;
    }

    auto job = std::make_shared<WorkerJob>();
    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        job->id = g_nextJobId++;
        g_jobs[job->id] = job;
    }

    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (pool->stopping) {
            setError(errorMessage, "Worker pool is stopping.");
            return -1;
        }

        pool->queue.push([job, action]() {
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->state = WORKER_JOB_RUNNING;
            }

            TransferValue result;
            std::string error;
            bool ok = action(&result, &error);

            {
                std::lock_guard<std::mutex> lock(job->mutex);
                if (ok) {
                    job->state = WORKER_JOB_COMPLETED;
                    job->result = std::move(result);
                    job->hasResult = true;
                } else {
                    job->state = WORKER_JOB_FAILED;
                    job->error = std::move(error);
                }
            }
            job->cv.notify_all();
        });
    }
    pool->cv.notify_one();
    return job->id;
}

bool readTextFile(const std::string& path, std::string* text, std::string* errorMessage) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open '" + path + "' for reading.");
    }

    std::string result((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        return setError(errorMessage, "Could not read '" + path + "'.");
    }

    if (text != nullptr) {
        *text = std::move(result);
    }
    return true;
}

bool writeTextFile(const std::string& path, const std::string& text, std::string* errorMessage) {
    std::filesystem::path filePath(path);
    std::error_code createError;
    std::filesystem::create_directories(filePath.parent_path(), createError);
    if (createError && !filePath.parent_path().empty()) {
        return setError(errorMessage, "Could not create directory for '" + path + "'.");
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return setError(errorMessage, "Could not open '" + path + "' for writing.");
    }

    file << text;
    if (!file.good()) {
        return setError(errorMessage, "Could not write '" + path + "'.");
    }
    return true;
}

std::string workerJobStateName(WorkerJobState state) {
    switch (state) {
        case WORKER_JOB_QUEUED:
            return "queued";
        case WORKER_JOB_RUNNING:
            return "running";
        case WORKER_JOB_COMPLETED:
            return "completed";
        case WORKER_JOB_FAILED:
            return "failed";
    }
    return "unknown";
}

} // namespace

Value nativeThreadSpawn(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        return setError(errorMessage, "Expected script path string to spawn thread."),
               Value::nilValue();
    }

    std::string path = args[0].asString();
    std::lock_guard<std::mutex> lock(g_threadMutex);

    auto thread = std::make_unique<std::thread>([path]() {
        VM threadVm;
        std::string source;
        std::string loadError;
        std::filesystem::path fullPath = std::filesystem::absolute(path);
        if (loadProgramWithImports(fullPath, std::filesystem::current_path(), &source, &loadError)) {
            threadVm.interpret(source.c_str());
        }
    });

    g_threads.push_back(std::move(thread));
    return Value::numberValue(static_cast<double>(g_threads.size() - 1));
}

Value nativeThreadJoin(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureNumber(args[0], "threadJoin", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::size_t id = static_cast<std::size_t>(args[0].asNumber());
    std::lock_guard<std::mutex> lock(g_threadMutex);
    if (id >= g_threads.size() || !g_threads[id]) {
        return Value::boolValue(false);
    }

    if (g_threads[id]->joinable()) {
        g_threads[id]->join();
    }
    return Value::boolValue(true);
}

Value nativeThreadChannelCreate(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelCreate", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(g_channelsMutex);
    std::string name = args[0].asString();
    if (g_channels.find(name) == g_channels.end()) {
        g_channels[name] = std::make_shared<Channel>();
    }
    return Value::boolValue(true);
}

Value nativeThreadChannelSend(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureString(args[0], "threadChannelSend", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    TransferValue transferred;
    if (!copyValueToTransfer(args[1], &transferred, errorMessage)) {
        return Value::nilValue();
    }

    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (channel->closed) {
            setError(errorMessage, "Channel is closed.");
            return Value::nilValue();
        }
        channel->messages.push(std::move(transferred));
    }
    channel->cv.notify_one();
    return Value::boolValue(true);
}

Value nativeThreadChannelReceive(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelReceive", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    std::unique_lock<std::mutex> lock(channel->mutex);
    channel->cv.wait(lock, [&channel]() { return channel->closed || !channel->messages.empty(); });
    if (channel->messages.empty()) {
        return Value::nilValue();
    }

    TransferValue value = std::move(channel->messages.front());
    channel->messages.pop();
    return transferToValue(value);
}

Value nativeThreadChannelTryReceive(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelTryReceive", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(channel->mutex);
    if (channel->messages.empty()) {
        return Value::nilValue();
    }

    TransferValue value = std::move(channel->messages.front());
    channel->messages.pop();
    return transferToValue(value);
}

Value nativeThreadChannelPoll(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelPoll", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    TransferValue transferred;
    bool hasValue = false;
    bool closed = false;
    std::size_t remainingSize = 0;
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        closed = channel->closed;
        if (!channel->messages.empty()) {
            transferred = std::move(channel->messages.front());
            channel->messages.pop();
            hasValue = true;
        }
        remainingSize = channel->messages.size();
    }

    MapPtr state = uraniumHeap().allocateMap();
    state->entries["ok"] = Value::boolValue(hasValue);
    state->entries["closed"] = Value::boolValue(closed);
    state->entries["size"] = Value::numberValue(static_cast<double>(remainingSize));
    Value payload = hasValue ? transferToValue(transferred) : Value::nilValue();
    state->entries["value"] = payload;
    uraniumHeap().writeBarrier(state, payload);
    return Value::mapValue(state);
}

Value nativeThreadChannelSize(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelSize", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(channel->mutex);
    return Value::numberValue(static_cast<double>(channel->messages.size()));
}

Value nativeThreadChannelClose(int argCount, const Value* args, std::string* errorMessage) {
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureString(args[0], "threadChannelClose", 0, errorMessage)) {
        return Value::nilValue();
    }

    std::shared_ptr<Channel> channel = getChannel(args[0].asString());
    if (channel == nullptr) {
        setError(errorMessage, "Channel does not exist.");
        return Value::nilValue();
    }

    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        channel->closed = true;
    }
    channel->cv.notify_all();
    return Value::boolValue(true);
}

Value nativeThreadMutexCreate(int argCount, const Value* args, std::string* errorMessage) {
    (void)args;
    if (!ensureArgCount(argCount, 0, errorMessage)) {
        return Value::nilValue();
    }

    auto handle = std::make_shared<MutexHandle>();
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutexesMutex);
        id = g_nextMutexId++;
        g_mutexes[id] = handle;
    }
    return Value::numberValue(static_cast<double>(id));
}

Value nativeThreadMutexLock(int argCount, const Value* args, std::string* errorMessage) {
    int id = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadMutexLock", 0, errorMessage, &id)) {
        return Value::nilValue();
    }

    std::shared_ptr<MutexHandle> handle = getMutexHandle(id);
    if (handle == nullptr) {
        setError(errorMessage, "Mutex does not exist.");
        return Value::nilValue();
    }

    handle->mutex.lock();
    return Value::boolValue(true);
}

Value nativeThreadMutexTryLock(int argCount, const Value* args, std::string* errorMessage) {
    int id = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadMutexTryLock", 0, errorMessage, &id)) {
        return Value::nilValue();
    }

    std::shared_ptr<MutexHandle> handle = getMutexHandle(id);
    if (handle == nullptr) {
        setError(errorMessage, "Mutex does not exist.");
        return Value::nilValue();
    }

    return Value::boolValue(handle->mutex.try_lock());
}

Value nativeThreadMutexUnlock(int argCount, const Value* args, std::string* errorMessage) {
    int id = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadMutexUnlock", 0, errorMessage, &id)) {
        return Value::nilValue();
    }

    std::shared_ptr<MutexHandle> handle = getMutexHandle(id);
    if (handle == nullptr) {
        setError(errorMessage, "Mutex does not exist.");
        return Value::nilValue();
    }

    handle->mutex.unlock();
    return Value::boolValue(true);
}

Value nativeThreadWorkerPoolCreate(int argCount, const Value* args, std::string* errorMessage) {
    int workerCount = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerPoolCreate", 0, errorMessage, &workerCount)) {
        return Value::nilValue();
    }

    if (workerCount < 1) {
        setError(errorMessage, "threadWorkerPoolCreate expects a positive worker count.");
        return Value::nilValue();
    }

    return Value::numberValue(static_cast<double>(createWorkerPoolInternal(workerCount)->id));
}

Value nativeThreadWorkerPoolDestroy(int argCount, const Value* args, std::string* errorMessage) {
    int poolId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerPoolDestroy", 0, errorMessage, &poolId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerPool> pool = getWorkerPool(poolId);
    if (pool == nullptr) {
        return Value::boolValue(false);
    }

    destroyWorkerPoolInternal(pool);
    return Value::boolValue(true);
}

Value nativeThreadWorkerReadText(int argCount, const Value* args, std::string* errorMessage) {
    int poolId = 0;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerReadText", 0, errorMessage, &poolId) ||
        !ensureString(args[1], "threadWorkerReadText", 1, errorMessage)) {
        return Value::nilValue();
    }

    std::string path = args[1].asString();
    int jobId = enqueueWorkerJob(
        getWorkerPool(poolId),
        [path](TransferValue* result, std::string* workerError) {
            std::string text;
            if (!readTextFile(path, &text, workerError)) {
                return false;
            }
            result->kind = TRANSFER_STRING;
            result->string = std::move(text);
            return true;
        },
        errorMessage);
    if (jobId < 0) {
        return Value::nilValue();
    }
    return Value::numberValue(static_cast<double>(jobId));
}

Value nativeThreadWorkerWriteText(int argCount, const Value* args, std::string* errorMessage) {
    int poolId = 0;
    if (!ensureArgCount(argCount, 3, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerWriteText", 0, errorMessage, &poolId) ||
        !ensureString(args[1], "threadWorkerWriteText", 1, errorMessage) ||
        !ensureString(args[2], "threadWorkerWriteText", 2, errorMessage)) {
        return Value::nilValue();
    }

    std::string path = args[1].asString();
    std::string text = args[2].asString();
    int jobId = enqueueWorkerJob(
        getWorkerPool(poolId),
        [path, text](TransferValue* result, std::string* workerError) {
            if (!writeTextFile(path, text, workerError)) {
                return false;
            }
            result->kind = TRANSFER_BOOL;
            result->boolean = true;
            return true;
        },
        errorMessage);
    if (jobId < 0) {
        return Value::nilValue();
    }
    return Value::numberValue(static_cast<double>(jobId));
}

Value nativeThreadWorkerHttpGet(int argCount, const Value* args, std::string* errorMessage) {
    int poolId = 0;
    if (!ensureArgCount(argCount, 2, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerHttpGet", 0, errorMessage, &poolId) ||
        !ensureString(args[1], "threadWorkerHttpGet", 1, errorMessage)) {
        return Value::nilValue();
    }

    std::string url = args[1].asString();
    int jobId = enqueueWorkerJob(
        getWorkerPool(poolId),
        [url](TransferValue* result, std::string* workerError) {
            Value requestArgs[4] = {
                Value::stringValue("GET"),
                Value::stringValue(url),
                Value::stringValue(""),
                Value::stringValue("")
            };
            Value response = nativeHttpRequest(4, requestArgs, workerError);
            if (workerError != nullptr && !workerError->empty()) {
                return false;
            }
            return copyValueToTransfer(response, result, workerError);
        },
        errorMessage);
    if (jobId < 0) {
        return Value::nilValue();
    }
    return Value::numberValue(static_cast<double>(jobId));
}

Value nativeThreadWorkerStatus(int argCount, const Value* args, std::string* errorMessage) {
    int jobId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerStatus", 0, errorMessage, &jobId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerJob> job = getWorkerJob(jobId);
    if (job == nullptr) {
        return Value::stringValue("missing");
    }

    std::lock_guard<std::mutex> lock(job->mutex);
    return Value::stringValue(workerJobStateName(job->state));
}

Value nativeThreadWorkerDone(int argCount, const Value* args, std::string* errorMessage) {
    int jobId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerDone", 0, errorMessage, &jobId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerJob> job = getWorkerJob(jobId);
    if (job == nullptr) {
        return Value::boolValue(false);
    }

    std::lock_guard<std::mutex> lock(job->mutex);
    return Value::boolValue(job->state == WORKER_JOB_COMPLETED ||
                            job->state == WORKER_JOB_FAILED);
}

Value nativeThreadWorkerResult(int argCount, const Value* args, std::string* errorMessage) {
    int jobId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerResult", 0, errorMessage, &jobId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerJob> job = getWorkerJob(jobId);
    if (job == nullptr) {
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->state != WORKER_JOB_COMPLETED || !job->hasResult) {
        return Value::nilValue();
    }

    return transferToValue(job->result);
}

Value nativeThreadWorkerError(int argCount, const Value* args, std::string* errorMessage) {
    int jobId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerError", 0, errorMessage, &jobId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerJob> job = getWorkerJob(jobId);
    if (job == nullptr) {
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->state != WORKER_JOB_FAILED) {
        return Value::nilValue();
    }

    return Value::stringValue(job->error);
}

Value nativeThreadWorkerWait(int argCount, const Value* args, std::string* errorMessage) {
    int jobId = 0;
    if (!ensureArgCount(argCount, 1, errorMessage) ||
        !ensureWholeNumber(args[0], "threadWorkerWait", 0, errorMessage, &jobId)) {
        return Value::nilValue();
    }

    std::shared_ptr<WorkerJob> job = getWorkerJob(jobId);
    if (job == nullptr) {
        setError(errorMessage, "Worker job does not exist.");
        return Value::nilValue();
    }

    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&job]() {
        return job->state == WORKER_JOB_COMPLETED || job->state == WORKER_JOB_FAILED;
    });

    if (job->state == WORKER_JOB_FAILED) {
        setError(errorMessage, job->error);
        return Value::nilValue();
    }

    return transferToValue(job->result);
}
