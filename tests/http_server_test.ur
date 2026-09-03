import assert as assert
import json as json
import net as net
import thread as thread

const READY_CHANNEL = "__uranium_http_server_ready__"
const FIXTURE_PATH = "./tests/fixtures/http_server_fixture/server_once.ur"
const FIXTURE_PORT = 18991

fn findText(text, part, start = 0) {
if (part == "") {
return start
}

let index = start
while (index + len(part) <= len(text)) {
if (slice(text, index, len(part)) == part) {
return index
}
index = index + 1
}

return -1
}

fn bodyOf(rawResponse) {
let marker = "\r\n\r\n"
let index = findText(rawResponse, marker)
if (index < 0) {
return ""
}

return slice(rawResponse, index + len(marker), len(rawResponse) - index - len(marker))
}

class main() {
thread.createChannel(READY_CHANNEL)
let threadId = thread.spawn(FIXTURE_PATH)
let ready = thread.receiveWithTimeout(READY_CHANNEL, 3000)
assert.equal(ready, "ready", "fixture should publish ready signal")

let client = net.connect("127.0.0.1", FIXTURE_PORT)
net.send(client, "GET /hello?name=Uranium HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
let rawResponse = net.receiveAll(client, 1024, 65536)
net.close(client)

assert.ok(contains(rawResponse, "HTTP/1.1 200 OK"), "fixture should return HTTP 200")

let payload = json.parse(bodyOf(rawResponse))
assert.equal(payload.message, "hello Uranium", "server should echo query data")
assert.equal(payload.method, "GET", "request method should be parsed")
assert.equal(payload.path, "/hello", "request path should be parsed")
assert.equal(payload.query.name, "Uranium", "query parser should preserve parameter values")

assert.ok(thread.join(threadId), "fixture thread should join cleanly")
thread.closeChannel(READY_CHANNEL)
print("http-server-ok")
}
