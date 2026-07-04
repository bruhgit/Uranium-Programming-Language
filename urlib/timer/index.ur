const MILLISECOND = 1
const SECOND = 1000
const MINUTE = 60 * SECOND
const HOUR = 60 * MINUTE

fn sleep(ms) {
return processSleep(ms)
}

fn nowMillis() {
return unixMillis()
}

fn nowSeconds() {
return unixMillis() / 1000
}

fn measure(action) {
let startedAt = nowMillis()
action()
return nowMillis() - startedAt
}

fn measureResult(action) {
let startedAt = nowMillis()
let value = action()
return [
"value": value,
"elapsedMs": nowMillis() - startedAt
]
}

class Stopwatch() {
fn init() {
this.startedAt = nowMillis()
}

fn elapsed() {
return nowMillis() - this.startedAt
}

fn reset() {
let elapsedMs = this.elapsed()
this.startedAt = nowMillis()
return elapsedMs
}
}
