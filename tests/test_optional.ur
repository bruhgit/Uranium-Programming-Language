fn fonk(a, Optional b, c = 5, Optional d) {
    printn("a: " + a)
    printn("b: " + b)
    printn("c: " + c)
    printn("d: " + d)
}

printn("--- fonk(1) ---")
fonk(1)

printn("--- fonk(1, 2) ---")
fonk(1, 2)

printn("--- fonk(1, 2, 3) ---")
fonk(1, 2, 3)

printn("--- fonk(1, 2, 3, 4) ---")
fonk(1, 2, 3, 4)
