# mouseum-esp32

USB HID mouse emulator with **human-like cursor movement** (quadratic Bezier,
sine ease-in-out, micro-jitter) controlled wirelessly over an **HTTP REST
API**.  This is the ESP-IDF port of the original STM32F411 firmware — same
HID report format, same motion algorithms, new control plane.

The board appears to the host computer as a standard USB mouse.  No drivers
are needed on the host.  Control happens over Wi-Fi from any device with an
HTTP client (curl, Python, browser, mobile phone).

## Supported boards

| Target          | HID transport            | Host sees                | Notes |
|-----------------|--------------------------|--------------------------|-------|
| **ESP32-S3**    | TinyUSB HID (USB OTG)    | A USB mouse              | Primary target. Use the USB OTG port for the HID link. |
| **ESP32-S2**    | TinyUSB HID (USB OTG)    | A USB mouse              | Same as S3 but single-core. |
| **ESP32-C6**    | NimBLE HID-over-GATT     | A Bluetooth LE mouse     | No native USB OTG; pair the device once and the host re-bonds on reconnect. |
| **ESP32-C3 / H2** | NimBLE HID-over-GATT   | A Bluetooth LE mouse     | Same path as C6. |

The HTTP REST control plane is identical on every target.  Wi-Fi and BLE
coexist on the same 2.4 GHz radio on the C6 — see "BLE notes" below.

Optional: any GPIO button.  Defaults to `GPIO 0` (the BOOT button), which
already has a pull-up on every dev kit.

See [docs/wiring.md](docs/wiring.md) for pinout details.

## Build

Requirements: **ESP-IDF v5.1+**.  For S3/S2 targets, the `esp_tinyusb`
component is also required.

### ESP32-S3 (USB HID)

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py add-dependency "espressif/esp_tinyusb^1.4.0"   # only if not bundled
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### ESP32-C6 (BLE HID)

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

ESP-IDF picks `sdkconfig.defaults.esp32s3` or `sdkconfig.defaults.esp32c6`
automatically based on the active target.  The common settings live in
`sdkconfig.defaults`.

## Run

1. After flashing, the device boots a Soft-AP named **`mouseum`**
   (open network by default — edit `main/board_config.h` to add a WPA2
   password).
2. Connect your laptop / phone to that SSID.
3. Browse to **<http://192.168.4.1/>** for a built-in control panel, or
   call the REST API directly.

### Pairing the BLE HID (C6 only)

In addition to the steps above, the host that should *receive the mouse
input* needs to pair with the device:

- macOS: `System Settings → Bluetooth → mouseum → Connect`
- Windows: `Settings → Bluetooth → Add device → mouseum`
- Linux:   `bluetoothctl → scan on, pair <addr>, trust <addr>, connect <addr>`

Pairing uses JustWorks (no PIN).  After the first pair the host re-bonds
automatically.  The same host can drive the REST API over Wi-Fi from a
different machine — they don't have to be the same device.

### Quick curl examples

```bash
curl http://192.168.4.1/api/v1/status
curl -X POST http://192.168.4.1/api/v1/move_human \
     -H 'Content-Type: application/json' -d '{"dx":300,"dy":100}'
curl -X POST http://192.168.4.1/api/v1/click \
     -H 'Content-Type: application/json' -d '{"button":"left"}'
curl -X POST http://192.168.4.1/api/v1/autowalk/toggle
```

### Python host driver

```bash
python host/mouseum_http.py status
python host/mouseum_http.py human 300 100
python host/mouseum_http.py demo
```

### REST API summary

| Method | Path                              | Body                              |
|--------|-----------------------------------|-----------------------------------|
| POST   | `/api/v1/move`                    | `{"dx":int,"dy":int}`             |
| POST   | `/api/v1/move_human`              | `{"dx":int,"dy":int}`             |
| POST   | `/api/v1/click`                   | `{"button":"left|right|middle"}`  |
| POST   | `/api/v1/buttons/down`            | `{"mask":int}` (1=L,2=R,4=M)      |
| POST   | `/api/v1/buttons/up`              | `{"mask":int}`                    |
| POST   | `/api/v1/buttons/release_all`     | *(empty)*                         |
| POST   | `/api/v1/wheel`                   | `{"delta":int}`                   |
| POST   | `/api/v1/autowalk/toggle`         | *(empty)*                         |
| GET    | `/api/v1/status`                  | —                                 |
| GET    | `/api/v1/help`                    | —                                 |

