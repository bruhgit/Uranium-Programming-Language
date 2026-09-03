fn shout(text) {
return upper(text) + "!"
}

fn whisper(text) {
return lower(text)
}

fn tag(label, value) {
return label + ": " + str(value)
}

fn headline(a, b) {
return upper(a) + " :: " + upper(b)
}

fn headline3(a, b, c) {
return upper(a) + " :: " + upper(b) + " :: " + upper(c)
}

fn surround(leftText, middleText, rightText) {
return leftText + middleText + rightText
}

fn slug(text) {
return lower(replace(trim(text), " ", "-"))
}

fn snake(text) {
return lower(replace(trim(text), " ", "_"))
}

fn csv2(a, b) {
return str(a) + "," + str(b)
}

fn csv3(a, b, c) {
return str(a) + "," + str(b) + "," + str(c)
}

fn path2(a, b) {
return a + "/" + b
}

fn join3(a, b, c, separator) {
return str(a) + separator + str(b) + separator + str(c)
}

fn initials(a, b) {
return upper(left(trim(a), 1)) + upper(left(trim(b), 1))
}

fn banner(text) {
return repeat("=", 4) + " " + upper(text) + " " + repeat("=", 4)
}
