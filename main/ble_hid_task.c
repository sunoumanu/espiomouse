/*
 * ble_hid_task.c — BLE HID-over-GATT implementation of hid_transport.h.
 *
 * Used on the ESP32-C6 (and any other chip selected by main/CMakeLists.txt
 * that has BLE but no native USB OTG).
 *
 * What we expose to the host
 * --------------------------
 *   - GAP advertising with the HID appearance (0x03C2 = "Mouse").
 *   - Three primary GATT services:
 *       * Device Information (0x180A) — manufacturer + PnP ID.
 *       * Battery Service     (0x180F) — fixed 100% (we are USB-powered).
 *       * HID                 (0x1812) — Report Map + Input Report.
 *
 * Why NimBLE
 * ----------
 *   The ESP32-C6 ships with NimBLE only (no Bluedroid).  NimBLE is the
 *   smaller, modern stack and is the default for the C6/C2/H2 line.
 *
 * Pairing model
 * -------------
 *   The HID service requires authenticated/encrypted access.  We use
 *   "JustWorks" pairing (no passkey, no MITM protection) since this is a
 *   hobby tool — adequate for desk-range use.  After the first pair, the
 *   host re-bonds automatically on reconnect.
 *
 * Throughput
 * ----------
 *   We notify on the Input Report characteristic at the connection
 *   interval set by the host (typically 7.5–15 ms).  hid_queue drains at
 *   roughly that cadence; if a step would block the queue for >5 ms, we
 *   drop it, mirroring the USB path's behaviour.
 */

#include "hid_transport.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mouse_engine.h"
#include "board_config.h"

static const char *TAG = "ble_hid";

/* ---- State --------------------------------------------------------------- */

static uint8_t   s_own_addr_type = 0;
static uint16_t  s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t  s_report_input_handle = 0;
static uint16_t  s_battery_input_handle = 0;
static bool      s_input_notify_enabled = false;
static bool      s_paired = false;

/* ---- Standard mouse report descriptor (4-byte boot-mouse-compatible) ----- *
 * Matches the USB descriptor layout: buttons(8), X(8), Y(8), wheel(8).      */
static const uint8_t s_hid_report_map[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop)        */
    0x09, 0x02,             /* Usage (Mouse)                       */
    0xA1, 0x01,             /* Collection (Application)            */
        0x09, 0x01,         /*   Usage (Pointer)                   */
        0xA1, 0x00,         /*   Collection (Physical)             */
            0x05, 0x09,     /*     Usage Page (Buttons)            */
            0x19, 0x01,     /*     Usage Minimum (1)               */
            0x29, 0x03,     /*     Usage Maximum (3)               */
            0x15, 0x00,     /*     Logical Minimum (0)             */
            0x25, 0x01,     /*     Logical Maximum (1)             */
            0x95, 0x03,     /*     Report Count (3)                */
            0x75, 0x01,     /*     Report Size (1)                 */
            0x81, 0x02,     /*     Input (Data,Var,Abs)            */
            0x95, 0x01,     /*     Report Count (1)                */
            0x75, 0x05,     /*     Report Size (5)                 */
            0x81, 0x03,     /*     Input (Const)  -- padding       */
            0x05, 0x01,     /*     Usage Page (Generic Desktop)    */
            0x09, 0x30,     /*     Usage (X)                       */
            0x09, 0x31,     /*     Usage (Y)                       */
            0x09, 0x38,     /*     Usage (Wheel)                   */
            0x15, 0x81,     /*     Logical Minimum (-127)          */
            0x25, 0x7F,     /*     Logical Maximum ( 127)          */
            0x75, 0x08,     /*     Report Size (8)                 */
            0x95, 0x03,     /*     Report Count (3)                */
            0x81, 0x06,     /*     Input (Data,Var,Rel)            */
        0xC0,               /*   End Collection                    */
    0xC0,                   /* End Collection                      */
};

/* HID Information characteristic: bcdHID 1.11, country 0, flags 0x02
 * (NormallyConnectable). */
