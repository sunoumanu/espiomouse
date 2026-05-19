/*
 * usb_hid_task.c — TinyUSB device init + HID report pump.
 *
 * Two layers:
 *   1. `usb_hid_init()` configures and starts the TinyUSB driver
 *      (esp_tinyusb component).  We supply a mouse HID descriptor and the
 *      standard configuration descriptor.
 *   2. `usb_hid_task()` drains hid_queue and forwards each item via
 *      tud_hid_report().  The TinyUSB driver runs `tud_task()` in its own
 *      internal task on ESP-IDF, so we only need to wait on the queue.
 *
 * If a report arrives while the host is not ready, we drop it.  TinyUSB
 * already buffers one report internally and the host polls at 1 kHz, so
 * this is essentially never the steady-state.
 */
/* USB HID (TinyUSB) implementation of hid_transport.h.
 *
 * Built only when ${IDF_TARGET} has a native USB OTG controller — selected
 * by main/CMakeLists.txt.  See ble_hid_task.c for the BLE alternative used
 * on the ESP32-C6.
 */
#include "hid_transport.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "mouse_engine.h"
#include "board_config.h"

static const char *TAG = "usb_hid";

/* ---- HID Report Descriptor ----------------------------------------------- */
/* Boot-protocol-compatible mouse with 8-bit X/Y/wheel.  Matches the 4-byte
 * layout used by the STM32 firmware. */
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

/* TinyUSB calls this to obtain the report descriptor at enumeration time. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

/* Host SET_REPORT — ignored; we never accept output reports. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)bufsize;
}

/* Host GET_REPORT — always reply with zero bytes; not used. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}

/* ---- String descriptors -------------------------------------------------- */
static const char *s_string_desc[5] = {
    (char[]){ 0x09, 0x04 },          /* 0: language id (English-US) */
    "Espressif",                     /* 1: Manufacturer */
    "ESP32 Human interface",         /* 2: Product */
    "000000",                        /* 3: Serial — overridden at init from eFuse MAC */
    "ESP32 mouseum HID",             /* 4: Interface */
};

/* ---- Init ---------------------------------------------------------------- */
void hid_transport_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB HID");

    /* Optional: derive a per-board serial from the eFuse MAC so multiple
     * dev boards on the same host get distinct USB serial numbers.  We keep
     * the fallback in place so this still works if eFuse access fails. */

    tinyusb_config_t cfg = {
        .device_descriptor       = NULL,            /* use default built from sdkconfig */
        .string_descriptor       = s_string_desc,
        .string_descriptor_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy            = false,
        .configuration_descriptor = NULL,           /* TinyUSB will auto-build */
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "TinyUSB installed");
}

bool hid_transport_is_ready(void)
{
    return tud_mounted() && tud_hid_ready();
}

/* ---- Task ---------------------------------------------------------------- */
void hid_transport_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "hid_transport_task (USB) running on core %d", xPortGetCoreID());

    /* Wait for the host to enumerate before we start trying to send.
     * TinyUSB on ESP-IDF runs its own internal task to call tud_task(),
     * so we only need to gate on tud_mounted() here. */
    while (!tud_mounted()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* Give the host an extra moment to bind its HID driver. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "USB host mounted — pumping HID reports");

    hid_queue_item_t item;
    for (;;) {
        /* Block until a report appears.  10 s tick is just for periodic
         * housekeeping; the loop is event-driven in steady state. */
        if (xQueueReceive(hid_queue, &item, pdMS_TO_TICKS(10000)) != pdPASS) {
            continue;
        }

        /* If USB went away, drop accumulated reports rather than bursting
         * them when it comes back.  Drain everything that's queued. */
        if (!tud_mounted()) {
            ESP_LOGW(TAG, "USB unmounted — dropping queued reports");
            do { /* drain */ } while (xQueueReceive(hid_queue, &item, 0) == pdPASS);
            continue;
        }

        /* Wait briefly for the previous report to clear.  TinyUSB returns
         * false if the endpoint is still busy — we give it up to 5 ms,
         * then drop. */
        TickType_t wait_until = xTaskGetTickCount() + pdMS_TO_TICKS(5);
        while (!tud_hid_ready() && xTaskGetTickCount() < wait_until) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (tud_hid_ready()) {
            tud_hid_mouse_report(item.report_id,
                                 item.r.buttons,
                                 item.r.x,
                                 item.r.y,
                                 item.r.wheel,
                                 0 /* horizontal scroll, unused */);
        } else {
            ESP_LOGW(TAG, "tud_hid_ready false — dropped report");
        }
    }
}
