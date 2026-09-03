# Uranium Programlama Dili: Resmi Spesifikasyon ve Geliştirici Kılavuzu

Uranium; yüksek performanslı, nesne yönelimli, asenkron ve C++ tabanlı modern bir betik dilidir. Güçlü bir sanal makine (VM), kademeli bytecode optimizasyonu (-O1, -O2, -O3), JIT (Just-In-Time) / AOT (Ahead-Of-Time) derleme mimarisi, yerleşik paket yöneticisi (bolt) ve zengin native entegrasyonlu standart kütüphanesi (urlib) ile profesyonel yazılım geliştirme süreçleri için tasarlanmıştır.

Bu döküman, Uranium dilinin tüm bileşenlerini, dil kurallarını (syntax), gelişmiş özelliklerini ve standart kütüphane API referansını eksiksiz şekilde içeren resmi kılavuzdur.

---

## İÇİNDEKİLER
1. [Sözdizimi ve Temel Yapılar](#1-sözdizimi-ve-temel-yapılar)
2. [Kontrol Akışı ve Örüntü Eşleme (Pattern Matching)](#2-kontrol-akışı-ve-örüntü-eşleme-pattern-matching)
3. [Nesne Yönelimli Programlama (OOP) ve Sözleşmeler](#3-nesne-yönelimli-programlama-oop-ve-sözleşmeler)
4. [Eşzamanlılık (Concurrency) ve Asenkron Programlama](#4-eşzamanlılık-concurrency-ve-asenkron-programlama)
5. [Paralel Çalışma (Multi-threading) ve İzole VM'ler](#5-paralel-çalışma-multi-threading-ve-izole-vmler)
6. [Dahili Paket Yönetimi ve Derleme (AOT/JIT)](#6-dahili-paket-yönetimi-ve-derleme-aotjit)
7. [Dahili Hata Ayıklayıcı (Interactive Debugger)](#7-dahili-hata-ayıklayıcı-interactive-debugger)
8. [Uranium Standart Kütüphanesi (urlib) API Referansı](#8-uranium-standart-kütüphanesi-urlib-api-referansı)

---

## 1. Sözdizimi ve Temel Yapılar

Uranium, temiz ve okunabilir bir C/Python melezi sözdizimine sahiptir. Noktalı virgül (`;`) kullanımı opsiyoneldir.

### Değişken Tanımlama ve Kapsam (Scope)
Değişkenler blok kapsamlıdır (lexical scoping). İki tür tanımlama anahtar kelimesi bulunur:
- `let`: Değeri sonradan değiştirilebilen değişkenler tanımlar.
- `const`: Yalnızca bir kez değer atanan salt okunur değişkenler tanımlar.

```uranium
let sayi = 42                 // Tip çıkarımı (implicit int)
let metin: String = "Uranium" // Açık tip tanımı (explicit String)
const LIMIT: float = 99.9     // Değiştirilemez ondalıklı sayı
```

### Veri Tipleri
Uranium, çalışma zamanında performansı maksimize etmek için verileri optimize edilmiş etiketli yapılar (`Value`) olarak saklar:

1. **`int`**: 64-bit işaretli tam sayılar. Bitwise işlemleri doğrudan bu tip üzerinde gerçekleştirilir.
2. **`float`**: 64-bit IEEE 754 çift duyarlıklı ondalıklı sayılar.
3. **`String`**: UTF-8 metin dizileri.
4. **`Bool`**: `true` ve `false` mantıksal değerleri.
5. **`Array`**: Dinamik olarak büyüyebilen, heterojen veya homojen diziler.
6. **`Map`**: String anahtarlı hash tabloları.
7. **`Nil`**: Değerin yokluğunu belirten `nil` sabiti.

### Operatörler
Aritmetik işlemlerin yanında, düşük seviyeli donanım manipülasyonları için bitwise operatörler sunulur:

| Operatör | Tanım | Örnek |
| :--- | :--- | :--- |
| `+`, `-`, `*`, `/` | Temel aritmetik işlemler | `5 + 2 // 7` |
| `%` | Modulo (Kalan). `int` ve `float` destekler. | `5 % 2 // 1` |
| `&` | Bitwise AND (Yalnızca `int`) | `5 & 3 // 1` |
| `\|` | Bitwise OR (Yalnızca `int`) | `5 \| 3 // 7` |
| `^` | Bitwise XOR (Yalnızca `int`) | `5 ^ 3 // 6` |
| `~` | Bitwise NOT (Tekli operatör, yalnızca `int`) | `~5 // -6` |
| `<<` | Sola Bit Kaydırma | `1 << 3 // 8` |
| `>>` | Sağa Bit Kaydırma | `8 >> 2 // 2` |

---

## 2. Kontrol Akışı ve Örüntü Eşleme

### Karar Yapıları
`if`, `elif` ve `else` blokları standart koşullu yürütme sağlar. Koşul ifadelerinde parantez kullanımı isteğe bağlıdır ancak blok gövdelerinde süslü parantez `{}` zorunludur.

```uranium
let sicaklik = 25
if sicaklik > 30 {
    print("Sıcak")
} elif sicaklik >= 15 {
    print("Ilık")
} else {
    print("Soğuk")
}
```

### Switch ve Match
`switch` ifadesi doğrudan değer eşitliklerini kontrol ederken; `match` ifadesi diziler, haritalar ve sınıflar üzerinde yapısal örüntü eşleme yapılmasına olanak tanır.

```uranium
let veri = [1, 2, 3]

match veri {
    case [1, x, 3] => {
        print("Orta eleman: " + str(x))
    }
    case { "tip": "kullanici", "ad": isim } => {
        print("Kullanıcı adı: " + isim)
    }
    case _ => {
        print("Eşleşme bulunamadı.")
    }
}
```

---

## 3. Nesne Yönelimli Programlama (OOP) ve Sözleşmeler

Uranium, sınıfları (class) tekli kalıtımı ve birden çok sözleşmeyi (interface/trait) destekleyecek şekilde tasarlamıştır.

### Sınıf Tanımlama
Sınıfların kurucu metodu `init` ismini alır. Sınıf içi özelliklere ve metotlara erişim `this` kelimesiyle yapılır.

```uranium
class Nokta {
    let x: float
    let y: float

    fn init(x: float, y: float) {
        this.x = x
        this.y = y
    }

    fn uzaklik() {
        return sqrt(this.x * this.x + this.y * this.y)
    }
}
```

### Arayüzler (Interfaces) ve Özellikler (Traits)
Sözleşmeler (`interface` ve `trait`), sınıfların uygulaması gereken metot imzalarını tanımlar. Uranium derleyicisi, bir sınıf arayüzü `implements` ettiğinde, metotların varlığını, parametre sayılarını (arity) ve `async` niteliklerini derleme zamanında sıkı şekilde denetler.

```uranium
interface Sekil {
    fn alan(): float
    fn cevre(): float
}

class Kare implements Sekil {
    let kenar: float

    fn init(kenar: float) {
        this.kenar = kenar
    }

    fn alan(): float {
        return this.kenar * this.KENAR
    }

    fn cevre(): float {
        return this.kenar * 4.0
    }
}
```

---

## 4. Eşzamanlılık (Concurrency) ve Asenkron Programlama

Uranium, I/O işlemlerinde bloke olmayı engellemek amacıyla **asenkron görev (Task)** tabanlı bir eşzamanlılık modeline sahiptir. `async fn` ile tanımlanan fonksiyonlar çağrıldığında doğrudan çalışmaz, bir `Task` nesnesi döndürür. Bu görevler `await` anahtar kelimesi ile beklenir.

```uranium
async fn veriIndir(url: String): String {
    print("İndirme başladı: " + url)
    // Bloklamayan uyku simülasyonu
    await sleepAsync(1000) 
    return "Veri içeriği"
}

class main {
    fn init() {
        let gorev = veriIndir("https://uranium-lang.org")
        let sonuc = await gorev
        print("Sonuç: " + sonuc)
    }
}
```

---

## 5. Paralel Çalışma (Multi-threading) ve İzole VM'ler

Gerçek paralel işlemci gücünü kullanmak için Uranium, ana thread'i bloke etmeyen izole işletim sistemi thread'leri oluşturabilir. Her thread kendi çöp toplayıcısına (GC) ve bellek alanına sahip izole birer VM örneğidir. İletişim, thread'ler arası mesaj kanalları (channels) üzerinden gerçekleştirilir.

```uranium
// worker.ur dosyası
let kanal = threadChannelReceive()
print("Worker: Mesaj alındı -> " + kanal)

// main.ur dosyası
class main {
    fn init() {
        let threadId = threadSpawn("worker.ur")
        let kanal = threadChannelCreate(threadId)
        threadChannelSend(kanal, "Merhaba paralel dünya!")
        threadJoin(threadId)
    }
}
```

---

## 6. Dahili Paket Yönetimi ve Derleme (AOT/JIT)

### Paket Yapısı (`uranium.pkg`)
Bir projenin bağımlılıkları ve giriş noktası projenin kök dizinindeki `uranium.pkg` dosyasında bildirilir:

```json
{
  "name": "web_server",
  "version": "1.0.0",
  "entry": "src/main.ur",
  "dependencies": {
    "http_parser": "^2.0.0"
  }
}
```

### Derleme Seviyeleri ve Optimizasyonlar
Derleyici, bytecode üretim aşamasında optimizasyon seviyelerine göre kodu dönüştürür:
- `--O0`: Optimizasyonlar kapalıdır. Hızlı derleme sağlar.
- `--O1`: Sabit katlama (Constant Folding) ve ölü kod temizliği yapar.
- `--O2`: O1 optimizasyonlarına ek olarak Jump Threading ve gereksiz atlamaları (BOC) temizler.
- `--O3`: En agresif seviyedir. İşlem gücü azaltma (Strength Reduction) ve bytecode sıkıştırma (NOP compaction) uygular.

Uranium derleyicisi ile doğrudan çalıştırılabilir ikili (AOT Binary) üretmek için:
```bash
uranium src/main.ur --compile web_server.exe --O3
```

---

## 7. Dahili Hata Ayıklayıcı (Interactive Debugger)

Uranium derleyicisi ve VM'i, kodun çalışma anında durdurulup incelenebilmesi için `debugger` anahtar kelimesini destekler. Kod yürütülürken `debugger` ifadesine rastlandığında VM duraklatılır ve interaktif hata ayıklama konsolu `(db)` açılır.

```uranium
fn islem(x: int) {
    let y = x * 10
    debugger // Program burada duracak.
    return y
}
```

### Hata Ayıklayıcı Komut Tablosu
| Komut | Alternatif | Tanım |
| :--- | :--- | :--- |
| `c` | `continue` | Programın yürütülmesine normal şekilde devam eder. |
| `bt` | `backtrace` | Aktif çağrı yığınını (call stack) dosya adı ve satır numaralarıyla listeler. |
| `print <ad>` | | Belirtilen global değişkenin değerini ekrana yazdırır. |
| `print slot<N>` | | Mevcut fonksiyon çerçevesindeki (stack frame) N numaralı yerel değişken slotunu gösterir. (Örn: `print slot0`) |
| `q` | `quit` | Programın yürütülmesini derhal sonlandırır ve VM'den çıkar. |

---

## 8. Uranium Standart Kütüphanesi (urlib) API Referansı

Standart kütüphane fonksiyonları, native C++ kodlarına doğrudan bağlı yüksek performanslı araçlar sunar.

### 8.1. Matematik Modülü (`math`)
Matematiksel hesaplamalar için kullanılan dahili fonksiyonlar.

- **`abs(x: Number): Number`**: Sayının mutlak değerini döner.
- **`sqrt(x: Number): Number`**: Sayının karekökünü hesaplar.
- **`pow(taban: Number, us: Number): Number`**: Üs alma işlemi gerçekleştirir.
- **`sin(rad: float): float`** / **`cos(rad: float): float`** / **`tan(rad: float): float`**: Trigonometrik fonksiyonlar.
- **`random(): float`**: `0.0` ile `1.0` arasında rastgele bir ondalıklı sayı üretir.
- **`randInt(min: int, max: int): int`**: Belirtilen aralıkta rastgele bir tam sayı üretir.

### 8.2. Dosya Sistemi Modülü (`fs`)
Dosya ve dizin işlemleri için senkron fonksiyonlar.

- **`fsReadText(path: String): String`**: Belirtilen dosyanın içeriğini metin olarak okur.
- **`fsWriteText(path: String, icerik: String): Bool`**: Dosyaya metin yazar (varsa üzerine yazar).
- **`fsAppendText(path: String, icerik: String): Bool`**: Dosyanın sonuna metin ekler.
- **`fsExists(path: String): Bool`**: Dosya veya dizinin varlığını kontrol eder.
- **`fsListNames(path: String): Array<String>`**: Dizin içeriğindeki dosya ve klasör isimlerini listeler.
- **`fsRemove(path: String): Bool`**: Belirtilen dosyayı siler.
- **`fsRemoveTree(path: String): Bool`**: Belirtilen dizini ve altındaki tüm ögeleri rekürsif olarak siler.

### 8.3. Ağ Programlama Modülü (`net`)
TCP/IP üzerinden soket haberleşmesi sağlayan fonksiyonlar.

- **`netTcpListen(ip: String, port: int): Map`**: Belirtilen IP ve port üzerinde TCP sunucusu başlatır.
- **`netTcpAccept(server: Map): Map`**: Bağlanmak isteyen bir istemciyi kabul eder (bloke edici).
- **`netTcpReceive(client: Map, size: int): String`**: İstemciden belirtilen boyuta kadar veri okur.
- **`netTcpSend(client: Map, veri: String): int`**: İstemciye metinsel veri gönderir.
- **`netTcpClose(socket: Map): Nil`**: Soket bağlantısını kapatır.

### 8.4. Veritabanı Modülü (`db`)
SQLite3 veritabanı işlemlerini gerçekleştiren native modül.

- **`dbOpen(path: String): Map`**: Belirtilen yoldaki SQLite veritabanını açar (yoksa oluşturur).
- **`dbExecute(db: Map, sql: String): Bool`**: Veritabanı üzerinde veri döndürmeyen SQL komutları (CREATE, INSERT, UPDATE) çalıştırır.
- **`dbQuery(db: Map, sql: String): Array<Map>`**: SELECT sorguları çalıştırır ve sonuçları bir dizi Harita (Map) olarak döner.
- **`dbClose(db: Map): Nil`**: Veritabanı bağlantısını güvenli bir şekilde kapatır.

### 8.5. Grafik Arayüz Modülü (`gui`)
İşletim sistemi pencereleri ve arayüz elemanları oluşturmak için kullanılan native GUI kütüphanesi.

- **`guiCreateWindow(baslik: String, genislik: int, yukseklik: int): Map`**: Yeni bir uygulama penceresi oluşturur.
- **`guiAddButton(window: Map, x: int, y: int, g: int, y: int, metin: String): Map`**: Pencereye buton ekler.
- **`guiAddInput(window: Map, x: int, y: int, g: int, y: int, varsayilan: String): Map`**: Pencereye metin girdi alanı ekler.
- **`guiPollEvent(): Map`**: Oluşan son arayüz olayını (tıklama, tuş basımı vb.) kuyruktan çeker.

---
*Uranium Programlama Dili Resmi Spesifikasyonu - 2026*