static const uint8_t s_hid_info[] = { 0x11, 0x01, 0x00, 0x02 };

/* PnP ID (DIS): VID source=USB, VID 0x303A, PID 0x8234, version 0x0100. */
static const uint8_t s_pnp_id[] = {
    0x02,                   /* Vendor ID Source = USB-IF           */
    0x3A, 0x30,             /* Vendor ID  (little-endian) 0x303A   */
    0x34, 0x82,             /* Product ID (little-endian) 0x8234   */
    0x00, 0x01,             /* Product Version 0x0100              */
};

/* ---- GATT access callbacks ---------------------------------------------- *
 * For simple read-only characteristics, we pack {pointer,length} into a
 * static_blob struct and pass its address as the access callback's `arg`. */

struct static_blob { const void *data; size_t len; };

static int access_blob(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle;
    const struct static_blob *b = (const struct static_blob *)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, b->data, b->len) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct static_blob blob_report_map = {
    .data = s_hid_report_map, .len = sizeof(s_hid_report_map),
};
static const struct static_blob blob_hid_info = {
    .data = s_hid_info, .len = sizeof(s_hid_info),
};
static const struct static_blob blob_pnp_id = {
    .data = s_pnp_id, .len = sizeof(s_pnp_id),
};

/* Battery level — always 100, we run from USB. */
static int access_battery_level(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    uint8_t lvl = 100;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &lvl, 1) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* HID Control Point: write-only one-byte command (0=suspend, 1=exit). We
 * accept and ignore — we have no power-management actions to take. */
static int access_hid_ctrl(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    return BLE_ATT_ERR_UNLIKELY;
}

/* Protocol Mode: 0 = boot, 1 = report.  We default to report and accept
 * the host's selection without changing behaviour (we send the same 4-byte
 * report either way, which is also the boot-mouse layout). */
static uint8_t s_protocol_mode = 1;
static int access_proto_mode(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_protocol_mode, 1) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t got = OS_MBUF_PKTLEN(ctxt->om);
        if (got != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        ble_hs_mbuf_to_flat(ctxt->om, &s_protocol_mode, 1, NULL);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Input Report characteristic.  Host reads return zeros; we notify pushes. */
static int access_input_report(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    static const uint8_t zero4[4] = {0};
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, zero4, sizeof(zero4)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Report Reference descriptor — pairs the Input Report with report ID 0
 * and type "Input". */
static const uint8_t s_report_ref[] = { 0x00, 0x01 };  /* ID=0, type=Input */
static int access_report_ref(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return os_mbuf_append(ctxt->om, s_report_ref, sizeof(s_report_ref)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---- GATT service table -------------------------------------------------- */

static const struct ble_gatt_svc_def s_services[] = {
    /* Device Information Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A50),   /* PnP ID */
                .access_cb = access_blob,
                .arg     = (void *)&blob_pnp_id,
                .flags   = BLE_GATT_CHR_F_READ,
            },
            { 0 }
        }
    },
    /* Battery Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A19),   /* Battery Level */
                .access_cb = access_battery_level,
                .val_handle = &s_battery_input_handle,
                .flags   = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        }
    },
    /* HID Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A4A),   /* HID Information */
                .access_cb = access_blob,
                .arg     = (void *)&blob_hid_info,
                .flags   = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A4C),   /* HID Control Point */
                .access_cb = access_hid_ctrl,
                .flags   = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A4B),   /* Report Map */
                .access_cb = access_blob,
                .arg     = (void *)&blob_report_map,
                .flags   = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A4E),   /* Protocol Mode */
                .access_cb = access_proto_mode,
                .flags   = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid    = BLE_UUID16_DECLARE(0x2A4D),   /* Report (Input) */
                .access_cb = access_input_report,
                .val_handle = &s_report_input_handle,
                .flags   = BLE_GATT_CHR_F_READ
                        | BLE_GATT_CHR_F_NOTIFY
                        | BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid     = BLE_UUID16_DECLARE(0x2908),  /* Report Reference */
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = access_report_ref,
                    },
                    { 0 }
                }
            },
            { 0 }
        }
    },
    { 0 }
};

