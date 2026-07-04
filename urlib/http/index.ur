import json as json

fn request(method, url, body, contentType) {
return httpRequest(method, url, body, contentType)
}

fn get(url) {
return request("GET", url, "", "")
}

fn getText(url) {
return get(url).body
}

fn getJson(url) {
return json.parse(get(url).body)
}

fn post(url, body, contentType) {
return request("POST", url, body, contentType)
}

fn postText(url, body) {
return post(url, body, "text/plain; charset=utf-8")
}

fn postJson(url, value) {
return request("POST", url, json.stringify(value), "application/json")
}

fn put(url, body, contentType) {
return request("PUT", url, body, contentType)
}

fn patch(url, body, contentType) {
return request("PATCH", url, body, contentType)
}

fn del(url) {
return request("DELETE", url, "", "")
}

fn ok(response) {
return response.ok
}

fn status(response) {
return response.status
}

fn body(response) {
return response.body
}
