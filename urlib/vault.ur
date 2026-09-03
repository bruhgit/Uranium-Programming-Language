export const API_VERSION = "vault@1"

private const PREFIX = "sealed"

private fn encode(code) {
return PREFIX + ":" + code
}

let openCount = 0

export fn reveal(code) {
openCount = openCount + 1
return encode(code) + "#" + str(openCount)
}

export class Vault() {
fn init(code) {
this.code = code
}

fn open() {
return reveal(this.code)
}
}
