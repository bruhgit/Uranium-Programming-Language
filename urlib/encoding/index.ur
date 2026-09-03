// Encoding module for Uranium

// Decodes a binary string into a UTF-8 string according to the specified encoding type.
// Supported types: "utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "iso-8859-1" (or "ascii").
fn decode(data, type) {
    return encodingDecode(data, type)
}

// Encodes a UTF-8 string into a binary string according to the specified encoding type.
// Supported types: "utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "iso-8859-1" (or "ascii").
fn encode(text, type) {
    return encodingEncode(text, type)
}
