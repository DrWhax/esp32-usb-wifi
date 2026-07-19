# esp32-usb-eth

ESP32-S3 port of [pico-usb-wifi](../pico-usb-wifi): a driverless USB Wi-Fi
adapter (USB CDC-NCM device bridging to a Wi-Fi station).

Current state: **Phase 0/2 seed** — Espressif's `tusb_ncm` example from
ESP-IDF v5.5 (`examples/peripherals/usb/device/tusb_ncm`, Unlicense/CC0),
project-renamed. It already does the core data path from the port plan
(`../pico-usb-wifi/esp32_plan.md`): station MAC adoption, raw L2 forwarding
via `esp_wifi_internal_tx`/`_reg_rxcb`, no `esp_netif`, TinyUSB NCM via
`esp_tinyusb`. Upstream's README is kept as `README.upstream.md`.

Not yet ported (see the plan): reflection filter, IP snooping, management
console + profiles in NVS, scan/join, debug stats, watchdog/crash telemetry.

## Build

```sh
. ~/esp/esp-idf/export.sh
cp wifi_creds.defaults.example wifi_creds.defaults   # edit SSID/password
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;wifi_creds.defaults" \
  idf.py set-target esp32s3 build
```

(Changing credentials later: edit `wifi_creds.defaults`, delete `sdkconfig`,
re-run the build command — or just use `idf.py menuconfig` → Example
Configuration.)

## Flash and monitor

```sh
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Note: the firmware takes over the S3's only USB port with the OTG peripheral
(NCM device), so after first flash the serial-JTAG console disappears and the
board re-enumerates as a USB network interface. To reflash, hold BOOT while
plugging in (download mode), then run the flash command again.

## What you should see on the host

The Mac/Linux host gets a new network interface (CDC-NCM, in-box driver) whose
MAC is the S3's Wi-Fi station MAC; once the station associates, DHCP on that
interface yields an address from the AP's own subnet.
