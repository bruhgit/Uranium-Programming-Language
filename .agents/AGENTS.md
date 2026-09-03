# Uranium — Kod Tabanı Değerlendirmesi ve Yol Haritası

Bu belge, 22 Ağustos 2026 tarihinde yapılan kaynak kod taramasının sonucudur. Uranium; C++17 ile yazılmış, bytecode VM, GC, async görev zamanlayıcısı, tip doğrulaması, optimizer, JIT/AOT, LSP, formatter/linter, paket yöneticisi ve geniş bir yerel standart kütüphane sağlayan olgun bir dil çalışma zamanı konumundadır.

## Mevcut güçlü yönler

- Dil çekirdeği: lexer, derleyici, bytecode (`chunk`), VM, nesne modeli, GC ve tip sistemi ayrı modüllerde bulunuyor.
- Çalıştırma modelleri: yorumlayıcı, `.urc` derleme, Windows AOT ikili üretimi, JIT hızlı yol ve WebAssembly hedefi mevcut.
- Geliştirici deneyimi: `--fmt`, `--fmt-check`, `--lint`, etkileşimli debugger, `--lsp` ve `UMake` desteği var.
- Ekosistem: yerel registry, lockfile, paket arşivi ve paket başlatma komutları; HTTP, ağ, SQLite, kripto, FFI, iş parçacığı, GUI ve Godot köprüleri bulunuyor.
- Test varlığı: `tests/` altında dil özelliklerini ve çoğu çalışma zamanı alanını kapsayan çok sayıda `.ur` senaryosu var.

## Öncelik 0 — Güvenlik ve güvenilirlik

### 1. Paket bütünlüğünü kriptografik hâle getir

`src/package_manager.cpp`, paket bütünlüğü için `fnv64` kullanıyor. FNV hızlı bir sağlama toplamıdır; kasıtlı dosya değiştirmeye karşı güvenli değildir.

- `sha256:<hex>` biçiminde içerik hash'i kullanın; mümkünse bağımsız imzalar (Ed25519) ekleyin.
- Hash kapsamını deterministik yapın: sıralı göreli yol, dosya türü, dosya içeriği ve paket metadatası.
- `--publish`, `--install`, `--pack` akışlarına doğrulama ve imzacı/anahtar kimliği ekleyin.
- Geriye uyumluluk için `fnv64` lockfile'larını okunabilir tutup yeni üretimleri SHA-256 yapın.

Kabul ölçütü: değiştirilmiş veya imzası geçersiz bir paket, kopyalanmadan önce anlaşılır hata ile reddedilir.

### 2. Kripto API'sini güvenli varsayılanlara taşı

`src/crypto_native.cpp` AES-CBC ve sabit sıfır IV kullanıyor; ayrıca paroladan anahtar türetme platformlar arasında farklı ve modern bir parola türetme fonksiyonu değil.

- Yeni API'de AES-256-GCM veya ChaCha20-Poly1305 kullanın.
- Her şifreleme için rastgele nonce/IV ve tuz üretin; çıktı biçimine sürüm, algoritma, tuz, nonce ve doğrulama etiketi koyun.
- Parola tabanlı kullanımda Argon2id (tercih edilen) ya da PBKDF2-HMAC-SHA-256 kullanın.
- Mevcut CBC verisi için yalnızca açıkça adlandırılmış `legacy_*` çözme fonksiyonu bırakın.

Kabul ölçütü: aynı metin aynı anahtarla iki kez şifrelendiğinde farklı çıktı üretilir; değiştirilmiş şifreli veri doğrulama hatası verir.

### 3. FFI'yi izinli, tipli ve hata güvenli tasarla

`src/ffi_native.cpp` serbest dosya yolu yükleyebiliyor, çağrıları dört argümana kesiyor ve x64 makine kodu üreten bir trampoline kullanıyor. Şu an desteklenmeyen mimari/ABI, float, pointer sahipliği, null değer ve çağrı sözleşmesi açıkça temsil edilmiyor.

