import fs as fs

const SEPARATOR = fs.SEPARATOR

fn normalize(path) {
return fs.normalize(path)
}

fn absolute(path) {
return fs.absolute(path)
}

fn parent(path) {
return fs.parent(path)
}

fn basename(path) {
return fs.basename(path)
}

fn name(path) {
return basename(path)
}

fn stem(path) {
return fs.stem(path)
}

fn extension(path) {
return fs.extension(path)
}

fn ext(path) {
return extension(path)
}

fn join(a, b) {
return fs.join(a, b)
}

fn join3(a, b, c) {
return fs.join3(a, b, c)
}

fn join4(a, b, c, d) {
return fs.join4(a, b, c, d)
}

fn isAbsolute(path) {
let normalized = absolute(path)
return normalized == normalize(path)
}

fn changeExtension(path, newExtension) {
let base = stem(path)
let dir = parent(path)
let normalizedExtension = newExtension
if (!startsWith(normalizedExtension, ".")) {
normalizedExtension = "." + normalizedExtension
}

if (dir == "") {
return base + normalizedExtension
}

return join(dir, base + normalizedExtension)
}
