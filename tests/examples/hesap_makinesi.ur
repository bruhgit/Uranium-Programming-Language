printn("===========================================")
printn("       Uranium Hesap Makinesi              ")
printn("===========================================")
printn("Gelistirici: Absolute AI Architecture")
printn("")

// `optional` kelimesini kullanarak ucuncu parametreyi zorunlu olmaktan cikariyoruz.
// Eger `islem` parametresi verilmezse, otomatik olarak 'nil' (bos) olur.
fn hesapla(sayi1, sayi2, Optional islem) {
    let op = "+" // Varsayilan islem: Toplama

    // Eger islem parametresi gonderilmisse (nil degilse) onu kullan
    if (islem.is_nil != 0) {
        op = islem
    }
    
    if (op == "+") {
        return sayi1 + sayi2
    } else if (op == "-") {
        return sayi1 - sayi2
    } else if (op == "*") {
        return sayi1 * sayi2
    } else if (op == "/") {
        if (sayi2 == 0) {
            printn("Hata: Sifira bolme islemi yapilamaz!")
            return nil
        }
        return sayi1 / sayi2
    } else {
        printn("Hata: Gecersiz islem operatoru -> " + op)
        return nil
    }
}

fn main() {
    // 1. Durum: Islem belirtilmedi (Varsayilan olarak + yapilacak)
    let sonuc1 = hesapla(20, 5)
    printn("hesapla(20, 5)        => Sonuc: " + sonuc1 + " (Varsayilan islem '+')")

    // 2. Durum: Islem belirtildi (-)
    let sonuc2 = hesapla(20, 5, "-")
    printn("hesapla(20, 5, '-')   => Sonuc: " + sonuc2)

    // 3. Durum: Islem belirtildi (*)
    let sonuc3 = hesapla(20, 5, "*")
    printn("hesapla(20, 5, '*')   => Sonuc: " + sonuc3)

    // 4. Durum: Islem belirtildi (/)
    let sonuc4 = hesapla(20, 5, "/")
    printn("hesapla(20, 5, '/')   => Sonuc: " + sonuc4)

    // 5. Durum: Sifira Bolme Hatasi
    let sonuc5 = hesapla(20, 0, "/")
    if (sonuc5.is_nil == 0) {
        printn("hesapla(20, 0, '/')   => Islem Basarisiz (Sifira Bolme Hatasi)")
    }
}
