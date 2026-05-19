/*
 * cmd_dispatcher.c — all mutating state goes through here.
 *
 * State invariants:
 *   - `s_current_buttons` is the live HID button mask.  Every report we send
 *     carries it, so a button held with /buttons/down stays held during any
 *     subsequent move (drag behaviour).
 *   - `s_autowalk_enabled` is toggled by the HTTP endpoint and consumed by
 *     cmd_autowalk_tick() inside http_cmd_task.
 *   - Both are protected by a single mutex.  Long blocking operations
 *     (CMD_MOVE_HUMAN, the click sleep) happen *outside* the mutex so they
 *     don't stall the status endpoint or autowalk reader.
 */
#include "cmd_dispatcher.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "mouse_engine.h"
#include "hid_transport.h"
#include "board_config.h"

static const char *TAG = "cmd";

static SemaphoreHandle_t s_state_mtx = NULL;
static uint8_t           s_current_buttons   = 0;
static bool              s_autowalk_enabled  = false;
static int8_t            s_autowalk_direction = 1;
static TickType_t        s_autowalk_last_tick = 0;
static uint32_t          s_autowalk_next_delay_ms = 1000;

#define BTN_LEFT   0x01
#define BTN_RIGHT  0x02
#define BTN_MIDDLE 0x04
#define BTN_VALID_MASK (BTN_LEFT | BTN_RIGHT | BTN_MIDDLE)

void cmd_dispatcher_init(void)
{
    if (s_state_mtx == NULL) {
        s_state_mtx = xSemaphoreCreateMutex();
    }
    s_autowalk_last_tick     = xTaskGetTickCount();
    s_autowalk_next_delay_ms = get_random(700, 1500);
}

static inline void state_lock(void)   { xSemaphoreTake(s_state_mtx, portMAX_DELAY); }
static inline void state_unlock(void) { xSemaphoreGive(s_state_mtx); }

uint8_t cmd_current_buttons(void)
{
    state_lock();
    uint8_t b = s_current_buttons;
    state_unlock();
    return b;
}

bool cmd_autowalk_enabled(void)
{
    state_lock();
    bool e = s_autowalk_enabled;
    state_unlock();
    return e;
}

void cmd_get_status(cmd_status_t *out)
{
    if (!out) return;
    state_lock();
    out->buttons   = s_current_buttons;
    out->autowalk  = s_autowalk_enabled;
    state_unlock();
    out->usb_ready = hid_transport_is_ready();
}

/* Build + send a single instantaneous report carrying the current button
 * mask.  Used for relative moves, button changes, and wheel scrolls. */
static void send_now(int8_t x, int8_t y, int8_t wheel, uint8_t buttons)
{
    mouse_report_t rep = { .buttons = buttons, .x = x, .y = y, .wheel = wheel };
    /* Block up to 50 ms; if the queue is full that long, something is wrong. */
    if (!mouse_engine_send(&rep, pdMS_TO_TICKS(50))) {
        ESP_LOGW(TAG, "hid_queue full on direct send");
    }
}

bool cmd_execute(const cmd_t *cmd)
{
    if (!cmd) return false;

    switch (cmd->kind) {

    case CMD_NOOP:
        return true;

    case CMD_MOVE_REL: {
        /* Clamp to int8 — single HID report. */
        int16_t dx = cmd->a; if (dx >  127) dx =  127; if (dx < -128) dx = -128;
        int16_t dy = cmd->b; if (dy >  127) dy =  127; if (dy < -128) dy = -128;
        uint8_t btns = cmd_current_buttons();
        send_now((int8_t)dx, (int8_t)dy, 0, btns);
        return true;
    }

    case CMD_MOVE_HUMAN: {
        /* Blocks for ~300–900 ms; caller is an HTTP worker thread. */
        uint8_t btns = cmd_current_buttons();
        move_mouse_human(cmd->a, cmd->b, btns);
        return true;
    }

    case CMD_CLICK: {
        uint16_t mask = (uint16_t)cmd->a & BTN_VALID_MASK;
        if (mask == 0) {
            return false;
        }
        state_lock();
        uint8_t prev = s_current_buttons;
        s_current_buttons |= (uint8_t)mask;
        uint8_t held = s_current_buttons;
        state_unlock();

        send_now(0, 0, 0, held);
        vTaskDelay(pdMS_TO_TICKS(20));

        state_lock();
        s_current_buttons = prev & ~(uint8_t)mask;   /* clear what we set */
        uint8_t released = s_current_buttons;
        state_unlock();

        send_now(0, 0, 0, released);
        return true;
    }

    case CMD_BUTTONS_DOWN: {
        uint8_t mask = (uint8_t)cmd->a & BTN_VALID_MASK;
        state_lock();
        s_current_buttons |= mask;
        uint8_t held = s_current_buttons;
        state_unlock();
        send_now(0, 0, 0, held);
        return true;
    }

    case CMD_BUTTONS_UP: {
        uint8_t mask = (uint8_t)cmd->a & BTN_VALID_MASK;
        state_lock();
        s_current_buttons &= ~mask;
        uint8_t held = s_current_buttons;
        state_unlock();
        send_now(0, 0, 0, held);
        return true;
    }

    case CMD_BUTTONS_RELEASE_ALL: {
        state_lock();
        s_current_buttons = 0;
        state_unlock();
        send_now(0, 0, 0, 0);
        return true;
    }

    case CMD_WHEEL: {
        int16_t d = cmd->a; if (d > 127) d = 127; if (d < -128) d = -128;
        send_now(0, 0, (int8_t)d, cmd_current_buttons());
        return true;
    }

    case CMD_AUTOWALK_TOGGLE: {
        state_lock();
        s_autowalk_enabled       = !s_autowalk_enabled;
        s_autowalk_last_tick     = xTaskGetTickCount();
        s_autowalk_next_delay_ms = get_random(700, 1500);
        state_unlock();
        return true;
    }
    }

    return false;
}

void cmd_button_press_action(void)
{
    /* Manual human-move triggered by the GPIO button. */
    int8_t dir;
    state_lock();
    dir = s_autowalk_direction;
    s_autowalk_direction = -s_autowalk_direction;
    state_unlock();

    uint8_t btns = cmd_current_buttons();
    move_mouse_human(300 * dir, 100 * dir, btns);
}

void cmd_autowalk_tick(void)
{
    if (!cmd_autowalk_enabled()) {
        return;
    }
    TickType_t now = xTaskGetTickCount();

    state_lock();
    TickType_t last  = s_autowalk_last_tick;
    uint32_t   delay = s_autowalk_next_delay_ms;
    int8_t     dir   = s_autowalk_direction;
    state_unlock();

    if ((now - last) < pdMS_TO_TICKS(delay)) {
        return;
    }

    int16_t dx = (int16_t)get_random(120, 260) * dir;
    int16_t dy = (int16_t)get_random(40, 120)  * dir;
    uint8_t btns = cmd_current_buttons();
    move_mouse_human(dx, dy, btns);

    state_lock();
    s_autowalk_direction     = -dir;
    s_autowalk_last_tick     = xTaskGetTickCount();
    s_autowalk_next_delay_ms = get_random(700, 1500);
    state_unlock();
}
