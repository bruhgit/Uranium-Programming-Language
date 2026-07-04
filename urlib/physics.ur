const EARTH_GRAVITY = 9.80665

fn velocity(distance, time) {
return distance / time
}

fn acceleration(deltaVelocity, time) {
return deltaVelocity / time
}

fn force(mass, accelerationValue) {
return mass * accelerationValue
}

fn momentum(mass, velocityValue) {
return mass * velocityValue
}

fn kineticEnergy(mass, velocityValue) {
return (mass * velocityValue * velocityValue) / 2
}

fn potentialEnergy(mass, height, gravity) {
return mass * gravity * height
}

fn work(forceValue, distance) {
return forceValue * distance
}

fn powerRate(workValue, time) {
return workValue / time
}

fn pressure(forceValue, area) {
return forceValue / area
}

fn density(mass, volume) {
return mass / volume
}

fn waveSpeed(frequency, wavelength) {
return frequency * wavelength
}

fn ohmsCurrent(voltage, resistance) {
return voltage / resistance
}

fn ohmsVoltage(current, resistance) {
return current * resistance
}

fn ohmsResistance(voltage, current) {
return voltage / current
}
