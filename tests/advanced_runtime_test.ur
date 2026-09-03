import assert as assert
import path as path
import process as process
import std as std

fn repoRoot() {
    return path.parent(path.parent(path.parent(process.executable())))
}

fn identity<T>(value: T): T {
    return value
}

fn head<T>(values: Array<T>): T {
    return values[0]
}

fn greater(left: Number, right: Number): Bool {
    return left > right
}

fn choose(flag: Bool, left: Number, right: Number): Number {
    if (flag) {
        return left
    }

    return right
}

fn scoreOf(record: Map<String, Number>): Number {
    return record.score
}

async fn readViaAsyncWorker(poolHandle, filePath): String {
    return await std.threadReadText(poolHandle, filePath)
}

async fn receiveNilAsync(channel): Nil {
    return await std.threadReceiveAsync(channel)
}

class main() {
    let numeric: Number = identity(42)
    let text: String = identity("uranium")
    let first: Number = head([4, 5, 6])
    let score: Number = scoreOf(["score": 11, "bonus": 2])

    assert.equal(numeric, 42, "generic specialization should preserve number calls")
    assert.equal(text, "uranium", "generic specialization should preserve string calls")
    assert.equal(first, 4, "generic collection return inference should preserve element type")
    assert.equal(score, 11, "typed map property access should preserve value type")

    assert.ok(jitCompile(greater), "typed numeric comparison should be JIT-compilable")
    let backend = jitBackend(greater)
    assert.ok(backend == "native" or backend == "fastpath",
              "comparison backend should compile")
    assert.ok(jitCompile(choose), "typed branch function should be JIT-compilable")
    let chooseBackend = jitBackend(choose)
    assert.ok(chooseBackend == "native" or chooseBackend == "fastpath",
              "branch backend should compile")
    assert.ok(greater(9, 3), "typed bool return should stay correct")
    assert.equal(choose(true, 7, 9), 7, "typed branch function should still run")

    let channel = std.threadCreateChannel("advanced-runtime-test")
    std.threadSend(channel, ["kind": "payload", "items": [1, 2, 3], "ok": true])
    let payload = std.threadReceive(channel)
    assert.equal(payload.kind, "payload", "channel map transfer should work")
    assert.equal(payload.items[1], 2, "channel nested array transfer should work")
    assert.ok(payload.ok, "channel bool transfer should work")
    assert.equal(std.threadChannelSize(channel), 0, "channel should be empty after receive")
    assert.ok(std.threadCloseChannel(channel), "channel close should work")

    let mutex = std.threadMutex()
    assert.ok(std.threadTryLock(mutex), "mutex should lock immediately")
    std.threadUnlock(mutex)
    assert.ok(std.threadLock(mutex), "mutex blocking lock should work")
    std.threadUnlock(mutex)

    let pool = std.threadPool(2)
    let workerRoot = path.join4(repoRoot(), "tests", "fixtures", "advanced_runtime")
    std.fsCreateDirs(workerRoot)
    let inputPath = path.join(workerRoot, "input.txt")
    let outputPath = path.join(workerRoot, "output.txt")
    std.fsWriteText(inputPath, "worker-io")

    let readJob = std.threadReadTextAsync(pool, inputPath)
    let writeJob = std.threadWriteTextAsync(pool, outputPath, "done")
    let asyncReadTask = readViaAsyncWorker(pool, inputPath)

    assert.equal(std.threadWait(readJob), "worker-io", "worker read should return file text")
    assert.ok(std.threadWait(writeJob), "worker write should report success")
    while (!std.asyncDone(asyncReadTask)) {
        std.threadSleep(1)
    }
    assert.equal(std.asyncResult(asyncReadTask), "worker-io",
                 "async worker wrapper should integrate with scheduler")
    assert.equal(std.fsReadText(outputPath), "done", "worker write should persist output")
    assert.ok(std.threadDestroyPool(pool), "worker pool destroy should work")

    let nilChannel = std.threadCreateChannel("advanced-runtime-nil")
    std.threadSend(nilChannel, nil)
    let nilTask = receiveNilAsync(nilChannel)
    while (!std.asyncDone(nilTask)) {
        std.threadSleep(1)
    }
    assert.ok(std.asyncResult(nilTask) == nil,
              "async channel receive should preserve nil payloads")
    assert.ok(std.threadCloseChannel(nilChannel), "async nil channel close should work")

    print("advanced-runtime-ok")
}
