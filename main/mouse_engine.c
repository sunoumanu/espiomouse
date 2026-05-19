/*
 * mouse_engine.c — see header for contract.
 *
 * The MoveMouseHuman implementation is intentionally a near-verbatim port of
 * the STM32 reference so the on-screen motion feel is identical.  Only USB
 * delivery is changed: instead of calling USBD_HID_SendReport with a busy
 * retry loop, we push a report to `hid_queue` and let usb_hid_task drain it.
 */
#include "mouse_engine.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "board_config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const char *TAG = "mouse_engine";

QueueHandle_t hid_queue = NULL;

/* xorshift32 PRNG.  Identical to the STM32 firmware. */
static uint32_t s_rng_state = 0xDEADBEEFu;

static inline uint32_t xorshift32(void)
{
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

void mouse_engine_init(void)
{
    /* Seed from the hardware RNG XOR microsecond clock — stronger entropy
     * than the STM32's DWT->CYCCNT, but the algorithm itself is unchanged. */
    uint32_t seed = esp_random() ^ (uint32_t)esp_timer_get_time();
    if (seed == 0) {
        seed = 0xA5A5A5A5u; /* xorshift collapses on a zero seed. */
    }
    s_rng_state = seed;

    if (hid_queue == NULL) {
        hid_queue = xQueueCreate(HID_QUEUE_DEPTH, sizeof(hid_queue_item_t));
        if (hid_queue == NULL) {
            ESP_LOGE(TAG, "Failed to allocate hid_queue");
        }
    }
}

uint32_t get_random(uint32_t min, uint32_t max)
{
    if (max <= min) {
        return min;
    }
    uint32_t span = max - min + 1u;
    return min + (xorshift32() % span);
}

float ease_in_out(float t)
{
    return 0.5f * (1.0f - cosf(t * (float)M_PI));
}

bool mouse_engine_send(const mouse_report_t *rep, TickType_t timeout_ticks)
{
    if (hid_queue == NULL || rep == NULL) {
        return false;
    }
    hid_queue_item_t item = { .report_id = 0, .r = *rep };
    return xQueueSend(hid_queue, &item, timeout_ticks) == pdPASS;
}

/*
 * Saturating cast of a float pixel delta to int8.  USB boot mouse reports
 * are limited to int8 per axis; large Bezier sub-steps are extremely rare
 * because steps = 30–60 and we move 8–14 ms apart, but a host pause could
 * still produce one.
 */
static inline int8_t sat_i8(float v)
{
    if (v >  127.0f) return  127;
    if (v < -128.0f) return -128;
    return (int8_t)v;
}

void move_mouse_human(int16_t target_x, int16_t target_y, uint8_t current_buttons)
{
    const int16_t start_x = 0;
    const int16_t start_y = 0;

    /* Random control point near the midpoint — produces the Bezier "arc". */
    int16_t control_x = (int16_t)((target_x / 2) + (int32_t)get_random(0, 40) - 20);
    int16_t control_y = (int16_t)((target_y / 2) + (int32_t)get_random(0, 40) - 20);

    float steps   = (float)get_random(30, 60);
    float prev_px = 0.0f, prev_py = 0.0f;

    for (float i = 1.0f; i <= steps; i += 1.0f) {
        float t       = i / steps;
        float eased_t = ease_in_out(t);
        float omt     = 1.0f - eased_t;

        float cur_px = omt * omt * (float)start_x
                     + 2.0f * omt * eased_t * (float)control_x
                     + eased_t * eased_t * (float)target_x;
        float cur_py = omt * omt * (float)start_y
                     + 2.0f * omt * eased_t * (float)control_y
                     + eased_t * eased_t * (float)target_y;

        float dx = cur_px - prev_px;
        float dy = cur_py - prev_py;

        mouse_report_t rep = {
            .buttons = current_buttons,
            .x       = sat_i8(dx),
            .y       = sat_i8(dy),
            .wheel   = 0,
        };

        /* Micro-jitter ~20% of the time, +/-1 pixel.  Identical to STM32. */
        if (get_random(0, 10) > 8) {
            int8_t jx = (int8_t)((int32_t)get_random(0, 2) - 1);
            int8_t jy = (int8_t)((int32_t)get_random(0, 2) - 1);
            rep.x = sat_i8((float)rep.x + jx);
            rep.y = sat_i8((float)rep.y + jy);
        }

        /* See design 7.3: drop on full queue rather than block the engine. */
        if (!mouse_engine_send(&rep, pdMS_TO_TICKS(5))) {
            ESP_LOGW(TAG, "hid_queue full — dropped step");
        }

        prev_px = cur_px;
        prev_py = cur_py;

        vTaskDelay(pdMS_TO_TICKS(get_random(8, 14)));
    }
}
