fn collect() {
gcCollect()
return nil
}

fn stats() {
return gcStats()
}

fn objects() {
return stats().objects
}

fn bytes() {
return stats().bytes
}

fn thresholdBytes() {
return stats().thresholdBytes
}

fn collections() {
return stats().collections
}

fn youngObjects() {
return stats().youngObjects
}

fn oldObjects() {
return stats().oldObjects
}

fn minorCollections() {
return stats().minorCollections
}

fn fullCollections() {
return stats().fullCollections
}
