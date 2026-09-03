const SECOND = 1
const MINUTE = 60
const HOUR = 3600
const DAY = 86400
const WEEK = 604800
const YEAR = 31557600

fn nowSeconds() {
return clock()
}

fn nowMillis() {
return unixMillis()
}

fn minutesToSeconds(minutes) {
return minutes * MINUTE
}

fn hoursToSeconds(hours) {
return hours * HOUR
}

fn daysToSeconds(days) {
return days * DAY
}

fn weeksToSeconds(weeks) {
return weeks * WEEK
}

fn secondsToMinutes(seconds) {
return seconds / MINUTE
}

fn secondsToHours(seconds) {
return seconds / HOUR
}

fn secondsToDays(seconds) {
return seconds / DAY
}
