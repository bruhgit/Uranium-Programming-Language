fn args() {
return processArgs()
}

fn argCount() {
return processArgCount()
}

fn arg(index) {
return processArgs()[index]
}

fn executable() {
return processExecutablePath()
}

fn entry() {
return processEntryPath()
}

fn cwd() {
return processCwd()
}

fn chdir(path) {
return processChangeDir(path)
}

fn env(name) {
return processGetEnv(name)
}

fn setEnv(name, value) {
return processSetEnv(name, value)
}

fn platform() {
return processPlatform()
}

let __processCapabilityCache = nil

fn __processCapabilityJitProbe(left: Number, right: Number): Number {
return left + right
}

fn __buildProcessCapabilityMap() {
let platformName = processPlatform()

let guiBackend = "stub"
let httpBackend = "unavailable"
let cryptoBackend = "unavailable"

if (platformName == "windows") {
guiBackend = "win32"
httpBackend = "winhttp"
cryptoBackend = "cryptoapi"
} elif (platformName == "macos") {
httpBackend = "libcurl"
cryptoBackend = "commoncrypto"
} elif (platformName == "linux") {
httpBackend = "libcurl"
cryptoBackend = "openssl"
}

let compiled = jitCompile(__processCapabilityJitProbe)
let backend = compiled ? jitBackend(__processCapabilityJitProbe) : "none"

return [
"platform": platformName,
"gui": guiBackend != "stub",
"guiBackend": guiBackend,
"godotScaffold": true,
"netTcp": true,
"http": httpBackend != "unavailable",
"httpBackend": httpBackend,
"cryptoSha256": cryptoBackend != "unavailable",
"cryptoAes": cryptoBackend != "unavailable",
"cryptoBackend": cryptoBackend,
"sqlite": true,
"threads": true,
"workerPool": true,
"asyncScheduler": true,
"packageManager": true,
"formatter": true,
"linter": true,
"debugger": true,
"lsp": true,
"umake": true,
"urc": true,
"ura": true,
"aotCompile": platformName == "windows",
"typeCheckerStatic": true,
"genericSpecialization": true,
"gcYoungGeneration": true,
"gcMoving": false,
"gcConcurrent": false,
"nativeJit": backend == "native",
"jitBackend": backend
]
}

fn features() {
if (__processCapabilityCache == nil) {
__processCapabilityCache = __buildProcessCapabilityMap()
}

return __processCapabilityCache
}

fn feature(name) {
return features()[name]
}

fn supports(name) {
let value = feature(name)
return value == true
}

fn pid() {
return processPid()
}

fn sleep(ms) {
return processSleep(ms)
}

fn run(command) {
return processRun(command)
}

fn status(command) {
return processSystem(command)
}

fn exit(code) {
return processExit(code)
}
