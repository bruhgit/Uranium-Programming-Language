let arr = [1, 2, 3]
printn("Initial length: " + arr.length)

arr.push(4)
arr.push(5)
printn("After push, length: " + arr.length + ", element 3: " + arr[3])

let popped = arr.pop()
printn("Popped element: " + popped)
printn("Length after pop: " + arr.length)

arr.insert(1, 99)
printn("After insert at index 1: " + arr[1])

let removed = arr.remove(1)
printn("Removed element: " + removed + ", element 1 is back to: " + arr[1])

arr.clear()
printn("Length after clear: " + arr.length)
