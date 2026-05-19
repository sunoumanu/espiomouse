# Wiring — mouseum-esp32

Two reference dev boards are supported.  See the relevant section below.

## ESP32-S3 DevKit-C-1 (USB HID)

The S3 dev board exposes two USB-C connectors:

```
    +--------------------------------+
    |  [USB]            [UART]       |
    |   ^                ^           |
    |   |                |           |
    |  native USB OTG    USB-serial  |
    |  -> HOST PC        -> flash &  |
    |     (HID mouse)       monitor  |
    +--------------------------------+
```

Connect the **USB OTG** port to the host computer that should see the
emulated mouse.  Use the **UART** port for `idf.py flash` and
`idf.py monitor`.  You can power the board from either port; it does **not**
need to be plugged into both simultaneously after flashing.

## Pinout

| Signal      | GPIO         | Notes                                              |
|-------------|--------------|----------------------------------------------------|
| USB D-      | GPIO 19      | Fixed S3 USB OTG pin.  No software config needed.  |
| USB D+      | GPIO 20      | Fixed S3 USB OTG pin.                              |
| BOOT button | GPIO 0       | Active-low.  Trigger for the one-shot human move.  |
| 3V3 / GND   | 3V3 / GND    | USB-powered.                                       |

To use a different button GPIO, edit `BUTTON_GPIO` in
`main/board_config.h` (any input-capable pin with a pull-up).

## Wi-Fi

The ESP32-S3 has an internal 2.4 GHz radio and PCB antenna — no wiring is
required.  Default Soft-AP:

| Field    | Value                          |
|----------|--------------------------------|
| SSID     | `mouseum` (open network)       |
| Channel  | 1                              |
| Gateway  | `192.168.4.1`                  |
| HTTP     | port 80                        |

Override the SSID, channel, password, or HTTP port in
`main/board_config.h`.

## ESP32-C6 DevKit-C-1 (BLE HID)

The C6 dev board has a **single USB-C port** (USB Serial/JTAG) used for
both flashing and the runtime console.  There is no native USB OTG, so
the mouse input reaches the host over Bluetooth LE instead.

```
    +--------------------------------+
    |  [USB]                         |
    |   ^                            |
    |   |                            |
    |  USB Serial/JTAG               |
    |  -> flash & monitor only       |
    |                                |
    |  HID mouse data leaves the     |
    |  board via the internal BLE +  |
    |  Wi-Fi antenna.                |
    +--------------------------------+
```

### Pinout

| Signal      | GPIO         | Notes                                              |
|-------------|--------------|----------------------------------------------------|
| BOOT button | GPIO 9       | Active-low.  Trigger for the one-shot human move.  |
| USB         | GPIO 12/13   | USB Serial/JTAG, used for flashing only.           |
| 3V3 / GND   | 3V3 / GND    | USB-powered.                                       |

The default `BUTTON_GPIO` in `main/board_config.h` is `GPIO 0` (matches
the S3 BOOT button).  On the C6 DevKit-C-1, the BOOT button is wired to
`GPIO 9` — set `BUTTON_GPIO=GPIO_NUM_9` when building for the C6, or wire
a button to GPIO 0 yourself.

### BLE pairing

After flashing, the device advertises as `mouseum` with the HID Mouse
appearance.  Pair it on the host that should receive the mouse input
(macOS Bluetooth pane, Windows Bluetooth settings, `bluetoothctl` on
Linux).  Pairing uses JustWorks — no PIN.  After the first pair the host
re-bonds automatically on reconnect.

Wi-Fi continues to host the HTTP REST control plane in parallel with BLE,
sharing the same 2.4 GHz radio via the ESP-IDF coexistence layer.

## Troubleshooting

- **(S3) Host sees no USB mouse.**  Make sure the cable is plugged into
  the **USB OTG** port, not the **UART** port.
- **(C6) Host sees no BLE mouse.**  Confirm the device is advertising
  (search for `mouseum` from the host's Bluetooth scanner).  If it
  doesn't appear, check the serial monitor for NimBLE init errors.  If
  pairing fails, remove any stale `mouseum` entry from the host's
  Bluetooth settings and re-pair.
- **No `mouseum` SSID visible.**  Check the serial monitor for Wi-Fi init
  errors.  Some bench supplies brown out during Wi-Fi TX bursts — use the
  USB OTG port for power.
- **`/api/v1/status` shows `usb_ready: false`.**  The host has not bound
  its HID driver yet.  Wait a second or two after plugging in.
- **`move_human` is very fast.**  The host may be running a high-DPI mouse
  acceleration curve.  The firmware emits the same 30–60 sub-steps as the
  STM32 reference; the on-screen distance is host-dependent.