- FFI'yi varsayılan kapalı yapın; `--allow-ffi` / capability tabanlı izin ve izinli kütüphane dizinleri ekleyin.
- libffi ya da hedefe göre derlenen, tipli ABI katmanı kullanın; argüman sayısını sessizce kırpmak yerine hata verin.
- `int32`, `int64`, `float`, `double`, `bool`, `pointer`, `cstring`, `buffer`, `void` tipleri ve çağrı sözleşmesini tanımlayın.
- Dinamik kitaplık tanıtıcısını RAII ile yönetin; `jit_alloc_executable()` null dönerse kontrollü hata verin.
- FFI çağrılarını ayrı süreç/sandbox seçeneğiyle çalıştırmayı değerlendirin.

Kabul ölçütü: desteklenmeyen imza güvenli biçimde reddedilir; ABI testleri Windows, Linux ve macOS'ta C fixture kitaplığıyla çalışır.

### 4. JIT bellek politikasını W^X kuralına yaklaştır

`src/native_jit_mem.cpp` Windows'ta bellek sayfasını önce `PAGE_EXECUTE_READWRITE` olarak ayırıyor. Kod yazımı tamamlanana kadar yazılabilir ama çalıştırılamaz, ardından yalnızca okunur/çalıştırılabilir sayfa tercih edilmelidir.

- Windows ayırmasını `PAGE_READWRITE` ile başlatın; derleme sonunda `PAGE_EXECUTE_READ`e geçin.
- Koruma geçişi ve instruction-cache temizleme sonuçlarını kontrol edin.
- JIT/FFI başarısızlıklarını hata değerine dönüştürün; null bellek üzerinde `memcpy` çağırmayın.

## Öncelik 1 — Kalite, test ve dağıtım

### 5. Tek komutla çalışan CI kalite kapısı kur

Kök `CMakeLists.txt` yalnızca hedef derliyor; CTest, sanitiser, uyarı politikası ve yayın/paketleme hedefleri yok. Ayrıca bu taramada `build/uranium.exe --test tests` komutu Windows giriş noktası hatasıyla (`0xC0000139`) başlamadan sonlandı; önce bu dağıtım/çalıştırma sorunu teşhis edilmelidir.

- CTest ekleyin: hızlı çekirdek testleri, CLI uçtan uca testleri ve mevcut `.ur` test süitini ayrı test hedefleri yapın.
- GitHub Actions veya eşdeğeri ile Windows/Linux/macOS için debug + release derlemesi çalıştırın.
- Clang/GCC'de ASan, UBSan; Windows'ta uygun AddressSanitizer yapılandırması ekleyin.
- CMake'de `-Wall -Wextra -Wpedantic` (MSVC karşılıkları dahil) ve seçili uyarılar için hata politikası kullanın.
- Derlenen çalıştırılabilir için bağımlılık denetimi yapın; `0xC0000139` türü başlatma hatası CI'da görünür olsun.

Kabul ölçütü: temiz bir CI makinesinde derleme, test, format kontrolü ve paket smoke testi tek komutla başarılıdır.

### 6. Test piramidini genişlet

Mevcut `.ur` senaryoları değerli; fakat C++ çekirdeği için birim/property/fuzz katmanı görünmüyor.

- Lexer, parser/derleyici, type system, optimizer, URC okuyucu-yazıcı ve paket manifesti için native birim testleri ekleyin.
- Fuzzer'lar: lexer, derleyici, URC yükleyici, manifest/lock JSON ayrıştırıcıları ve import çözümleyici.
- Diferansiyel test: optimize edilmemiş VM ile `--O1/--O2/--O3` ve JIT çıktıları aynı sonucu vermeli.
- GC stres modu, task scheduler yarış senaryoları, bozuk bytecode/bozuk paket regresyon fixture'ları ekleyin.
- Test adlarını `*_test.ur` standardında tutun; geçici `tmp_*` fixture'larını ayrı negatif-test klasörüne taşıyın.

### 7. Sürümleme ve yayın disiplini ekle

