fn sum2(a, b) {
return a + b
}

fn sum3(a, b, c) {
return a + b + c
}

fn sum4(a, b, c, d) {
return a + b + c + d
}

fn mean2(a, b) {
return sum2(a, b) / 2
}

fn mean3(a, b, c) {
return sum3(a, b, c) / 3
}

fn mean4(a, b, c, d) {
return sum4(a, b, c, d) / 4
}

fn weighted2(a, weightA, b, weightB) {
return ((a * weightA) + (b * weightB)) / (weightA + weightB)
}

fn range2(a, b) {
return max(a, b) - min(a, b)
}

fn range3(a, b, c) {
return max(max(a, b), c) - min(min(a, b), c)
}

fn percent(part, total) {
return (part / total) * 100
}

fn ratio(a, b) {
return a / b
}

fn zScore(value, meanValue, stddev) {
return (value - meanValue) / stddev
}

fn midrange2(a, b) {
return (max(a, b) + min(a, b)) / 2
}
