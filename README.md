# QuizFabric for the Waveshare ESP32-S3-Touch-LCD-7B

A touchscreen **QuizFabric client** for the 7-inch Waveshare board. It plays
quizzes from any QuizFabric server ([QuizFabric-Web](https://github.com/QuizFabric/QuizFabric-Web))
over plain HTTP, with the same look and scoring as the 3DS client.

Inspired by the 3DS port.

## Features

- **Play Solo** — browse the quiz list with banner thumbnails, then play:
  - Multiple choice (tap an answer)
  - Type-in (on-screen keyboard)
  - Slider (drag the track)
  - Puzzle / ordering (tap the arrows)
  - Scoring matches the 3DS client: `max(0, 1000 - seconds × 50)`
- **Question images** — decoded from the server's 3DS `.565` format and
  drawn on screen. **Animated GIFs are skipped** (the ESP32 is not fast
  enough for them) and shown as "GIF skipped".
- **Steal from Kahoot** — search Kahoot, preview covers, tap to steal a quiz
  straight onto the server.
- **Server settings** — the server address is editable on-device (defaults
  to the Pi on the LAN, `192.168.50.65:5710`).

## Hardware

- [Waveshare ESP32-S3-Touch-LCD-7B](https://www.waveshare.com/esp32-s3-touch-lcd-7b.htm)
  (ESP32-S3R8, 1024×600 RGB LCD + GT911 capacitive touch)
- Board settings: `ESP32S3 Dev Module`, **16 MB Flash**, **OPI PSRAM**

## Flashing

**Easiest way — the web flasher:** open
<https://quizfabric.github.io/QuizFabric-WV-TOUCH-LCD-7B/> in Chrome/Edge,
plug the board's **UART** USB-C port into your PC, click *Connect*, pick the
serial port, then *Flash*. (Requires a Web Serial browser — Chrome or Edge.)

Or flash `quizfabric-client.bin` with any ESP32-S3 tool at address `0x0`
(it's a merged image: bootloader + partition table + app).

## Building from source

1. Install the Arduino ESP32 core (3.x) and the
   [ArduinoJson](https://arduinojson.org/) library
2. Open the `quizfabric_client/` folder in the Arduino IDE
3. Select `ESP32S3 Dev Module`, 16 MB Flash, **OPI PSRAM**, and compile

The sketch folder contains Waveshare's official 7B RGB LCD driver
(`rgb_lcd_port`, `i2c`, `io_extension`, `gt911`) from their
[ESP32-S3-Touch-LCD-7B](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B)
repo (Apache-2.0) and the public-domain [font8x8](https://github.com/dhepper/font8x8) font.

## WiFi

The client connects to WiFi `VM6842809` (edit `WIFI_SSID` / `WIFI_PASS` at
the top of `quizfabric_client.ino`). The QuizFabric server address is
configurable on the touchscreen (Server settings).

## License

Code: MIT. Third-party drivers retain their own licenses (see headers).
