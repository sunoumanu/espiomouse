/*
 * wifi_manager.c — minimal ESP-IDF Soft-AP bring-up.
 *
 * Why Soft-AP first?  Zero configuration: the user just connects to the
 * `mouseum` SSID and pokes 192.168.4.1.  No NVS-stored credentials.
 *
 * NVS init must run before esp_wifi_init() — done from app_main.
 */
#include "wifi_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"   /* MACSTR / MAC2STR — moved out of esp_wifi.h in IDF v5.x */

#include "board_config.h"

static const char *TAG = "wifi";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) return;

    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "client joined: " MACSTR " (aid=%d)", MAC2STR(e->mac), e->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "client left: " MACSTR " (aid=%d)", MAC2STR(e->mac), e->aid);
        break;
    }
    default:
        break;
    }
}

void wifi_manager_start_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t ap_cfg = {0};

    /* Copy SSID + password as bounded memcpy.  We avoid strncpy/strnlen on
     * the fixed-size struct buffers because GCC 14's stringop-overread
     * analysis fires when a short string literal is paired with a longer
     * destination bound. */
    const size_t ssid_len = strlen(MOUSEUM_AP_SSID);
    const size_t pass_len = strlen(MOUSEUM_AP_PASS);

    const size_t ssid_copy = ssid_len < sizeof(ap_cfg.ap.ssid)
                             ? ssid_len : sizeof(ap_cfg.ap.ssid);
    memcpy(ap_cfg.ap.ssid, MOUSEUM_AP_SSID, ssid_copy);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_copy;

    ap_cfg.ap.channel         = MOUSEUM_AP_CHANNEL;
    ap_cfg.ap.max_connection  = MOUSEUM_AP_MAX_STA;
    ap_cfg.ap.beacon_interval = 100;

    if (pass_len == 0) {
        /* Open network — convenient for first-light, terrible for shared
         * environments.  Override MOUSEUM_AP_PASS in board_config.h for WPA2. */
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        const size_t pass_copy = pass_len < sizeof(ap_cfg.ap.password)
                                 ? pass_len : sizeof(ap_cfg.ap.password);
        memcpy(ap_cfg.ap.password, MOUSEUM_AP_PASS, pass_copy);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Soft-AP up — SSID:%s channel:%d auth:%s",
             MOUSEUM_AP_SSID, MOUSEUM_AP_CHANNEL,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2");
    ESP_LOGI(TAG, "Connect to the AP and browse to http://192.168.4.1/");
}
