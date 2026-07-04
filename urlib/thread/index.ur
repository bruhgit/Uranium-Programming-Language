fn cores() {
return threadHardwareConcurrency()
}

fn yieldNow() {
return threadYield()
}

fn sleep(ms) {
return processSleep(ms)
}

fn spinYield(count) {
let index = 0
while (index < count) {
yieldNow()
index = index + 1
}

return count
}

fn spawn(scriptPath) {
return threadSpawn(scriptPath)
}

fn join(id) {
return threadJoin(id)
}

fn createChannel(name) {
threadChannelCreate(name)
return name
}

fn send(channel, value) {
return threadChannelSend(channel, value)
}

fn receive(channel) {
return threadChannelReceive(channel)
}

fn tryReceive(channel) {
return threadChannelTryReceive(channel)
}

fn poll(channel) {
return threadChannelPoll(channel)
}

fn channelSize(channel) {
return threadChannelSize(channel)
}

fn closeChannel(channel) {
return threadChannelClose(channel)
}

fn broadcast(channels, value) {
let index = 0
while (index < len(channels)) {
send(channels[index], value)
index = index + 1
}

return len(channels)
}

fn drain(channel) {
let values = []
while (true) {
let state = poll(channel)
if (!state.ok) {
return values
}

push(values, state.value)
}
}

fn mutex() {
return threadMutexCreate()
}

fn lock(handle) {
return threadMutexLock(handle)
}

fn tryLock(handle) {
return threadMutexTryLock(handle)
}

fn unlock(handle) {
return threadMutexUnlock(handle)
}

fn pool(workerCount) {
return threadWorkerPoolCreate(workerCount)
}

fn destroyPool(poolHandle) {
return threadWorkerPoolDestroy(poolHandle)
}

fn readTextAsync(poolHandle, path) {
return threadWorkerReadText(poolHandle, path)
}

fn writeTextAsync(poolHandle, path, text) {
return threadWorkerWriteText(poolHandle, path, text)
}

fn httpGetAsync(poolHandle, url) {
return threadWorkerHttpGet(poolHandle, url)
}

fn jobStatus(job) {
return threadWorkerStatus(job)
}

fn jobDone(job) {
return threadWorkerDone(job)
}

fn jobResult(job) {
return threadWorkerResult(job)
}

fn jobError(job) {
return threadWorkerError(job)
}

fn wait(job) {
return threadWorkerWait(job)
}

async fn receiveAsync(channel) {
while (true) {
let state = poll(channel)
if (state.ok) {
return state.value
}

if (state.closed) {
return nil
}

await yieldAsync()
}
}

fn receiveWithTimeout(channel, timeoutMs) {
let deadline = unixMillis() + timeoutMs
while (true) {
let state = poll(channel)
if (state.ok) {
return state.value
}

if (state.closed) {
return nil
}

if (unixMillis() >= deadline) {
throw "Channel receive timed out after " + str(timeoutMs) + "ms"
}

processSleep(1)
}
}

fn select(channels, timeoutMs = nil) {
let deadline = nil
if (timeoutMs != nil) {
deadline = unixMillis() + timeoutMs
}

while (true) {
let index = 0
while (index < len(channels)) {
let channel = channels[index]
let state = poll(channel)
if (state.ok) {
return [
"ok": true,
"status": "received",
"index": index,
"channel": channel,
"value": state.value,
"closed": false,
"size": state.size
]
}

if (state.closed) {
return [
"ok": false,
"status": "closed",
"index": index,
"channel": channel,
"value": nil,
"closed": true,
"size": state.size
]
}

index = index + 1
}

if (deadline != nil and unixMillis() >= deadline) {
return [
"ok": false,
"status": "timeout",
"index": -1,
"channel": nil,
"value": nil,
"closed": false,
"size": 0
]
}

processSleep(1)
}
}

async fn waitAsync(job) {
while (!jobDone(job)) {
await yieldAsync()
}

if (jobStatus(job) == "failed") {
throw jobError(job)
}

return jobResult(job)
}

async fn readText(poolHandle, path) {
return await waitAsync(readTextAsync(poolHandle, path))
}

async fn writeText(poolHandle, path, text) {
return await waitAsync(writeTextAsync(poolHandle, path, text))
}

async fn httpGet(poolHandle, url) {
return await waitAsync(httpGetAsync(poolHandle, url))
}
