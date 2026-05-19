/*
 * main.c — app_main + the http_cmd_task supervisory loop.
 *
 * Boot order:
 *   1. NVS init (Wi-Fi driver requires it).
 *   2. GPIO button.
 *   3. mouse_engine_init (seeds PRNG, allocates hid_queue).
 *   4. cmd_dispatcher_init (allocates mutex).
 *   5. Wi-Fi Soft-AP up.
 *   6. HTTP server up.
 *   7. TinyUSB up.
 *   8. usb_hid_task pinned to core 0 (high priority).
 *   9. http_cmd_task pinned to core 1 (normal priority).
 *
 * Note that USB enumeration takes ~1 s — usb_hid_task gates on tud_mounted()
 * before sending anything, so it is safe to start everything in this order.
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"
#include "mouse_engine.h"
#include "cmd_dispatcher.h"
#include "hid_transport.h"
#include "wifi_manager.h"
#include "http_server.h"

static const char *TAG = "main";

/* ---- Button GPIO --------------------------------------------------------- */
static void button_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/* ---- http_cmd_task ------------------------------------------------------- */
/*
 * This task drives the autowalk pacer and polls the GPIO button.  The HTTP
 * server itself runs in its own LWIP worker threads — it does not live in
 * this task.  We could fold this into a dedicated supervisor task, but
 * keeping them together avoids extra context switches and stack overhead.
 */
static void http_cmd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "http_cmd_task running on core %d", xPortGetCoreID());

    for (;;) {
        /* Manual button trigger.  Active-low (BOOT button). */
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            ESP_LOGI(TAG, "button pressed — human-move trigger");
            cmd_button_press_action();
            /* Crude debounce + cooldown matching the STM32 firmware. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Autowalk pacer.  Internal state in cmd_dispatcher. */
        cmd_autowalk_tick();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- Entry --------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "mouseum-esp32 booting");

    /* 1. NVS — required by Wi-Fi. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* 2. GPIO. */
    button_gpio_init();

    /* 3-4. Engine + dispatcher. */
    mouse_engine_init();
    cmd_dispatcher_init();

    /* 5-6. Wi-Fi + HTTP.  wifi_manager_start() picks STA or Soft-AP based
     * on MOUSEUM_STA_SSID in board_config.h.  In STA mode it blocks until
     * we have an IP (or times out) before HTTP starts. */
    wifi_manager_start();
    start_http_server();

    /* 7. HID device stack (TinyUSB on S3, NimBLE HID on C6). */
    hid_transport_init();

    /* 8. hid transport task on the high-priority core. */
    BaseType_t r;
    r = xTaskCreatePinnedToCore(hid_transport_task, "hid_xport",
                                USB_HID_TASK_STACK,  NULL,
                                USB_HID_TASK_PRIO,   NULL, USB_HID_TASK_CORE);
    configASSERT(r == pdPASS);

    /* 9. http_cmd_task on core 1 (button polling + autowalk pacer). */
    r = xTaskCreatePinnedToCore(http_cmd_task, "http_cmd",
                                HTTP_CMD_TASK_STACK, NULL,
                                HTTP_CMD_TASK_PRIO,  NULL, HTTP_CMD_TASK_CORE);
    configASSERT(r == pdPASS);

    ESP_LOGI(TAG, "boot complete");
}
