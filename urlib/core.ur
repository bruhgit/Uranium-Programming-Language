const VERSION = URANIUM_VERSION

fn describe(label, value) {
return label + ": " + str(value)
}

fn debug(label, value) {
return upper(label) + ": " + str(value)
}

fn pair(a, b) {
return str(a) + " | " + str(b)
}

fn triple(a, b, c) {
return str(a) + " | " + str(b) + " | " + str(c)
}

fn between(leftText, value, rightText) {
return leftText + str(value) + rightText
}

fn repeatText(text, count) {
return repeat(text, count)
}

fn typed(value) {
return typeOf(value) + " => " + str(value)
}

fn headline3(a, b, c) {
return upper(a) + " :: " + upper(b) + " :: " + upper(c)
}
