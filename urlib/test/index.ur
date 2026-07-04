// Uranium Test Framework
import assert as assert

// Global test state
let currentSuite = ""
let passCount = 0
let failCount = 0

fn describe(name, callback) {
    let oldSuite = currentSuite
    currentSuite = name
    print("\n--- Suite: " + name + " ---\n")
    try {
        callback()
    } catch (err) {
        print("  [ERROR] Suite failed to execute: " + str(err) + "\n")
    }
    currentSuite = oldSuite
}

fn it(name, callback) {
    print("  Test: " + name + " ... ")
    try {
        callback()
        print("PASS\n")
        passCount = passCount + 1
    } catch (err) {
        print("FAIL\n    " + str(err) + "\n")
        failCount = failCount + 1
    }
}

class Expectation {
    let val
    
    fn init(value) {
        this.val = value
    }
    
    fn toBe(expected) {
        assert.equal(this.val, expected, "Expected values to be equal")
    }
    
    fn toBeNil() {
        assert.isNil(this.val, "Expected value to be nil")
    }
    
    fn toThrow() {
        let threw = false
        try {
            this.val()
        } catch (err) {
            threw = true
        }
        if (!threw) {
            throw "Expected function to throw an exception but it did not"
        }
    }
}

fn expect(value) {
    return Expectation(value)
}

fn getSummary() {
    return {
        "passed": passCount,
        "failed": failCount
    }
}
