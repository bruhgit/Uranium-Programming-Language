fn connect(host, port) {
return netTcpConnect(host, port)
}

fn listen(host, port) {
return netTcpListen(host, port)
}

fn accept(serverSocket) {
return netTcpAccept(serverSocket)
}

fn receive(socket, chunkSize = 1024) {
return netTcpReceive(socket, chunkSize)
}

fn send(socket, data) {
return netTcpSend(socket, data)
}

fn close(socket) {
return netTcpClose(socket)
}

fn receiveAll(socket, chunkSize = 1024, maxBytes = 65536) {
let data = ""
while (len(data) < maxBytes) {
let chunk = receive(socket, chunkSize)
if (chunk == nil or chunk == "") {
return data
}

data = data + chunk
}

return data
}

fn readUntil(socket, marker, chunkSize = 1024, maxBytes = 65536) {
let data = ""
while (len(data) < maxBytes) {
if (contains(data, marker)) {
return data
}

let chunk = receive(socket, chunkSize)
if (chunk == nil or chunk == "") {
return data
}

data = data + chunk
}

return data
}
