fn testOptional(Optional a) {
    if (a.is_there == 0) {
        printn("a is provided: " + a)
    } else {
        printn("a is missing! is_there = " + a.is_there)
    }
}

printn("Testing with value:")
testOptional(5)

printn("Testing without value:")
testOptional()
