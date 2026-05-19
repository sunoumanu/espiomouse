/*
 * hid_transport.h — chip-neutral HID device interface.
 *
 * Two implementations live behind this header, selected at build time by
 * main/CMakeLists.txt based on ${IDF_TARGET}:
 *
 *   - usb_hid_task.c — TinyUSB HID device (ESP32-S3, ESP32-S2).
 *   - ble_hid_task.c — NimBLE HID-over-GATT (ESP32-C6, and any BLE-capable
 *                      chip without USB OTG).
 *
 * Both expose the same three entry points and read the same `hid_queue`
 * defined in mouse_engine.c.  The cmd_dispatcher and HTTP layer don't know
 * or care which transport is active.
 */
#pragma once

#include <stdbool.h>

/* Bring the HID transport up (TinyUSB install / NimBLE advertising start). */
void hid_transport_init(void);

/* FreeRTOS task that drains hid_queue and forwards reports to the host. */
void hid_transport_task(void *arg);

/* True once the host has bound/paired and the interface is ready to send. */
bool hid_transport_is_ready(void);

/* ----------------------------------------------------------------------------
 * Back-compat aliases.  The original codebase used `usb_hid_*` names; new
 * code should call the hid_transport_* names above.  The aliases are kept
 * as inline wrappers so older translation units compile unchanged.
 */
static inline void usb_hid_init(void)      { hid_transport_init(); }
static inline void usb_hid_task(void *arg) { hid_transport_task(arg); }
static inline bool usb_hid_is_ready(void)  { return hid_transport_is_ready(); }
