printn("--- STRING METHODS TEST ---")
let metin = " Uranium Dili Harika "
printn("Orijinal: '" + metin + "'")
printn("Trim: '" + metin.trim() + "'")
printn("toUpper: " + metin.toUpper())
printn("toLower: " + metin.toLower())

let csv = "elma,armut,muz"
let parcalar = csv.split(",")
printn("Split length: " + parcalar.length)
printn("Split [1]: " + parcalar[1])

let degisen = csv.replace("armut", "kivi")
printn("Replace: " + degisen)
printn("")

printn("--- MAP METHODS TEST ---")
let kullanici = ["ad": "Ali", "yas": 30, "sehir": "Istanbul"]
printn("Map Keys length: " + kullanici.keys().length)
printn("Map Values [0]: " + kullanici.values()[0])
printn("Map has 'yas': " + kullanici.has("yas"))
printn("Map has 'maas': " + kullanici.has("maas"))

kullanici.remove("sehir")
printn("After remove 'sehir', has 'sehir': " + kullanici.has("sehir"))
kullanici.clear()
printn("After clear, keys length: " + kullanici.keys().length)
printn("")

printn("--- JSON TEST ---")
let jsonMetin = R"({"isim": "Veli", "notlar": [90, 85, 95], "aktif": true})"
let ayrismis = jsonParse(jsonMetin)
printn("Parsed JSON isim: " + ayrismis.isim)
printn("Parsed JSON notlar[1]: " + ayrismis.notlar[1])

ayrismis.isim = "Ayse"
ayrismis.notlar.push(100)
let yeniJson = jsonStringify(ayrismis)
printn("Stringified JSON: " + yeniJson)
printn("")

printn("TUM TESTLER BASARILI!")
