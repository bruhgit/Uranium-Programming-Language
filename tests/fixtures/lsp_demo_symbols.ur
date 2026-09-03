import httpserver as httpserver
from "../fixtures/http_server_fixture/server_once.ur" import hello as importedHello

fn localHandler(request) {
return httpserver.okJson(["ok": true, "path": request.path])
}

class ApiServer() {
fn init(port) {
this.port = port
}
}

let serverName = "uranium-api"
const DEFAULT_PORT = 8080
