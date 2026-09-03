fn testDebug() {
    let a = 10
    let b = 20
    print("Breakpoint oncesi")
    debugger
    print("Breakpoint sonrasi")
    print(a + b)
}

testDebug()
