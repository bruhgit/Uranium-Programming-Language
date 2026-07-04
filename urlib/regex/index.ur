fn match(pattern, text) {
return regexMatch(pattern, text)
}

fn test(pattern, text) {
return match(pattern, text)
}

fn search(pattern, text) {
return regexSearch(pattern, text)
}

fn findAll(pattern, text) {
return regexFindAll(pattern, text)
}

fn replace(pattern, text, replacement) {
return regexReplace(pattern, text, replacement)
}

fn split(pattern, text) {
return regexSplit(pattern, text)
}