/* ---- GAP / connection plumbing ------------------------------------------ */

static void start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected; handle=%d", s_conn_handle);
            /* Require encryption for the HID service. */
            ble_gap_security_initiate(s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason=0x%x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_input_notify_enabled = false;
        s_paired = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %s",
                 event->enc_change.status == 0 ? "ON" : "FAILED");
        s_paired = (event->enc_change.status == 0);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_report_input_handle) {
            s_input_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "input report notify -> %s",
                     s_input_notify_enabled ? "ENABLED" : "disabled");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* JustWorks: nothing to do.  If the host requests a passkey we
         * decline. */
        ble_sm_inject_io(event->passkey.conn_handle,
                         &(struct ble_sm_io){ .action = BLE_SM_IOACT_NONE });
        break;

    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)MOUSEUM_BLE_NAME;
    fields.name_len = (uint8_t)strlen(MOUSEUM_BLE_NAME);
    fields.name_is_complete = 1;
    /* Appearance: 0x03C2 = HID Mouse. */
    fields.appearance = 0x03C2;
    fields.appearance_is_present = 1;
    /* Advertise the HID service UUID so OSes find us as an input device. */
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", MOUSEUM_BLE_NAME);
}

/* ---- NimBLE host stack lifecycle ---------------------------------------- */

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "ble host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed: %d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto failed: %d", rc); return; }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "ble addr: %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();           /* never returns */
    nimble_port_freertos_deinit();
}

/* ---- Public hid_transport API ------------------------------------------- */

void hid_transport_init(void)
{
    ESP_LOGI(TAG, "Initializing NimBLE HID");

    /* nvs_flash_init() has already been called by app_main.  NimBLE needs
     * NVS for the bonding store. */

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Security: JustWorks bonding, encryption required for HID reads. */
    ble_hs_cfg.sm_io_cap     = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding    = 1;
    ble_hs_cfg.sm_mitm       = 0;
    ble_hs_cfg.sm_sc         = 1;          /* LE Secure Connections */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* GAP "appearance" + device name. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(MOUSEUM_BLE_NAME));
    ESP_ERROR_CHECK(ble_svc_gap_device_appearance_set(0x03C2));

    /* Register our HID + DIS + Battery services. */
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));

    /* Kick off the NimBLE host task — advertising starts in on_sync(). */
    nimble_port_freertos_init(host_task);
}

bool hid_transport_is_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE
        && s_paired
        && s_input_notify_enabled;
}

/* Send one 4-byte mouse report as a GATT notification.  Returns true on
 * success. */
static bool send_report(const mouse_report_t *r)
{
    if (!hid_transport_is_ready()) return false;

    uint8_t buf[4] = {
        r->buttons,
        (uint8_t)r->x,
        (uint8_t)r->y,
        (uint8_t)r->wheel,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_report_input_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: %d", rc);
        return false;
    }
    return true;
}

void hid_transport_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "hid_transport_task (BLE) running on core %d", xPortGetCoreID());

    hid_queue_item_t item;
    for (;;) {
        if (xQueueReceive(hid_queue, &item, pdMS_TO_TICKS(10000)) != pdPASS) {
            continue;
        }

        /* Wait briefly for the host to subscribe / re-pair if we're not
         * ready, then drop the report. */
        if (!hid_transport_is_ready()) {
            TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(20);
            while (!hid_transport_is_ready() && xTaskGetTickCount() < until) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (!hid_transport_is_ready()) {
                /* No subscriber — drain the queue so we don't burst when a
                 * host eventually reconnects. */
                while (xQueueReceive(hid_queue, &item, 0) == pdPASS) { }
                continue;
            }
        }

        if (!send_report(&item.r)) {
            ESP_LOGW(TAG, "dropped HID notification");
        }
    }
}
