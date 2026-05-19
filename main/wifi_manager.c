/*
 * wifi_manager.c — Soft-AP or station-mode bring-up.
 *
 * Mode is picked by `wifi_manager_start()` based on whether
 * MOUSEUM_STA_SSID is set.  In station mode we block until we get an IP
 * (or until MOUSEUM_STA_TIMEOUT_MS expires); the HTTP server is started
 * from main.c only after this returns, so it can bind to the assigned IP.
 *
 * NVS must be initialised before this runs — handled by app_main().
 */
#include "wifi_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "board_config.h"

static const char *TAG = "wifi";

#define STA_GOT_IP_BIT       BIT0
#define STA_CONNECT_FAIL_BIT BIT1
static EventGroupHandle_t s_sta_evt = NULL;
static int                s_sta_retry = 0;
#define STA_MAX_RETRY        5

/* ---- shared event handler --------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    /* ----- AP-side events ----- */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "client joined: " MACSTR " (aid=%d)", MAC2STR(e->mac), e->aid);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "client left: " MACSTR " (aid=%d)", MAC2STR(e->mac), e->aid);
        return;
    }

    /* ----- STA-side events ----- */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA: connecting...");
        esp_wifi_connect();
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_retry < STA_MAX_RETRY) {
            ++s_sta_retry;
            ESP_LOGW(TAG, "STA: disconnected, retry %d/%d", s_sta_retry, STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "STA: connect failed after %d retries", STA_MAX_RETRY);
            if (s_sta_evt) xEventGroupSetBits(s_sta_evt, STA_CONNECT_FAIL_BIT);
        }
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA: connected — IP " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA: browse to http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        s_sta_retry = 0;
        if (s_sta_evt) xEventGroupSetBits(s_sta_evt, STA_GOT_IP_BIT);
        return;
    }
}

/* ---- one-time stack init shared by AP and STA ------------------------- */
static bool s_stack_inited = false;
static void wifi_stack_init_once(void)
{
    if (s_stack_inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    s_stack_inited = true;
}

/* ---- Soft-AP ---------------------------------------------------------- */
void wifi_manager_start_softap(void)
{
    wifi_stack_init_once();
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
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

/* ---- Station ---------------------------------------------------------- */
void wifi_manager_start_sta(void)
{
    wifi_stack_init_once();
    esp_netif_create_default_wifi_sta();

    if (!s_sta_evt) {
        s_sta_evt = xEventGroupCreate();
    }
    s_sta_retry = 0;

    wifi_config_t sta_cfg = {0};
    const size_t ssid_len = strlen(MOUSEUM_STA_SSID);
    const size_t pass_len = strlen(MOUSEUM_STA_PASS);
    const size_t ssid_copy = ssid_len < sizeof(sta_cfg.sta.ssid)
                             ? ssid_len : sizeof(sta_cfg.sta.ssid);
    const size_t pass_copy = pass_len < sizeof(sta_cfg.sta.password)
                             ? pass_len : sizeof(sta_cfg.sta.password);
    memcpy(sta_cfg.sta.ssid,     MOUSEUM_STA_SSID, ssid_copy);
    memcpy(sta_cfg.sta.password, MOUSEUM_STA_PASS, pass_copy);

    /* Accept any auth mode the AP advertises that has a password set;
     * open networks require explicit WIFI_AUTH_OPEN. */
    sta_cfg.sta.threshold.authmode =
        (pass_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable    = true;
    sta_cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA: joining \"%s\"...", MOUSEUM_STA_SSID);

    /* Block here until we get an IP or give up.  The HTTP server starts
     * after this returns, so it can advertise the right address. */
    EventBits_t bits = xEventGroupWaitBits(
        s_sta_evt,
        STA_GOT_IP_BIT | STA_CONNECT_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(MOUSEUM_STA_TIMEOUT_MS));

    if (bits & STA_GOT_IP_BIT) {
        /* Logged already by the event handler. */
        return;
    }
    if (bits & STA_CONNECT_FAIL_BIT) {
        ESP_LOGE(TAG, "STA: gave up after retries.  Check credentials.");
    } else {
        ESP_LOGE(TAG, "STA: timeout after %d ms waiting for IP.",
                 (int)MOUSEUM_STA_TIMEOUT_MS);
    }
    ESP_LOGW(TAG, "STA: continuing without network — HTTP API will be "
                  "unreachable until reset / fix credentials.");
}

/* ---- Mode picker ------------------------------------------------------ */
void wifi_manager_start(void)
{
    if (strlen(MOUSEUM_STA_SSID) > 0) {
        wifi_manager_start_sta();
    } else {
        wifi_manager_start_softap();
    }
}
