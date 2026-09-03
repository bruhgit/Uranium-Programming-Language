@define BIND_IP "127.0.0.1"
@define PORT 8080.0

// 1. Veritabanını aç ve tabloyu oluştur
let db = dbOpen("users.db")
dbExecute(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age INTEGER)")

// 2. TCP Sunucusunu başlat
let server = netTcpListen(BIND_IP, PORT)
printn("Veritabanı Shell Sunucusu Başlatıldı!")
printn("Bağlanmak için terminalden şu komutu girin: nc " + BIND_IP + " " + PORT)

// 3. İstemcileri dinle
while (true) {
    let client = netTcpAccept(server)
    if (client < 0) {
        continue
    }

    printn("Yeni bir istemci bağlandı!")
    
    netTcpSend(client, "===================================\n")
    netTcpSend(client, "   Uranium DB Shell Sunucusu v1.0  \n")
    netTcpSend(client, "===================================\n")
    netTcpSend(client, "Aktif Tablo: users (id, name, age)\n")
    netTcpSend(client, "Ornek Komutlar:\n")
    netTcpSend(client, "  INSERT INTO users (name, age) VALUES ('Ahmet', 25);\n")
    netTcpSend(client, "  SELECT * FROM users;\n")
    netTcpSend(client, "  UPDATE users SET age=26 WHERE id=1;\n")
    netTcpSend(client, "Cikmak icin baglantiyi koparin.\n\n> ")

    while (true) {
        let sql = netTcpReceive(client, 1024.0)
        
        if (isNil(sql) or sql == "") {
            break // İstemci bağlantıyı kopardı
        }

        // Önce komutu bir "Query" (Sorgu) olarak çalıştırmayı deneriz
        let rows = dbQuery(db, sql)
        
        if (!isNil(rows)) {
            // Eğer sorgu başarılıysa ve tablo döndürdüyse
            netTcpSend(client, "Sonuclar (" + rows.length + " satir):\n")
            let i = 0
            while (i < rows.length) {
                let r = rows[i]
                netTcpSend(client, "  [" + r.id + "] " + r.name + " (" + r.age + " yasinda)\n")
                i = i + 1
            }
        } else {
            // Sorgu başarısızsa, belki bu bir INSERT/UPDATE/DELETE komutudur
            let ok = dbExecute(db, sql)
            if (ok) {
                netTcpSend(client, "Komut basariyla calistirildi.\n")
            } else {
                netTcpSend(client, "HATA: Gecersiz SQL komutu veya soz dizimi.\n")
            }
        }
        
        netTcpSend(client, "> ")
    }
    
    printn("Istemci baglantiyi kesti.")
    netTcpClose(client)
}
