@define BIND_IP "127.0.0.1"
@define PORT 8080.0

printn("Veritabanı sunucusuna bağlanılıyor...")
let client = netTcpConnect(BIND_IP, PORT)

if (isNil(client) or client < 0) {
    printn("HATA: Sunucuya bağlanılamadı. Sunucunun çalıştığından emin olun.")
    // scripti bitirmek için boş bir return atabiliriz veya sonsuz döngü koymayız
} else {
    printn("Bağlantı başarılı!")
    
    // Sunucudan gelen ilk karşılama mesajını oku
    let baslangic_mesaji = netTcpReceive(client, 2048.0)
    print(baslangic_mesaji)

    while (true) {
        let komut = input("") // Kullanıcıdan komut bekle
        
        if (komut == "exit") {
            break
        }
        
        netTcpSend(client, komut)
        
        let cevap = netTcpReceive(client, 2048.0)
        if (isNil(cevap) or cevap == "") {
            printn("Sunucu bağlantısı koptu.")
            break
        }
        
        print(cevap)
    }
    
    netTcpClose(client)
    printn("Bağlantı kapatıldı.")
}
