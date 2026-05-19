/*
 * mouse_engine.h — algorithms ported byte-for-byte from the STM32 reference.
 *
 * Owns:
 *   - The HID mouse report structure (matches the STM32 4-byte layout).
 *   - The xorshift32 PRNG (preserves the original mouse "feel").
 *   - Quadratic-Bezier human-like motion with sine ease-in-out and micro-jitter.
 *
 * Threading:
 *   - mouse_engine_init() must run once before any other call.
 *   - move_mouse_human() blocks on vTaskDelay between steps, so call it only
 *     from the cmd_dispatcher / http_cmd_task context (never an ISR).
 *   - get_random() is not thread-safe; today only the command task calls it.
 *     If multiple producers ever need it, wrap the state in a mutex.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 4-byte HID report — identical layout to the STM32 build. */
typedef struct {
    uint8_t buttons;   /* bit0 = left, bit1 = right, bit2 = middle */
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} mouse_report_t;

typedef struct {
    uint8_t        report_id;   /* unused for boot mouse; kept for TinyUSB */
    mouse_report_t r;
} hid_queue_item_t;

/* The single global queue read by usb_hid_task. */
extern QueueHandle_t hid_queue;

/* Seed the PRNG from esp_random() XOR esp_timer_get_time(). */
void     mouse_engine_init(void);

/* xorshift32-backed integer in [min, max].  Same algorithm as the STM32 firmware. */
uint32_t get_random(uint32_t min, uint32_t max);

/* 0.5 * (1 - cos(t * PI)) — sine ease-in-out. */
float    ease_in_out(float t);

/*
 * Enqueue a single HID report.  Blocks up to `timeout_ticks` if the queue is
 * full.  Returns true on success, false if dropped.  Used by both
 * move_mouse_human() and cmd_dispatcher.
 */
bool     mouse_engine_send(const mouse_report_t *rep, TickType_t timeout_ticks);

/*
 * Run a human-like Bezier move of (target_x, target_y) deltas.  Splits the
 * motion into 30–60 sub-steps spaced 8–14 ms apart.  `current_buttons` is
 * the persistent button mask owned by cmd_dispatcher (so e.g. a drag stays
 * held down for the whole move).
 */
void     move_mouse_human(int16_t target_x, int16_t target_y, uint8_t current_buttons);
