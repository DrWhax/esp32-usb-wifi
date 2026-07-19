# esp32-usb-eth

ESP32-S3 port of [pico-usb-wifi](https://gitlab.com/baiyibai/pico-usb-wifi): a driverless USB Wi-Fi
adapter (USB CDC-NCM device bridging to a Wi-Fi station).

Seeded from Espressif's `tusb_ncm` example (ESP-IDF v5.5,
`examples/peripherals/usb/device/tusb_ncm`, Unlicense/CC0.

* station MAC adoption + raw L2 forwarding (`esp_wifi_internal_*`, no `esp_netif`)
* reflection filter (drops the host's own frames echoed back by the AP)
* host IPv4/IPv6 snooping (the device holds no IP; the console reports the host's)
* **management console** on CDC-ACM (composite NCM + ACM): up to 8 credential
  _profiles_ in NVS (`set ssid`/`set pass` edit the active one, `list`/`use`/`del`
  manage the set), `scan` + `join <n>` to discover and stage networks, and
  `set debug on` for a 2 s stats stream (`dbg:`-prefixed) with association
  events and failure reasons (`badauth`/`nonet`). Changes apply immediately;
  `save` persists. Compile-time creds are only a never-provisioned fallback
* `tools/provision.py <ssid> [password]` — scripted provisioning over the console

Not yet ported (see the plan): watchdog/crash telemetry, LED states.

## Provisioning

```sh
. ~/esp/esp-idf/export.sh          # or: pip install pyserial
tools/provision.py "MyNetwork" "hunter2"
```

Or interactively: `picocom /dev/cu.usbmodem1234561` (any baud), then `help`.

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
