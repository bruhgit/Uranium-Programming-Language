from "../math.ur" import square, average2, cube

fn distance2(x, y) {
return sqrt(square(x) + square(y))
}

fn distance3(x, y, z) {
return sqrt(square(x) + square(y) + square(z))
}

fn ringArea(outer, inner) {
return PI * (square(outer) - square(inner))
}

fn midpoint(a, b) {
return average2(a, b)
}

fn rectangleArea(width, height) {
return width * height
}

fn rectanglePerimeter(width, height) {
return (width * 2) + (height * 2)
}

fn triangleArea(base, height) {
return (base * height) / 2
}

fn trapezoidArea(a, b, height) {
return ((a + b) * height) / 2
}

fn boxVolume(width, height, depth) {
return width * height * depth
}

fn cylinderVolume(radius, height) {
return PI * square(radius) * height
}

fn cylinderSurfaceArea(radius, height) {
return (2 * PI * radius * height) + (2 * PI * square(radius))
}

fn coneVolume(radius, height) {
return (PI * square(radius) * height) / 3
}

fn sphereVolume(radius) {
return (4 * PI * cube(radius)) / 3
}
