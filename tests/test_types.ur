// Uranium - Null-Safety (T?) & Union Types (A | B) Test

fn yazdir_id(id: Int | String) {
    println(f"ID Gecerli: {id}")
}

fn selamla(isim: String?) {
    if (isim != nil) {
        println(f"Merhaba {isim}")
    } else {
        println("Merhaba misafir!")
    }
}

fn topla_opsiyonel(a: Int, b: Int?): Int {
    if (b != nil) {
        return a + b
    }
    return a
}

fn main() {
    println("--- Test 1: Union Types (Int | String) ---")
    let id1: Int | String = 1001
    let id2: Int | String = "USR-992"
    yazdir_id(id1)
    yazdir_id(id2)
    yazdir_id(42)
    yazdir_id("ABC-XYZ")

    println("--- Test 2: Nullable Types (String?) & Null-Safety ---")
    let adi: String? = "Omer"
    let soyadi: String? = nil
    selamla(adi)
    selamla(soyadi)

    println("--- Test 3: Fonksiyon Nullable Parametre ---")
    println(f"Sonuc 1: {topla_opsiyonel(10, 5)}")
    println(f"Sonuc 2: {topla_opsiyonel(10, nil)}")

    println("TUM TIP SISTEMI (UNION & NULLABLE) TESTLERI BASARILI!")
}
