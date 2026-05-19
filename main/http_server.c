/*
 * http_server.c — esp_http_server bindings.
 *
 * Every handler:
 *   - reads up to 256 bytes of request body,
 *   - parses with cJSON when a body is expected,
 *   - constructs a cmd_t and calls cmd_execute(),
 *   - returns {"status":"ok"} on success or HTTPD_400_BAD_REQUEST on parse
 *     failure / unknown parameter.
 *
 * Handlers may run concurrently on multiple sockets (LWIP default), so all
 * state mutation happens inside cmd_dispatcher.c (mutex-guarded).
 */
#include "http_server.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "cmd_dispatcher.h"
#include "board_config.h"

static const char *TAG = "http";

#define MAX_BODY 256

/* ---- Body helpers -------------------------------------------------------- */

/* Read the request body into `out` (null-terminated).  Returns ESP_OK on
 * success.  On error, an HTTP 400 has already been sent. */
static esp_err_t read_body(httpd_req_t *req, char *out, size_t out_sz)
{
    if (req->content_len == 0) {
        out[0] = '\0';
        return ESP_OK;  /* empty bodies are legal for /toggle, /release_all */
    }
    if (req->content_len >= out_sz) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    int total = 0;
    while (total < (int)req->content_len) {
        int r = httpd_req_recv(req, out + total, out_sz - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        total += r;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t reply_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t reply_bad(httpd_req_t *req, const char *why)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
}

/* ---- Handlers ------------------------------------------------------------ */

static esp_err_t move_handler(httpd_req_t *req)
{
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) return reply_bad(req, "bad json");

    cJSON *jdx = cJSON_GetObjectItem(root, "dx");
    cJSON *jdy = cJSON_GetObjectItem(root, "dy");
    if (!cJSON_IsNumber(jdx) || !cJSON_IsNumber(jdy)) {
        cJSON_Delete(root);
        return reply_bad(req, "missing dx/dy");
    }

    cmd_t cmd = {
        .kind = CMD_MOVE_REL,
        .a    = (int16_t)jdx->valueint,
        .b    = (int16_t)jdy->valueint,
    };
    cmd_execute(&cmd);
    cJSON_Delete(root);
    return reply_ok(req);
}

static esp_err_t move_human_handler(httpd_req_t *req)
{
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) return reply_bad(req, "bad json");

    cJSON *jdx = cJSON_GetObjectItem(root, "dx");
    cJSON *jdy = cJSON_GetObjectItem(root, "dy");
    if (!cJSON_IsNumber(jdx) || !cJSON_IsNumber(jdy)) {
        cJSON_Delete(root);
        return reply_bad(req, "missing dx/dy");
    }

    cmd_t cmd = {
        .kind = CMD_MOVE_HUMAN,
        .a    = (int16_t)jdx->valueint,
        .b    = (int16_t)jdy->valueint,
    };
    cmd_execute(&cmd);   /* blocks until motion completes */
    cJSON_Delete(root);
    return reply_ok(req);
}

static esp_err_t click_handler(httpd_req_t *req)
{
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    /* "left" by default if no body. */
    uint8_t mask = 0x01;
    if (body[0]) {
        cJSON *root = cJSON_Parse(body);
        if (!root) return reply_bad(req, "bad json");
        cJSON *jb = cJSON_GetObjectItem(root, "button");
        if (cJSON_IsString(jb) && jb->valuestring) {
            if      (strcmp(jb->valuestring, "left")   == 0) mask = 0x01;
            else if (strcmp(jb->valuestring, "right")  == 0) mask = 0x02;
            else if (strcmp(jb->valuestring, "middle") == 0) mask = 0x04;
            else { cJSON_Delete(root); return reply_bad(req, "unknown button"); }
        } else if (cJSON_IsNumber(jb)) {
            mask = (uint8_t)jb->valueint;
        }
        cJSON_Delete(root);
    }

    cmd_t cmd = { .kind = CMD_CLICK, .a = (int16_t)mask };
    if (!cmd_execute(&cmd)) return reply_bad(req, "bad mask");
    return reply_ok(req);
}

static esp_err_t buttons_mask_handler(httpd_req_t *req, cmd_kind_t kind)
{
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return reply_bad(req, "bad json");
    cJSON *jm = cJSON_GetObjectItem(root, "mask");
    if (!cJSON_IsNumber(jm)) { cJSON_Delete(root); return reply_bad(req, "missing mask"); }

    cmd_t cmd = { .kind = kind, .a = (int16_t)jm->valueint };
    cmd_execute(&cmd);
    cJSON_Delete(root);
    return reply_ok(req);
}

static esp_err_t buttons_down_handler(httpd_req_t *req) {
    return buttons_mask_handler(req, CMD_BUTTONS_DOWN);
}
static esp_err_t buttons_up_handler(httpd_req_t *req) {
    return buttons_mask_handler(req, CMD_BUTTONS_UP);
}

static esp_err_t buttons_release_all_handler(httpd_req_t *req)
{
    /* No body required. */
    char body[8];
    (void)read_body(req, body, sizeof(body));
    cmd_t cmd = { .kind = CMD_BUTTONS_RELEASE_ALL };
    cmd_execute(&cmd);
    return reply_ok(req);
}

