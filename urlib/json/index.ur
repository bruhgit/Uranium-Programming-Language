import fs as fs

fn parse(text) {
return jsonParse(text)
}

fn valid(text) {
return jsonValid(text)
}

fn stringify(value) {
return jsonStringify(value)
}

fn pretty(value) {
return jsonStringifyPretty(value, 2)
}

fn prettyWith(value, indent) {
return jsonStringifyPretty(value, indent)
}

fn read(path) {
return parse(fs.readText(path))
}

fn write(path, value) {
return fs.writeText(path, pretty(value))
}

fn writeCompact(path, value) {
return fs.writeText(path, stringify(value))
}
