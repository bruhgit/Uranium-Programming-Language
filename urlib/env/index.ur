import process as process

fn get(name) {
return process.env(name)
}

fn set(name, value) {
return process.setEnv(name, value)
}

fn remove(name) {
return process.setEnv(name, nil)
}

fn cwd() {
return process.cwd()
}

fn chdir(path) {
return process.chdir(path)
}

fn platform() {
return process.platform()
}

fn features() {
return process.features()
}

fn supports(name) {
return process.supports(name)
}

fn pid() {
return process.pid()
}

fn executable() {
return process.executable()
}

fn entry() {
return process.entry()
}
