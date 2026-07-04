fn fail(message) {
throw "assertion failed: " + message
}

fn ok(condition, message) {
if (!condition) {
fail(message)
}
return true
}

fn equal(left, right, message) {
if (left != right) {
fail(message + " | left=" + str(left) + " right=" + str(right))
}
return true
}

fn notEqual(left, right, message) {
if (left == right) {
fail(message + " | both=" + str(left))
}
return true
}

fn isNil(value, message) {
if (typeOf(value) != "nil") {
fail(message + " | value=" + str(value))
}
return true
}

fn isNumberValue(value, message) {
if (!isNumber(value)) {
fail(message + " | type=" + typeOf(value))
}
return true
}

fn isStringValue(value, message) {
if (!isString(value)) {
fail(message + " | type=" + typeOf(value))
}
return true
}

fn throws(action, message) {
try {
action()
} catch (err) {
return err
}

fail(message)
return nil
}
