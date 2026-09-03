import assert as assert

class main() {
let plain = "uranium-cross-platform"
let encoded = cryptoBase64Encode(plain)
assert.equal(cryptoBase64Decode(encoded), plain, "base64 roundtrip should work")

let digest = cryptoHashSha256("abc")
assert.equal(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 should be stable")

let key = "portable-key"
let cipher = cryptoAesEncrypt(key, plain)
let restored = cryptoAesDecrypt(key, cipher)
assert.equal(restored, plain, "aes roundtrip should work")

print("crypto-portability-ok")
}
