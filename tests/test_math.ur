import test as test

class main() {
    test.describe("Math Operations", fn() {
        test.it("should add numbers correctly", fn() {
            test.expect(1 + 2).toBe(3)
        })

        test.it("should handle modulo", fn() {
            test.expect(10 % 3).toBe(1)
        })
    })

    test.describe("Failure Demonstration", fn() {
        test.it("should fail gracefully", fn() {
            test.expect(5).toBe(6)
        })
    })
}
