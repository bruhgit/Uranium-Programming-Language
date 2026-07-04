// Uranium Collection and Advanced Data Structures Library
// Includes Set, Queue, Stack, and BigInt (implemented in Uranium script)

let arrayPush = push
let arrayPop = pop

// --- SET IMPLEMENTATION ---
class Set() {
    fn init() {
        this.items = map()
    }
    
    fn add(value) {
        let key = str(value)
        this.items[key] = value
        return this
    }
    
    fn has(value) {
        let key = str(value)
        return hasKey(this.items, key)
    }
    
    fn remove(value) {
        let key = str(value)
        if (hasKey(this.items, key)) {
            deleteKey(this.items, key)
            return true
        }
        return false
    }
    
    fn clear() {
        this.items = map()
    }
    
    fn size() {
        return len(keys(this.items))
    }
    
    fn values() {
        let result = []
        let allKeys = keys(this.items)
        let count = len(allKeys)
        for (let i = 0, i < count, i = i + 1) {
            let k = allKeys[i]
            arrayPush(result, this.items[k])
        }
        return result
    }
}

// --- QUEUE IMPLEMENTATION ---
class Queue() {
    fn init() {
        this.items = []
    }
    
    fn enqueue(value) {
        arrayPush(this.items, value)
        return this
    }
    
    fn dequeue() {
        if (this.isEmpty()) {
            throw "Queue underflow: cannot dequeue from an empty queue"
        }
        // Remove and return the first element
        let val = this.items[0]
        let newItems = []
        let count = len(this.items)
        for (let i = 1, i < count, i = i + 1) {
            arrayPush(newItems, this.items[i])
        }
        this.items = newItems
        return val
    }
    
    fn peek() {
        if (this.isEmpty()) {
            return nil
        }
        return this.items[0]
    }
    
    fn isEmpty() {
        return len(this.items) == 0
    }
    
    fn size() {
        return len(this.items)
    }
    
    fn clear() {
        this.items = []
    }
    
    fn values() {
        let result = []
        let count = len(this.items)
        for (let i = 0, i < count, i = i + 1) {
            arrayPush(result, this.items[i])
        }
        return result
    }
}

// --- STACK IMPLEMENTATION ---
class Stack() {
    fn init() {
        this.items = []
    }
    
    fn push(value) {
        arrayPush(this.items, value)
        return this
    }
    
    fn pop() {
        if (this.isEmpty()) {
            throw "Stack underflow: cannot pop from an empty stack"
        }
        return arrayPop(this.items)
    }
    
    fn peek() {
        if (this.isEmpty()) {
            return nil
        }
        let count = len(this.items)
        return this.items[count - 1]
    }
    
    fn isEmpty() {
        return len(this.items) == 0
    }
    
    fn size() {
        return len(this.items)
    }
    
    fn clear() {
        this.items = []
    }
    
    fn values() {
        let result = []
        let count = len(this.items)
        for (let i = 0, i < count, i = i + 1) {
            arrayPush(result, this.items[i])
        }
        return result
    }
}

class BigInt() {
    fn init(val) {
        this.digits = []
        this.negative = false
        
        let valType = typeOf(val)
        if (valType == "int" or valType == "number") {
            let num = val
            if (num < 0) {
                this.negative = true
                num = -num
            }
            if (num == 0) {
                arrayPush(this.digits, 0)
            } else {
                while (num > 0) {
                    arrayPush(this.digits, num % 10)
                    num = (num - (num % 10)) / 10
                }
            }
        } elif (valType == "string") {
            let strVal = val
            let start = 0
            let length = len(strVal)
            if (length > 0 and strVal[0] == "-") {
                this.negative = true
                start = 1
            }
            // Parse from end to start to store least significant digit first
            for (let i = length - 1, i >= start, i = i - 1) {
                let digitChar = strVal[i]
                let digitInt = toNumber(digitChar)
                arrayPush(this.digits, digitInt)
            }
            if (len(this.digits) == 0) {
                arrayPush(this.digits, 0)
            }
            this.normalize()
        } elif (valType == "instance") {
            // Assume copying another BigInt
            this.digits = []
            let count = len(val.digits)
            for (let i = 0, i < count, i = i + 1) {
                arrayPush(this.digits, val.digits[i])
            }
            this.negative = val.negative
        } else {
            arrayPush(this.digits, 0)
        }
    }
    
