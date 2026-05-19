/*
 * board_config.h — pin assignments and tunable constants.
 *
 * Defaults match the ESP32-S3 DevKit-C-1 (section 8 of the design doc).
 * Override per-board by editing this file or supplying -D flags.
 */
#pragma once

#include "driver/gpio.h"

/* ----- GPIO ----- */
/* BOOT button on most S3 dev kits.  Active-low, internal pull-up. */
#ifndef BUTTON_GPIO
#define BUTTON_GPIO          GPIO_NUM_0
#endif

/* ----- USB ----- */
/* Native USB OTG D-/D+ are fixed on the S3 at GPIO 19 / 20; nothing for us
 * to configure in software, but documented here for the wiring diagram. */

/* ----- BLE (C6 / NimBLE HID) ----- */
#ifndef MOUSEUM_BLE_NAME
#define MOUSEUM_BLE_NAME     "mouseum"
#endif

/* ----- Wi-Fi (Soft-AP defaults) ----- */
#ifndef MOUSEUM_AP_SSID
#define MOUSEUM_AP_SSID      "mouseum"
#endif

#ifndef MOUSEUM_AP_PASS
/* Empty string => open network.  Set a WPA2 password for a closed network. */
#define MOUSEUM_AP_PASS      ""
#endif

#ifndef MOUSEUM_AP_CHANNEL
#define MOUSEUM_AP_CHANNEL   1
#endif

#ifndef MOUSEUM_AP_MAX_STA
#define MOUSEUM_AP_MAX_STA   4
#endif

/* ----- HTTP server ----- */
#ifndef MOUSEUM_HTTP_PORT
#define MOUSEUM_HTTP_PORT    80
#endif

/* ----- HID queue ----- */
#ifndef HID_QUEUE_DEPTH
#define HID_QUEUE_DEPTH      16
#endif

/* ----- Task tuning ----- */
/*
 * On single-core targets (ESP32-C6, ESP32-C3, ESP32-H2, ESP32-S2) only
 * core 0 exists, so xTaskCreatePinnedToCore() asserts if we pass core 1.
 * Detect that here and force both tasks onto core 0; on dual-core targets
 * (S3) we keep the original split.
 */
#include "sdkconfig.h"
#ifdef CONFIG_FREERTOS_UNICORE
#  define MOUSEUM_DEFAULT_HIGH_CORE 0
#  define MOUSEUM_DEFAULT_LOW_CORE  0
#else
#  define MOUSEUM_DEFAULT_HIGH_CORE 0
#  define MOUSEUM_DEFAULT_LOW_CORE  1
#endif

#ifndef USB_HID_TASK_STACK
#define USB_HID_TASK_STACK   4096
#endif
#ifndef USB_HID_TASK_PRIO
#define USB_HID_TASK_PRIO    10
#endif
#ifndef USB_HID_TASK_CORE
#define USB_HID_TASK_CORE    MOUSEUM_DEFAULT_HIGH_CORE
#endif

#ifndef HTTP_CMD_TASK_STACK
#define HTTP_CMD_TASK_STACK  8192
#endif
#ifndef HTTP_CMD_TASK_PRIO
#define HTTP_CMD_TASK_PRIO   5
#endif
#ifndef HTTP_CMD_TASK_CORE
#define HTTP_CMD_TASK_CORE   MOUSEUM_DEFAULT_LOW_CORE
#endif
