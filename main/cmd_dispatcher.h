/*
 * cmd_dispatcher.h — transport-agnostic command surface.
 *
 * The HTTP server (or any future transport: WebSocket, BLE, I2C) parses its
 * inbound message into a `cmd_t` struct and calls cmd_execute().  This file
 * is the single place where state mutations happen, so it owns the mutex
 * that guards `current_buttons` and `autowalk_enabled`.
 *
 * Long-running commands (CMD_MOVE_HUMAN) block the caller — that is on
 * purpose: HTTP handlers run in their own LwIP worker thread, so blocking
 * one of those does not affect the USB task pinned to the other core.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CMD_NOOP = 0,
    CMD_MOVE_REL,        /* a=dx, b=dy   (int8 range, but accepted as int16 for the wire) */
    CMD_MOVE_HUMAN,      /* a=dx, b=dy   (int16) */
    CMD_CLICK,           /* a=button mask (1=L, 2=R, 4=M) */
    CMD_BUTTONS_DOWN,    /* a=mask */
    CMD_BUTTONS_UP,      /* a=mask */
    CMD_BUTTONS_RELEASE_ALL,
    CMD_WHEEL,           /* a=delta (int8) */
    CMD_AUTOWALK_TOGGLE, /* (no args) */
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int16_t    a;
    int16_t    b;
} cmd_t;

/* Snapshot of mutable engine state returned by /api/v1/status. */
typedef struct {
    uint8_t buttons;
    bool    autowalk;
    bool    usb_ready;
} cmd_status_t;

void cmd_dispatcher_init(void);

/*
 * Execute a command.  Thread-safe.  Returns false if validation fails (e.g.
 * unknown button mask), true on success.  Long moves block the caller —
 * that's fine when called from the HTTP worker thread.
 */
bool cmd_execute(const cmd_t *cmd);

/* Read-only accessors used by the status endpoint and the autowalk pacer. */
void cmd_get_status(cmd_status_t *out);
bool cmd_autowalk_enabled(void);
uint8_t cmd_current_buttons(void);

/*
 * Run one tick of autowalk if enabled.  Called from http_cmd_task on a 50 ms
 * cadence.  Tracks its own internal direction + pacing.
 */
void cmd_autowalk_tick(void);

/* Manual one-shot human-walk used by the GPIO button. */
void cmd_button_press_action(void);
