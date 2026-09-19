// Uranium - Namespace & Scope Resolution (::) Test

namespace Hello {
    func hell() {
        return "Hello from namespace with func!"
    }

    fn topla(a, b) {
        return a + b
    }

    let surum = "1.0.0"
}

namespace MathOps {
    namespace Trig {
        func pi() {
            return 3.14159
        }
    }
}

fn main() {
    println("--- Test 1: Temel Namespace & func ---")
    println(Hello::hell())

    println("--- Test 2: Namespace Fonksiyonu & Parametre ---")
    let sonuc = Hello::topla(15, 25)
    println(f"15 + 25 = {sonuc}")

    println("--- Test 3: Namespace Degiskeni ---")
    println(f"Surum: {Hello::surum}")

    println("--- Test 4: Ic Ice (Nested) Namespace ---")
    println(f"PI: {MathOps::Trig::pi()}")

    println("TUM NAMESPACE TESTLERI BASARILI!")
}
