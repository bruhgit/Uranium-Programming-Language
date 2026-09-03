import assert as assert
import async as async
import thread as thread

class main() {
    let token = async.token("demo")
    assert.ok(!async.cancelled(token), "new cancellation tokens should start active")
    async.cancel(token, "stop-now")
    assert.ok(async.cancelled(token), "cancelled token should report cancelled")
    assert.equal(async.reason(token), "stop-now", "cancel reason should be preserved")
    let cancelError = nil
    try {
        async.throwIfCancelled(token)
    } catch (err) {
        cancelError = err
    }
    assert.equal(cancelError, "stop-now", "throwIfCancelled should throw stored reason")

    let channelA = thread.createChannel("structured-a")
    let channelB = thread.createChannel("structured-b")
    thread.send(channelB, "payload")
    let receiveState = thread.select([channelA, channelB], 100)
    assert.equal(receiveState.index, 1, "thread select should report ready channel index")
    assert.equal(receiveState.value, "payload", "thread select should return received value")
    assert.equal(receiveState.status, "received", "thread select should report receive state")

    let receiveTimeoutError = nil
    try {
        thread.receiveWithTimeout(channelA, 1)
    } catch (err) {
        receiveTimeoutError = err
    }
    assert.ok(contains(receiveTimeoutError, "timed out"),
              "receiveWithTimeout should report timeout")

    let channelC = thread.createChannel("structured-c")
    thread.broadcast([channelA, channelC], "fanout")
    let drainedA = thread.drain(channelA)
    let drainedC = thread.drain(channelC)
    assert.equal(drainedA[0], "fanout", "broadcast should deliver to first channel")
    assert.equal(drainedC[0], "fanout", "broadcast should deliver to second channel")

    thread.closeChannel(channelA)
    thread.closeChannel(channelB)
    thread.closeChannel(channelC)

    print("structured-concurrency-ok")
}
