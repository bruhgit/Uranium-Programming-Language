import json as jsonlib
import net as net

const HEADER_END = "\r\n\r\n"
const DEFAULT_CONTENT_TYPE = "text/plain; charset=utf-8"
const JSON_CONTENT_TYPE = "application/json; charset=utf-8"

fn __findText(text, part, start = 0) {
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

fn __splitText(text, delimiter) {
let parts = []
if (delimiter == "") {
push(parts, text)
return parts
}

let start = 0
while (true) {
let index = __findText(text, delimiter, start)
if (index < 0) {
push(parts, slice(text, start, len(text) - start))
return parts
}

push(parts, slice(text, start, index - start))
start = index + len(delimiter)
}
}

fn __statusText(status) {
if (status == 200) { return "OK" }
if (status == 201) { return "Created" }
if (status == 202) { return "Accepted" }
if (status == 204) { return "No Content" }
if (status == 301) { return "Moved Permanently" }
if (status == 302) { return "Found" }
if (status == 304) { return "Not Modified" }
if (status == 400) { return "Bad Request" }
if (status == 401) { return "Unauthorized" }
if (status == 403) { return "Forbidden" }
if (status == 404) { return "Not Found" }
if (status == 405) { return "Method Not Allowed" }
if (status == 409) { return "Conflict" }
if (status == 422) { return "Unprocessable Entity" }
if (status == 500) { return "Internal Server Error" }
if (status == 502) { return "Bad Gateway" }
if (status == 503) { return "Service Unavailable" }
return "Status"
}

fn __parseQuery(text) {
let query = map()
if (text == "") {
return query
}

let pairs = __splitText(text, "&")
let index = 0
while (index < len(pairs)) {
let pair = pairs[index]
let equals = __findText(pair, "=")
if (equals < 0) {
query[trim(pair)] = ""
} else {
let key = trim(slice(pair, 0, equals))
let value = slice(pair, equals + 1, len(pair) - equals - 1)
query[key] = value
}
index = index + 1
}

return query
}

fn __headerMap(lines) {
let headers = map()
let index = 1
while (index < len(lines)) {
let line = trim(lines[index])
if (line == "") {
return headers
}

let colon = __findText(line, ":")
if (colon > 0) {
let name = lower(trim(slice(line, 0, colon)))
let value = trim(slice(line, colon + 1, len(line) - colon - 1))
headers[name] = value
}
index = index + 1
}

return headers
}

fn __contentLength(headers) {
if (!hasKey(headers, "content-length")) {
return 0
}

return toNumber(headers["content-length"])
}

fn parseRequest(raw) {
let headerEnd = __findText(raw, HEADER_END)
let headerText = raw
let body = ""
if (headerEnd >= 0) {
headerText = slice(raw, 0, headerEnd)
body = slice(raw, headerEnd + len(HEADER_END), len(raw) - headerEnd - len(HEADER_END))
}

let normalized = replace(headerText, "\r\n", "\n")
let lines = __splitText(normalized, "\n")
let requestLine = trim(lines[0])
let parts = __splitText(requestLine, " ")
let method = upper(parts[0])
let target = "/"
if (len(parts) > 1) {
target = parts[1]
}
let version = "HTTP/1.1"
if (len(parts) > 2) {
version = parts[2]
}

let path = target
let queryText = ""
let queryIndex = __findText(target, "?")
if (queryIndex >= 0) {
path = slice(target, 0, queryIndex)
queryText = slice(target, queryIndex + 1, len(target) - queryIndex - 1)
}

let headers = __headerMap(lines)
return [
"method": method,
"target": target,
"path": path,
"queryText": queryText,
"query": __parseQuery(queryText),
"version": version,
"headers": headers,
"body": body,
"raw": raw
]
}

fn header(request, name, fallback = nil) {
let key = lower(name)
if (hasKey(request.headers, key)) {
return request.headers[key]
}
return fallback
}

fn bodyJson(request) {
if (trim(request.body) == "") {
return map()
}
return jsonlib.parse(request.body)
}

fn response(status, body = "", contentType = DEFAULT_CONTENT_TYPE, headers = []) {
return [
"status": status,
"body": str(body),
"contentType": contentType,
"headers": headers
]
}

fn text(status, body, headers = []) {
return response(status, body, DEFAULT_CONTENT_TYPE, headers)
}

fn ok(body, contentType = DEFAULT_CONTENT_TYPE, headers = []) {
return response(200, body, contentType, headers)
}

fn json(status, value, headers = []) {
return response(status, jsonlib.stringify(value), JSON_CONTENT_TYPE, headers)
}

fn okJson(value, headers = []) {
return json(200, value, headers)
}

fn html(status, body, headers = []) {
return response(status, body, "text/html; charset=utf-8", headers)
}

fn redirect(location, status = 302) {
return response(status, "", DEFAULT_CONTENT_TYPE, [["name": "Location", "value": location]])
}

fn notFound(message = "Not Found") {
return text(404, message)
}

fn badRequest(message = "Bad Request") {
return text(400, message)
}

fn __normalizeResponse(value) {
if (value == nil) {
return notFound()
}

if (typeOf(value) == "string") {
return ok(value)
}

if (typeOf(value) != "map") {
return ok(str(value))
}

let status = 200
if (hasKey(value, "status")) {
status = value.status
}

let body = ""
if (hasKey(value, "body") and value.body != nil) {
body = str(value.body)
}

let contentType = DEFAULT_CONTENT_TYPE
if (hasKey(value, "contentType") and value.contentType != nil and value.contentType != "") {
contentType = value.contentType
}

let headers = []
if (hasKey(value, "headers") and value.headers != nil) {
headers = value.headers
}

return [
"status": status,
"body": body,
"contentType": contentType,
"headers": headers
]
}

fn __buildRawResponse(value) {
let responseValue = __normalizeResponse(value)
let body = responseValue.body
let raw = "HTTP/1.1 " + str(responseValue.status) + " " + __statusText(responseValue.status) + "\r\n"
raw = raw + "Content-Length: " + str(len(body)) + "\r\n"
raw = raw + "Content-Type: " + responseValue.contentType + "\r\n"
raw = raw + "Connection: close\r\n"

let index = 0
while (index < len(responseValue.headers)) {
let current = responseValue.headers[index]
raw = raw + current.name + ": " + current.value + "\r\n"
index = index + 1
}

raw = raw + "\r\n" + body
return raw
}

fn readRequest(socket, chunkSize = 1024, maxBytes = 65536) {
let buffer = ""
let headerEnd = -1
let totalRequired = -1

while (len(buffer) < maxBytes) {
let chunk = net.receive(socket, chunkSize)
if (chunk == nil or chunk == "") {
return buffer
}

buffer = buffer + chunk
if (headerEnd < 0) {
headerEnd = __findText(buffer, HEADER_END)
if (headerEnd >= 0) {
let headerText = slice(buffer, 0, headerEnd)
let headers = __headerMap(__splitText(replace(headerText, "\r\n", "\n"), "\n"))
let bodyLength = __contentLength(headers)
totalRequired = headerEnd + len(HEADER_END) + bodyLength
if (len(buffer) >= totalRequired) {
return slice(buffer, 0, totalRequired)
}
if (bodyLength == 0) {
return slice(buffer, 0, headerEnd + len(HEADER_END))
}
}
} else {
if (len(buffer) >= totalRequired) {
return slice(buffer, 0, totalRequired)
}
}
}

return buffer
}

fn writeResponse(socket, value) {
let raw = __buildRawResponse(value)
net.send(socket, raw)
return raw
}

fn serveConnection(socket, handler) {
let raw = readRequest(socket)
if (raw == "") {
net.close(socket)
return false
}

let request = parseRequest(raw)
let responseValue = handler(request)
writeResponse(socket, responseValue)
net.close(socket)
return true
}

fn app() {
return [
"routes": [],
"fallback": notFound
]
}

fn route(appValue, method, path, handler) {
push(appValue.routes, [
"method": upper(method),
"path": path,
"handler": handler
])
return appValue
}

fn get(appValue, path, handler) {
return route(appValue, "GET", path, handler)
}

fn post(appValue, path, handler) {
return route(appValue, "POST", path, handler)
}

fn put(appValue, path, handler) {
return route(appValue, "PUT", path, handler)
}

fn patch(appValue, path, handler) {
return route(appValue, "PATCH", path, handler)
}

fn del(appValue, path, handler) {
return route(appValue, "DELETE", path, handler)
}

fn fallback(appValue, handler) {
appValue.fallback = handler
return appValue
}

fn dispatch(appValue, request) {
let index = 0
while (index < len(appValue.routes)) {
let current = appValue.routes[index]
if (current.method == upper(request.method) and current.path == request.path) {
return current.handler(request)
}
index = index + 1
}

return appValue.fallback(request)
}

fn serveSocket(listenSocket, handler, maxRequests = 0) {
let handled = 0
while (maxRequests <= 0 or handled < maxRequests) {
let client = net.accept(listenSocket)
if (serveConnection(client, handler)) {
handled = handled + 1
}
}
return handled
}

fn serve(host, port, handler, maxRequests = 0) {
let server = net.listen(host, port)
let handled = 0
handled = serveSocket(server, handler, maxRequests)
net.close(server)
return handled
}

fn serveApp(host, port, appValue, maxRequests = 0) {
let server = net.listen(host, port)
let handled = 0
while (maxRequests <= 0 or handled < maxRequests) {
let client = net.accept(server)
let raw = readRequest(client)
if (raw != "") {
let request = parseRequest(raw)
writeResponse(client, dispatch(appValue, request))
handled = handled + 1
}
net.close(client)
}
net.close(server)
return handled
}