static esp_err_t wheel_handler(httpd_req_t *req)
{
    char body[MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return reply_bad(req, "bad json");
    cJSON *jd = cJSON_GetObjectItem(root, "delta");
    if (!cJSON_IsNumber(jd)) { cJSON_Delete(root); return reply_bad(req, "missing delta"); }

    cmd_t cmd = { .kind = CMD_WHEEL, .a = (int16_t)jd->valueint };
    cmd_execute(&cmd);
    cJSON_Delete(root);
    return reply_ok(req);
}

static esp_err_t autowalk_toggle_handler(httpd_req_t *req)
{
    /* No body required. */
    char body[8];
    (void)read_body(req, body, sizeof(body));
    cmd_t cmd = { .kind = CMD_AUTOWALK_TOGGLE };
    cmd_execute(&cmd);

    char out[64];
    snprintf(out, sizeof(out), "{\"status\":\"ok\",\"autowalk\":%s}",
             cmd_autowalk_enabled() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cmd_status_t s;
    cmd_get_status(&s);
    char out[96];
    snprintf(out, sizeof(out),
             "{\"buttons\":%u,\"autowalk\":%s,\"usb_ready\":%s}",
             s.buttons,
             s.autowalk  ? "true" : "false",
             s.usb_ready ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t help_handler(httpd_req_t *req)
{
    static const char help[] =
        "{\"endpoints\":["
            "\"POST /api/v1/move {dx,dy}\","
            "\"POST /api/v1/move_human {dx,dy}\","
            "\"POST /api/v1/click {button:'left'|'right'|'middle'}\","
            "\"POST /api/v1/buttons/down {mask}\","
            "\"POST /api/v1/buttons/up {mask}\","
            "\"POST /api/v1/buttons/release_all\","
            "\"POST /api/v1/wheel {delta}\","
            "\"POST /api/v1/autowalk/toggle\","
            "\"GET  /api/v1/status\","
            "\"GET  /api/v1/help\""
        "]}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, help);
}

/* ---- Built-in control panel ---------------------------------------------- *
 * Tiny static page so a phone connected to the AP gets something useful at /
 * without anyone having to type curl commands.  The frontend/index.html
 * file in the repo is a richer version intended to be hosted elsewhere.
 */
static const char s_index_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>mouseum</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:1em auto;padding:0 1em}"
    "button{font-size:1.1em;padding:.6em 1em;margin:.2em;border-radius:.5em;border:1px solid #888;background:#f8f8f8}"
    ".row{display:flex;gap:.3em;justify-content:center}"
    "</style></head><body>"
    "<h2>mouseum</h2>"
    "<div class=row><button onclick=\"mv(-50,0)\">&larr;</button>"
    "<button onclick=\"mv(0,-50)\">&uarr;</button>"
    "<button onclick=\"mv(0,50)\">&darr;</button>"
    "<button onclick=\"mv(50,0)\">&rarr;</button></div>"
    "<div class=row><button onclick=\"hm(300,100)\">human R</button>"
    "<button onclick=\"hm(-300,-100)\">human L</button></div>"
    "<div class=row><button onclick=\"cl('left')\">L click</button>"
    "<button onclick=\"cl('right')\">R click</button>"
    "<button onclick=\"cl('middle')\">M click</button></div>"
    "<div class=row><button onclick=\"wh(-1)\">scroll &uarr;</button>"
    "<button onclick=\"wh(1)\">scroll &darr;</button>"
    "<button onclick=\"aw()\">autowalk</button></div>"
    "<pre id=s></pre>"
    "<script>"
    "const B='/api/v1';"
    "async function P(p,b){const r=await fetch(B+p,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):''});return r.json()}"
    "const mv=(x,y)=>P('/move',{dx:x,dy:y});"
    "const hm=(x,y)=>P('/move_human',{dx:x,dy:y});"
    "const cl=(b)=>P('/click',{button:b});"
    "const wh=(d)=>P('/wheel',{delta:d});"
    "const aw=()=>P('/autowalk/toggle').then(j=>document.getElementById('s').textContent=JSON.stringify(j));"
    "setInterval(async()=>{const r=await fetch(B+'/status');document.getElementById('s').textContent=await r.text();},1500);"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

/* ---- Registration -------------------------------------------------------- */

static const httpd_uri_t s_routes[] = {
    { .uri = "/",                              .method = HTTP_GET,  .handler = index_handler },
    { .uri = "/api/v1/move",                   .method = HTTP_POST, .handler = move_handler },
    { .uri = "/api/v1/move_human",             .method = HTTP_POST, .handler = move_human_handler },
    { .uri = "/api/v1/click",                  .method = HTTP_POST, .handler = click_handler },
    { .uri = "/api/v1/buttons/down",           .method = HTTP_POST, .handler = buttons_down_handler },
    { .uri = "/api/v1/buttons/up",             .method = HTTP_POST, .handler = buttons_up_handler },
    { .uri = "/api/v1/buttons/release_all",    .method = HTTP_POST, .handler = buttons_release_all_handler },
    { .uri = "/api/v1/wheel",                  .method = HTTP_POST, .handler = wheel_handler },
    { .uri = "/api/v1/autowalk/toggle",        .method = HTTP_POST, .handler = autowalk_toggle_handler },
    { .uri = "/api/v1/status",                 .method = HTTP_GET,  .handler = status_handler },
    { .uri = "/api/v1/help",                   .method = HTTP_GET,  .handler = help_handler },
};

void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = MOUSEUM_HTTP_PORT;
    cfg.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]) + 2;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK || srv == NULL) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &s_routes[i]));
    }
    ESP_LOGI(TAG, "HTTP server listening on :%d", (int)MOUSEUM_HTTP_PORT);
}