The GPIO button performs a one-shot human-move in alternating directions.

## Source layout

```
mouseum-esp32/
├── CMakeLists.txt                project root
├── sdkconfig.defaults            shared defaults
├── sdkconfig.defaults.esp32s3    TinyUSB HID config
├── sdkconfig.defaults.esp32c6    NimBLE HID + Wi-Fi/BT coex config
├── partitions.csv                1.5 MB factory + NVS
├── main/
│   ├── CMakeLists.txt            picks USB vs BLE impl by ${IDF_TARGET}
│   ├── main.c                    app_main + http_cmd_task supervisor
│   ├── board_config.h            pins, SSID, BLE name, queue depth
│   ├── mouse_engine.[ch]         PRNG, easing, MoveMouseHuman
│   ├── cmd_dispatcher.[ch]       transport-agnostic command surface
│   ├── hid_transport.h           neutral HID interface
│   ├── usb_hid_task.c            TinyUSB HID device (S3/S2)
│   ├── ble_hid_task.c            NimBLE HID-over-GATT (C6/C3/H2)
│   ├── wifi_manager.[ch]         Soft-AP bring-up
│   └── http_server.[ch]          esp_http_server REST routes
├── host/
│   └── mouseum_http.py           Python CLI / library
├── frontend/
│   └── index.html                rich web control panel
└── docs/
    └── wiring.md                 pinout / connection diagram
```

## Architecture in one paragraph

`app_main` starts a HID transport (TinyUSB on S3, NimBLE on C6), a Wi-Fi
Soft-AP + HTTP server, and two FreeRTOS tasks.  `hid_transport_task`
(core 0, prio 10 on dual-core; just prio 10 on the single-core C6) blocks
on a 16-deep queue and forwards each `mouse_report_t` to the host via
`tud_hid_mouse_report()` or `ble_gatts_notify_custom()`.  `http_cmd_task`
polls the GPIO button and paces the autowalk demo.  Each HTTP handler
parses JSON into a `cmd_t` and calls `cmd_execute()`, which is the single
mutating surface — it holds the buttons mutex and either enqueues an
instantaneous report or runs the blocking `move_mouse_human()` 30–60-step
Bezier.  The transport is selected at build time by `main/CMakeLists.txt`
based on `${IDF_TARGET}`; the rest of the codebase is target-agnostic.

## BLE notes (ESP32-C6)

The C6 hosts Wi-Fi and BLE on a single 2.4 GHz radio, scheduled by the
ESP-IDF coexistence layer (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`).  In
practice this is fine for human-cadence input, but you may see a brief
stall (50–100 ms) when a Wi-Fi client is mid-burst.  If you need the
lowest BLE latency, disable Wi-Fi by setting `MOUSEUM_AP_SSID` to `""` or
gating `wifi_manager_start_softap()` behind a Kconfig option.

The BLE HID descriptor is the same boot-mouse-compatible 4-byte report as
the USB build (buttons, X, Y, wheel).  All host OSes recognise it as a
standard pointing device with no extra drivers.

## Differences from the STM32 reference

| Concern          | STM32 firmware                | ESP32 firmware                       |
|------------------|-------------------------------|--------------------------------------|
| Control transport| UART 115200 ASCII             | HTTP/JSON over Wi-Fi                 |
| USB busy retry   | `HAL_Delay(1)` spin loop      | FreeRTOS queue + `tud_hid_ready`     |
| Concurrency      | bare-metal superloop          | 2 tasks, dual-core pinned            |
| PRNG seed        | `DWT->CYCCNT`                 | `esp_random() ^ esp_timer_get_time()`|
| Algorithms       | unchanged                     | unchanged                            |

## Bring-up checklist

See section 13 of the design doc for the original step-by-step list.  The
short version: confirm USB enumeration first (host sees an "ESP32 Human
interface" mouse), then confirm Wi-Fi (laptop joins `mouseum`), then test
the REST endpoints one at a time.
