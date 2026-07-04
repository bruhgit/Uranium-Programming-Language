const SEPARATOR = "/"

fn cwd() {
return fsCwd()
}

fn chdir(path) {
return fsChangeDir(path)
}

fn exists(path) {
return fsExists(path)
}

fn isFile(path) {
return fsIsFile(path)
}

fn isDir(path) {
return fsIsDir(path)
}

fn normalize(path) {
return fsNormalize(path)
}

fn absolute(path) {
return fsAbsolute(path)
}

fn parent(path) {
return fsParent(path)
}

fn basename(path) {
return fsFileName(path)
}

fn name(path) {
return basename(path)
}

fn stem(path) {
return fsStem(path)
}

fn extension(path) {
return fsExtension(path)
}

fn ext(path) {
return extension(path)
}

fn join(left, right) {
return fsJoin2(left, right)
}

fn join3(a, b, c) {
return fsJoin3(a, b, c)
}

fn join4(a, b, c, d) {
return fsJoin4(a, b, c, d)
}

fn readText(path) {
return fsReadText(path)
}

fn readLines(path) {
return fsReadLines(path)
}

fn writeText(path, text) {
return fsWriteText(path, text)
}

fn appendText(path, text) {
return fsAppendText(path, text)
}

fn touch(path) {
if (exists(path)) {
return false
}

writeText(path, "")
return true
}

fn createDir(path) {
return fsCreateDir(path)
}

fn createDirs(path) {
return fsCreateDirs(path)
}

fn ensureDir(path) {
if (isDir(path)) {
return false
}

return createDirs(path)
}

fn remove(path) {
return fsRemove(path)
}

fn removeTree(path) {
return fsRemoveTree(path)
}

fn copy(fromPath, toPath) {
return fsCopy(fromPath, toPath)
}

fn move(fromPath, toPath) {
return fsMove(fromPath, toPath)
}

fn stat(path) {
return fsStat(path)
}

fn size(path) {
return stat(path).size
}

fn listNames(path) {
return fsListNames(path)
}

fn listEntries(path) {
return fsListEntries(path)
}

fn walk(path) {
return fsWalk(path)
}
