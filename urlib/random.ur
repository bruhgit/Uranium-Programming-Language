fn randomUnit() {
return random()
}

fn between(low, high) {
return randRange(low, high)
}

fn whole(low, high) {
return randInt(low, high)
}

fn dice6() {
return randInt(1, 6)
}

fn dice20() {
return randInt(1, 20)
}

fn coinFlipNumber() {
return randInt(0, 1)
}

fn randomAngle() {
return randRange(0, TAU)
}

fn jitter(value, amount) {
return value + randRange(-amount, amount)
}

fn seededInt(seed, low, high) {
seedRandom(seed)
return randInt(low, high)
}
