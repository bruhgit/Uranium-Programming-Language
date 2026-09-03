import std as std

fn head<T>(values: Array<T>): T {
return values[0]
}

fn scoreOf(record: Map<String, Number>): Number {
return record.score
}

class main() {
let first: Number = head([4, 5, 6])
let score: Number = scoreOf(["score": 11, "bonus": 2])
print(str(first) + ":" + str(score))
}
