import encoding

printn("=== ENCODING TEST ===")
let original = "Uranium Türkçe ÇĞŞİÖÜ"

let utf16_le = encoding.encode(original, "utf-16le")
let utf16_be = encoding.encode(original, "utf-16be")
let utf32_le = encoding.encode(original, "utf-32le")
let iso = encoding.encode(original, "iso-8859-1")

printn("Orijinal: " + original)
let dec1 = encoding.decode(utf16_le, "utf-16le")
printn("UTF-16 LE Decode: " + dec1)
let dec2 = encoding.decode(utf16_be, "utf-16be")
printn("UTF-16 BE Decode: " + dec2)
let dec3 = encoding.decode(utf32_le, "utf-32le")
printn("UTF-32 LE Decode: " + dec3)
let dec4 = encoding.decode(iso, "iso-8859-1")
printn("ISO-8859-1 Decode: " + dec4)

if (encoding.decode(utf16_le, "utf-16le") == original) {
    printn("UTF-16 LE Test: BASARILI")
} else {
    printn("UTF-16 LE Test: BASARISIZ")
}
