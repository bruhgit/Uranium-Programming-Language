// Uranium - Interfaces & Contract Type Subtyping Test

interface Greeter {
    fn greet(name: String)
}

// 1. Nominal Conformance: explicitly implements Greeter
class EnglishGreeter implements Greeter {
    fn greet(name: String) {
        println(f"Hello, {name}!")
    }
}

// 2. Structural Conformance: satisfies Greeter interface without explicit 'implements'
class TurkishGreeter {
    fn greet(name: String) {
        println(f"Selam, {name}!")
    }
}

// Function taking interface type annotation
fn runGreeting(g: Greeter, name: String) {
    g.greet(name)
}

// 3. Inheritance subtyping
class Shape {
    fn area() {
        return 0
    }
}

class Circle(Shape) {
    let r = 5
    fn area() {
        return 3 * this.r * this.r
    }
}

fn printArea(s: Shape) {
    println(f"Alan: {s.area()}")
}

fn main() {
    println("--- Test 1: Nominal Conformance (implements Greeter) ---")
    let eg: Greeter = EnglishGreeter()
    runGreeting(eg, "Alice")

    println("--- Test 2: Structural Conformance (Duck Typing -> Interface) ---")
    let tg = TurkishGreeter()
    runGreeting(tg, "Baris")

    println("--- Test 3: Inheritance Subtyping (Circle is Shape) ---")
    let c: Shape = Circle()
    printArea(c)

    println("TUM INTERFACE & SUBTYPING TESTLERI BASARILI!")
}
