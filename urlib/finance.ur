fn percentOf(amount, rate) {
return amount * (rate / 100)
}

fn interestOnly(principal, rate, time) {
return principal * ((rate / 100) * time)
}

fn simpleInterest(principal, rate, time) {
return principal + interestOnly(principal, rate, time)
}

fn compoundInterest(principal, rate, periods) {
return principal * pow(1 + (rate / 100), periods)
}

fn discount(price, rate) {
return price - percentOf(price, rate)
}

fn markup(cost, rate) {
return cost + percentOf(cost, rate)
}

fn taxAmount(amount, rate) {
return percentOf(amount, rate)
}

fn totalWithTax(amount, rate) {
return amount + taxAmount(amount, rate)
}

fn profit(revenue, cost) {
return revenue - cost
}

fn marginPercent(revenue, cost) {
return ((revenue - cost) / revenue) * 100
}

fn monthlyToYearly(amount) {
return amount * 12
}

fn yearlyToMonthly(amount) {
return amount / 12
}

fn netChange(startValue, endValue) {
return endValue - startValue
}

fn growthPercent(startValue, endValue) {
return ((endValue - startValue) / startValue) * 100
}
