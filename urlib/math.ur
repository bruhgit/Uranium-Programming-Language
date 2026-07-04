const EPSILON = 0.000001
const GOLDEN_RATIO = PHI

fn square(x) {
return x * x
}

fn cube(x) {
return x * x * x
}

fn quartic(x) {
return square(square(x))
}

fn average2(a, b) {
return (a + b) / 2
}

fn average3(a, b, c) {
return (a + b + c) / 3
}

fn average4(a, b, c, d) {
return (a + b + c + d) / 4
}

fn lerp(a, b, t) {
return a + ((b - a) * t)
}

fn inverseLerp(a, b, value) {
return (value - a) / (b - a)
}

fn remap(value, inMin, inMax, outMin, outMax) {
return outMin + (((value - inMin) / (inMax - inMin)) * (outMax - outMin))
}

fn circleArea(radius) {
return PI * square(radius)
}

fn circleCircumference(radius) {
return TAU * radius
}

fn sphereSurfaceArea(radius) {
return 4 * PI * square(radius)
}

fn sphereVolume(radius) {
return (4 * PI * cube(radius)) / 3
}

fn hypotenuse(a, b) {
return sqrt(square(a) + square(b))
}

fn distance3(x, y, z) {
return sqrt(square(x) + square(y) + square(z))
}

fn triangleArea(base, height) {
return (base * height) / 2
}
