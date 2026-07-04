fn line(level, message) {
let rendered = "[" + upper(level) + "] " + str(message)
println(rendered)
return rendered
}

fn info(message) {
return line("info", message)
}

fn warn(message) {
return line("warn", message)
}

fn error(message) {
return line("error", message)
}

fn debug(label, value) {
let rendered = "[DEBUG] " + label + ": " + str(value)
println(rendered)
return value
}
