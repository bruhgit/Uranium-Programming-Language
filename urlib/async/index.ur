fn sleep(ms) {
return sleepAsync(ms)
}

fn yieldNow() {
return yieldAsync()
}

fn token(name = "token") {
return [
"name": name,
"cancelled": false,
"reason": nil
]
}

fn cancel(tokenValue, reason = "Task cancelled") {
tokenValue.cancelled = true
tokenValue.reason = reason
return tokenValue
}

fn cancelled(tokenValue) {
if (tokenValue == nil or !isMap(tokenValue) or !hasKey(tokenValue, "cancelled")) {
return false
}

return tokenValue.cancelled == true
}

fn reason(tokenValue) {
if (!cancelled(tokenValue)) {
return nil
}

return tokenValue.reason
}

fn throwIfCancelled(tokenValue) {
if (!cancelled(tokenValue)) {
return false
}

if (tokenValue.reason == nil) {
throw "Task cancelled"
}

throw tokenValue.reason
}

async fn checkpoint(tokenValue = nil) {
if (tokenValue != nil) {
throwIfCancelled(tokenValue)
}

await yieldAsync()

if (tokenValue != nil) {
throwIfCancelled(tokenValue)
}

return true
}

fn isTaskValue(value) {
return isTask(value)
}

fn status(task) {
return taskStatus(task)
}

fn done(task) {
return taskDone(task)
}

fn failed(task) {
return taskFailed(task)
}

fn result(task) {
return taskResult(task)
}

fn error(task) {
return taskError(task)
}

fn pending(task) {
let state = status(task)
return state == "ready" or state == "running" or state == "waiting" or state == "sleeping"
}

fn settled(task) {
return done(task)
}

async fn run(action) {
return action()
}

fn spawn(action) {
return run(action)
}

async fn delay(ms, value) {
await sleepAsync(ms)
return value
}

async fn all(tasks) {
let results = []
let index = 0
while (index < len(tasks)) {
push(results, await tasks[index])
index = index + 1
}

return results
}

async fn allSettled(tasks) {
let results = []
let index = 0
while (index < len(tasks)) {
let current = tasks[index]
try {
push(results, [
"ok": true,
"status": "fulfilled",
"value": await current,
"error": nil
])
} catch (err) {
push(results, [
"ok": false,
"status": "rejected",
"value": nil,
"error": err
])
}

index = index + 1
}

return results
}

async fn race(tasks) {
if (len(tasks) == 0) {
return nil
}

while (true) {
let index = 0
while (index < len(tasks)) {
let current = tasks[index]
if (done(current)) {
if (failed(current)) {
throw error(current)
}

return result(current)
}

index = index + 1
}

await yieldAsync()
}
}
