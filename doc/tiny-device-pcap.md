# Streaming captures from embedded sniffers

The `ENABLE_TINY_DEVICES` configuration keeps the console capture tools such as
`tshark` and `dumpcap` available so that resource-constrained front-ends can
forward traffic to a more capable host for analysis.  One practical workflow is
piping frames observed by an ESP32 (or a similar SoC) in promiscuous mode to a
host running Wireshark tools.

## Serial or pipe transport

1. Let the microcontroller collect frames and emit them as a byte stream.  The
   easiest format is classic pcap so the host can consume the stream directly.
2. Connect the board over UART, USB CDC, or another link and expose it on the
   host as a character device or named pipe.
3. Start `tshark` (or `dumpcap`) against standard input or a named pipe.  For
   example, on a Unix-like host:

   ```sh
   stty -F /dev/ttyUSB0 460800 raw -echo -ixon
   cat /dev/ttyUSB0 | tshark -i -
   ```

   On Windows PowerShell you can rely on a named pipe:

   ```powershell
   $pipe = New-Object System.IO.Pipes.NamedPipeServerStream('esp32-capture')
   $pipe.WaitForConnection()
   $stream = New-Object System.IO.StreamReader($pipe)
   tshark -i \\?\pipe\esp32-capture -f 'wlan'
   ```

   Any transport that preserves byte order will work, including TCP sockets or
   WebUSB bridges.

## Producing pcap records on the device

The ESP-IDF promiscuous API provides raw 802.11 frames.  To make them usable by
Wireshark you must prepend a pcap global header once and emit a per-packet
record for every frame.  The following header-only helper, adapted from Stefan
Kremser's open-source ESP32 projects, shows one way to do this:

```cpp
/*
  ===========================================
       Copyright (c) 2017 Stefan Kremser
              github.com/spacehuhn
  ===========================================
*/

#ifndef PCAP_h
#define PCAP_h

#include <Arduino.h>
#include "SPI.h"
#if defined(ESP32)
        #include "FS.h"
        #include "SD.h"
#else
        #include <SPI.h>
        #include <SdFat.h>
#endif

class PCAP
{
  public:
    PCAP();

    void startSerial();
#if defined(ESP32)
    bool openFile(fs::FS &fs);
    bool removeFile(fs::FS &fs);
#else
    bool openFile(SdFat &SD);
    bool removeFile(SdFat &SD);
#endif

    void flushFile();
    void closeFile();

    void newPacketSerial(uint32_t ts_sec, uint32_t ts_usec, uint32_t len, uint8_t* buf);
    void newPacketSD(uint32_t ts_sec, uint32_t ts_usec, uint32_t len, uint8_t* buf);

    String filename = "capture.cap";

    uint32_t magic_number = 0xa1b2c3d4;
    uint16_t version_major = 2;
    uint16_t version_minor = 4;
    uint32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen = 2500;
    uint32_t network = 105;

  private:
    File file;

    void escape32(uint32_t n, uint8_t* buf);
    void escape16(uint16_t n, uint8_t* buf);

    void filewrite_16(uint16_t n);
    void filewrite_32(uint32_t n);

    void serialwrite_16(uint16_t n);
    void serialwrite_32(uint32_t n);
};

#endif
```

Pairing this helper with a promiscuous callback similar to the ESP-IDF example
below makes it easy to serialize frames:

```cpp
static PCAP pcap;

void setup() {
    Serial.begin(460800);
    pcap.startSerial();
    // ... configure Wi-Fi in promiscuous mode ...
    esp_wifi_set_promiscuous_rx_cb(sniffer);
}

void sniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
    auto *pkt = static_cast<wifi_promiscuous_pkt_t *>(buf);
    const wifi_pkt_rx_ctrl_t ctrl = pkt->rx_ctrl;

    if (ctrl.sig_len == 0 || ctrl.sig_len > pcap.snaplen) {
        return;
    }

    const uint32_t now_us = esp_timer_get_time();
    pcap.newPacketSerial(now_us / 1000000, now_us % 1000000, ctrl.sig_len, pkt->payload);
}
```

On the host, `tshark -i -` can decode the stream live.  Because the tiny-device
preset keeps libpcap enabled, you still have the option to attach directly to
interfaces provided by the target toolchain when that is feasible.
