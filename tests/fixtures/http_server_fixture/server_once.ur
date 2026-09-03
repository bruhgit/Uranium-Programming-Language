import httpserver as httpserver
import net as net
import thread as thread

const READY_CHANNEL = "__uranium_http_server_ready__"
const FIXTURE_PORT = 18991

fn hello(request) {
let name = "world"
if (hasKey(request.query, "name") and trim(request.query.name) != "") {
name = request.query.name
}

return httpserver.okJson([
"message": "hello " + name,
"method": request.method,
"path": request.path,
"query": request.query
])
}

class main() {
let app = httpserver.app()
httpserver.get(app, "/hello", hello)
let server = net.listen("127.0.0.1", FIXTURE_PORT)
thread.send(READY_CHANNEL, "ready")
let client = net.accept(server)
let request = httpserver.parseRequest(httpserver.readRequest(client))
httpserver.writeResponse(client, httpserver.dispatch(app, request))
net.close(client)
net.close(server)
}