    fn normalize() {
        // Remove trailing zeros
        while (len(this.digits) > 1 and this.digits[len(this.digits) - 1] == 0) {
            arrayPop(this.digits)
        }
        if (len(this.digits) == 1 and this.digits[0] == 0) {
            this.negative = false
        }
    }
    
    fn toString() {
        let result = ""
        if (this.negative) {
            result = "-"
        }
        let count = len(this.digits)
        for (let i = count - 1, i >= 0, i = i - 1) {
            result = result + str(this.digits[i])
        }
        return result
    }
    
    fn add(other) {
        let bOther = BigInt(other)
        
        if (this.negative == bOther.negative) {
            let result = BigInt(0)
            result.digits = []
            result.negative = this.negative
            
            let carry = 0
            let i = 0
            let lenA = len(this.digits)
            let lenB = len(bOther.digits)
            
            while (i < lenA or i < lenB or carry > 0) {
                let sum = carry
                if (i < lenA) {
                    sum = sum + this.digits[i]
                }
                if (i < lenB) {
                    sum = sum + bOther.digits[i]
                }
                arrayPush(result.digits, sum % 10)
                carry = (sum - (sum % 10)) / 10
                i = i + 1
            }
            result.normalize()
            return result
        } else {
            // One is positive, one is negative
            // We do absolute subtraction
            if (this.negative) {
                // -A + B = B - A
                let tempA = BigInt(this)
                tempA.negative = false
                return bOther.subtract(tempA)
            } else {
                // A + (-B) = A - B
                let tempB = BigInt(bOther)
                tempB.negative = false
                return this.subtract(tempB)
            }
        }
    }
    
    fn compareAbs(other) {
        let lenA = len(this.digits)
        let lenB = len(other.digits)
        if (lenA != lenB) {
            if (lenA < lenB) return -1
            return 1
        }
        for (let i = lenA - 1, i >= 0, i = i - 1) {
            if (this.digits[i] != other.digits[i]) {
                if (this.digits[i] < other.digits[i]) return -1
                return 1
            }
        }
        return 0
    }
    
    fn subtract(other) {
        let bOther = BigInt(other)
        
        if (this.negative != bOther.negative) {
            // A - (-B) = A + B
            let tempB = BigInt(bOther)
            tempB.negative = this.negative
            return this.add(tempB)
        }
        
        // Both have the same sign
        let cmp = this.compareAbs(bOther)
        if (cmp == 0) {
            return BigInt(0)
        }
        
        let result = BigInt(0)
        result.digits = []
        
        let larger = this
        let smaller = bOther
        if (cmp < 0) {
            larger = bOther
            smaller = this
            result.negative = !this.negative
        } else {
            result.negative = this.negative
        }
        
        let borrow = 0
        let i = 0
        let lenL = len(larger.digits)
        let lenS = len(smaller.digits)
        
        while (i < lenL) {
            let diff = larger.digits[i] - borrow
            if (i < lenS) {
                diff = diff - smaller.digits[i]
            }
            if (diff < 0) {
                diff = diff + 10
                borrow = 1
            } else {
                borrow = 0
            }
            arrayPush(result.digits, diff)
            i = i + 1
        }
        result.normalize()
        return result
    }
    
    fn multiply(other) {
        let bOther = BigInt(other)
        let result = BigInt(0)
        
        let lenA = len(this.digits)
        let lenB = len(bOther.digits)
        
        // Initialize result digits to 0
        let totalLen = lenA + lenB
        result.digits = []
        for (let i = 0, i < totalLen, i = i + 1) {
            arrayPush(result.digits, 0)
        }
        
        for (let i = 0, i < lenA, i = i + 1) {
            let carry = 0
            for (let j = 0, j < lenB or carry > 0, j = j + 1) {
                let idx = i + j
                // Ensure we have enough capacity in result.digits
                while (idx >= len(result.digits)) {
                    arrayPush(result.digits, 0)
                }
                
                let current = result.digits[idx] + carry
                if (j < lenB) {
                    current = current + (this.digits[i] * bOther.digits[j])
                }
                result.digits[idx] = current % 10
                carry = (current - (current % 10)) / 10
            }
        }
        
        result.negative = (this.negative != bOther.negative)
        result.normalize()
        return result
    }
}
