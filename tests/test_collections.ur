import std as std

class main() {
    print("=== Set Test ===")
    let s = std.Set()
    s.add(10)
    s.add(20)
    s.add(10) // Duplicate
    print("Set size (should be 2): " + str(s.size()))
    print("Set has 10: " + str(s.has(10)))
    print("Set has 30: " + str(s.has(30)))
    s.remove(10)
    print("Set has 10 after removal: " + str(s.has(10)))
    print("Set values: " + str(s.values()))

    print("\n=== Queue Test ===")
    let q = std.Queue()
    q.enqueue("first")
    q.enqueue("second")
    print("Queue size: " + str(q.size()))
    print("Queue peek: " + str(q.peek()))
    print("Queue dequeue: " + str(q.dequeue()))
    print("Queue dequeue: " + str(q.dequeue()))
    print("Queue isEmpty: " + str(q.isEmpty()))

    print("\n=== Stack Test ===")
    let st = std.Stack()
    st.push(100)
    st.push(200)
    print("Stack size: " + str(st.size()))
    print("Stack peek: " + str(st.peek()))
    print("Stack pop: " + str(st.pop()))
    print("Stack pop: " + str(st.pop()))
    print("Stack isEmpty: " + str(st.isEmpty()))

    print("\n=== BigInt Test ===")
    let a = std.BigInt("123456789012345678901234567890")
    let b = std.BigInt("987654321098765432109876543210")
    
    let sum = a.add(b)
    print("Sum: " + sum.toString())
    
    let diff = b.subtract(a)
    print("Diff: " + diff.toString())
    
    let prod = a.multiply(std.BigInt("123456789"))
    print("Product: " + prod.toString())
}
