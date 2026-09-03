printn("===========================================")
printn("       Uranium Web Server v1.0             ")
printn("===========================================")
printn("Baslatiliyor...")

let host = "127.0.0.1"
let port = 8080.0
let server_socket = netTcpListen(host, port)

if (server_socket.is_nil == 0) {
    printn("HATA: Sunucu baslatilamadi. Port " + port + " kullanimda olabilir.")
} else {
    printn("Sunucu basariyla baslatildi!")
    printn("Tarayicinizi acin ve su adrese gidin:")
    printn("-> http://127.0.0.1:8080")
    printn("-------------------------------------------")
    printn("Gelen istekler bekleniyor...\n")

    while (true) {
        let client = netTcpAccept(server_socket)
        if (client.is_nil != 0) {
            let request = netTcpReceive(client, 4096.0)
            
            if (request.is_nil != 0 and request.length > 0) {
                // Sadece ilk satiri (HTTP metodunu) yazdiralim kalabalik olmasin
                let lines = request.split("\n")
                if (lines.length > 0) {
                    printn("[ISTEK] " + lines[0].trim())
                }
                
                // Uranium gucunu gosteren guzel bir HTML sayfasi hazirlayalim
                let html = R"(
                    <!DOCTYPE html>
                    <html>
                    <head>
                        <title>Uranium Server</title>
                        <style>
                            body {
                                background-color: #121212;
                                color: #00ffcc;
                                font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
                                text-align: center;
                                padding-top: 100px;
                            }
                            h1 {
                                font-size: 50px;
                                text-shadow: 0 0 20px #00ffcc;
                            }
                            p {
                                font-size: 20px;
                                color: #ccc;
                            }
                            .box {
                                background: #1e1e1e;
                                padding: 30px;
                                border-radius: 15px;
                                display: inline-block;
                                box-shadow: 0 0 30px rgba(0, 255, 204, 0.2);
                            }
                        </style>
                    </head>
                    <body>
                        <div class="box">
                            <h1>Uranium Web Server Aktif!</h1>
                            <p>Bu sayfa <b>Uranium dili</b> ile yazilmis bir sunucu tarafindan olusturuldu.</p>
                            <p>Saf C++ gucu ve ozel TCP Soketleri kullanildi.</p>
                            <br/>
                            <p style="font-size: 14px; color: #888;">Gelistirici: Absolute AI Architecture</p>
                        </div>
                    </body>
                    </html>
                )"
                
                // HTTP Yanitini (Response) hazirlayalim
                let response = "HTTP/1.1 200 OK\r\n"
                response = response + "Content-Type: text/html; charset=UTF-8\r\n"
                response = response + "Connection: close\r\n"
                response = response + "Server: Uranium/1.0\r\n"
                response = response + "Content-Length: " + html.length + "\r\n\r\n"
                response = response + html
                
                // Yaniti gonder ve baglantiyi kapat
                netTcpSend(client, response)
            }
            netTcpClose(client)
        }
    }
}
