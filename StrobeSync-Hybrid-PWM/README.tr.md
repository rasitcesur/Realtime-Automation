# StrobeSync — Hibrit PWM ve Gerçek Zamanlı Senkronizasyon

Bu depo, **STM32F103** için CMSIS tabanlı C firmware, **Arduino Uno/Nano
ATmega328P** için AVR firmware ve her iki uygulamayla ortak UART protokolünü
kullanan **Python ile C#** istemcilerini içerir.

**Durum:** GitHub'a yüklenebilir kaynak paketi hazırlanmıştır. STM32/Arduino
firmware derlemesi, fiziksel devre testleri, gerçek zamanlı gecikme ölçümleri
ve cihazlar arası uçtan uca test yapılmamıştır.

## Klasörler

- `firmware/stm32/Core/Src/main.c`: STM32F103 CMSIS register tabanlı kaynak.
- `firmware/arduino/StrobeSyncAVR/StrobeSyncAVR.ino`: Arduino AVR kaynak.
- `clients/python/strobesync/client.py`: Python UART istemcisi.
- `clients/csharp/StrobeSyncController/StrobeSyncClient.cs`: C# istemcisi.
- `docs/PINOUT.md`: İki cihaz için ayrı pin eşlemeleri.
- `docs/PROTOCOL.md`: Komut ve telemetri çerçeveleri.
- `docs/VALIDATION.md`: Test kapsamı, teknik sınırlar ve riskler.

## Çalıştırma

Arduino IDE ile `.ino` dosyasını Uno veya klasik Nano üzerine yükleyin.
STM32 uygulaması için CMSIS, başlangıç dosyaları, linker script ve doğru
cihaz tanımı içeren ayrı bir proje oluşturulmalıdır; sadece `main.c`
tek başına derlenip flashlanabilir proje değildir.

Python istemcisini yükleme:

```bash
python -m pip install -e ./clients/python
```

Python kullanım örneği:

```python
from strobesync import StrobeSyncClient

client = StrobeSyncClient("COM3", on_sensor_data=lambda i, v, m: print(i, v, m))
try:
    client.connect()  # STM32 otomatik reset kullanmıyorsa reset_delay_sec=0
    client.set_pwm_channel(0, 1, 75)
    client.configure_sensor(0, StrobeSyncClient.STREAM_BINARY_CONTINUOUS)
    client.trigger_realtime_sync(500)
    input("Çıkış için Enter... ")
finally:
    client.close()
```

C# istemcisi:

```bash
dotnet build clients/csharp/StrobeSyncController/StrobeSyncController.csproj
dotnet run --project examples/csharp/StrobeSyncConsole/StrobeSyncConsole.csproj -- COM3
```

## Teknik çerçeve

Sistem 6 PWM, 6 senkron darbe, 2 uyarı çıkışı ve 4 sensör kanalını yönetir.
UART hızı **115200 8N1**'dir. ASCII telemetri `SENS:0:1023` gibi
satırlardan; binary telemetri ise `0xAA` ile başlayan 5 bayttan oluşur.
Binary çerçeve kontrolü CRC değil **XOR** kontrol toplamıdır. Anlık okuma
komutu yeni ADC örneklemesi başlatmak yerine son ölçülen değeri gönderir.

Arduino ve STM32'nin fiziksel pinleri farklıdır. 3,3 V STM32 UART RX
pinine doğrudan 5 V bağlamayın. LED/strobe sürücüleri MCU GPIO pinlerinden
doğrudan beslenmemelidir. Donanım kullanımından önce pin eşlemelerini ve
osiloskop ile zamanlamayı doğrulayın.

## Lisans ve yayınlama

MIT lisansı **öneri olarak** eklenmiştir. `LICENSE` içindeki hak sahibi
bilgisini ve kaynak kod üzerindeki haklarınızı GitHub'da yayımlamadan önce
kontrol edin. Bağımlılıklar kendi lisanslarını korur.

GitHub yükleme komutları için ana [README](README.md) dosyasına ve aşağıdaki
kısa örneğe bakabilirsiniz:

```bash
git init
git add .
git commit -m "Initial release: STM32, Arduino, Python and C#"
git branch -M main
git remote add origin https://github.com/<kullanici>/<repo>.git
git push -u origin main
```