- Tek bir sürüm kaynağı oluşturun (CMake project version + `uranium --version` + paket şablonu).
- SemVer, değişiklik günlüğü ve uyumluluk politikası yayınlayın.
- Windows installer/zip, macOS/Linux paketleri ve SHA-256 yayın doğrulaması hazırlayın.
- `--version --json` ile otomasyonların derleyici sürümü, hedefler ve etkin özellikleri sorgulamasını sağlayın.

## Öncelik 2 — Ürün ve dil yetenekleri

### 8. LSP'yi proje ölçeğine taşı

`src/tooling.cpp` tanılama, formatlama, tanıma gitme, hover, completion ve document symbol sağlıyor. Yeniden adlandırma, referanslar, imza yardımı, kod aksiyonları ve çalışma alanı çapında indeksleme henüz sunulmuyor.

- `references`, `prepareRename`/`rename`, `signatureHelp`, `workspace/symbol`, semantic tokens ve code actions ekleyin.
- Dosya değişimiyle geçersizleşen, workspace kapsamlı kalıcı sembol indeksi tasarlayın.
- LSP tanılarını derleyicinin tip hataları ve import zinciriyle zenginleştirin.
- VS Code uzantısı için server sürümü uyumluluğunu denetleyen activation/health-check ekleyin.

### 9. Paket ekosistemini geliştir

Yerel registry ve semver çözümü iyi bir temel oluşturuyor.

- Uzak HTTPS registry protokolü, ayna (mirror), çevrimdışı önbellek ve proxy desteği ekleyin.
- Paket adları/sürümleri için sıkı doğrulama; atomik kurulum ve kilitli eşzamanlı erişim sağlayın.
- Paket kaynağı, lisans, açıklama, repository, yazar, hedef platform ve izin/capability metadatası ekleyin.
- `uranium audit`, `outdated`, `why`, `tree`, `search`, `login` komutlarını planlayın.
- Arşiv açma/kopyalama aşamasında symlink ve hedef yol kaçışı testlerini ekleyin.

### 10. Dil ergonomisi ve performans görünürlüğü

- Zengin hatalar: kaynak parçası, hata kodu, öneri, import/async task stack trace ve makro genişleme izi.
- Yerleşik benchmark profili: opcode sayıları, GC duraklaması, JIT sıcaklıkları, derleme süresi ve bellek zirvesi.
- `--profile` ile JSON/Chrome Trace çıktısı üretin.
- Stabil bir bytecode/URC sürüm alanı ve geriye uyumluluk matrisi ekleyin.
- Optimizasyonlar için her pass öncesi/sonrası doğrulama ve açıklanabilir `--emit-ir`/`--disassemble` çıktıları sunun.

## Önerilen uygulama sırası

1. Windows'taki test çalıştırılabilir başlatma hatasını giderin ve CI'a alın.
2. Paket bütünlüğünü SHA-256 + imza modeline taşıyın.
3. Kripto API'sini AEAD ve rastgele nonce ile yeniden sürümlendirin.
4. FFI/JIT güvenlik sınırlarını ve ABI testlerini tamamlayın.
5. Native unit + fuzz + optimizer diferansiyel testlerini ekleyin.
6. LSP yeniden adlandırma/referanslar ve uzak registry gibi ekosistem özelliklerine geçin.

## Mimarî ilkeler

- Güvenlik açısından hassas özellikler (FFI, ağ, dosya sistemi, süreç, dinamik yükleme) capability tabanlı ve varsayılan kapalı olmalıdır.
- Her yeni CLI özelliğinin `--help`, en az bir başarılı ve bir hatalı kullanım testi bulunmalıdır.
- Her optimizer/JIT değişikliği yorumlayıcıyla diferansiyel test edilmelidir.
- Platform koşullu kod Windows, Linux ve macOS'ta derlenmeli; mimariye özgü özellikler açık capability/target kontrolüyle korunmalıdır.
- Üçüncü taraf gömülü kaynaklar (ör. SQLite) sürüm, kaynak URL'si, lisans ve güncelleme politikasıyla belgelenmelidir.
