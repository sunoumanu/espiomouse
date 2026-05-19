# mouseum-esp32 Design Document

> Target: ESP32-S3 (primary), ESP32-S2 (secondary)  
> Framework: ESP-IDF (CMake + FreeRTOS)

---

## 1. Executive Summary

**mouseum** is a USB HID mouse emulator with human-like cursor movement (quadratic Bezier curves, Fitts's Law easing, micro-jitter) controlled via:
- a physical trigger button, and
- a rich **HTTP REST API** served over Wi-Fi (or wired Ethernet where available).

The existing STM32 implementation runs on STM32F411CEU6 using bare-metal superloop + ST HAL + UART command line. This document defines how to replicate **identical mouse functionality** on ESP32, replacing the wired UART interface with a wireless/cable-free HTTP control plane.

---

## 2. Reference Hardware / Software Baseline

### 2.1 STM32 Reference (Original)

| Property | Value |
|----------|-------|
| MCU | STM32F411CEU6 (Cortex-M4, 96 MHz, 512 KB Flash, 128 KB RAM) |
| USB | OTG FS (on-chip, full-speed) |
| Control Transport | USART1 (PA9/PA10, 115200 8N1) — ASCII command line |
| Button | PA0, active-low, internal pull-up |
| Framework | STM32CubeIDE / HAL (bare-metal, no RTOS) |
| USB Stack | ST USB Device Library (HID class) |
| PRNG | xorshift32 seeded from DWT cycle counter |
| Build | GNU ARM GCC + managed Makefile |

### 2.2 Key Algorithms (Unchanged)

| Algorithm | Details |
|-----------|---------|
| `MoveMouseHuman()` | Quadratic Bezier with random control point, sine ease-in-out, 30–60 steps, 8–14 ms random delays |
| `xorshift32` | Fast non-cryptographic PRNG for jitter & timing randomization |
| `EaseInOut(t)` | `0.5 * (1 - cos(t * PI))` |
| HID report | 4 bytes: `buttons (uint8), x (int8), y (int8), wheel (int8)` |
| USB VID/PID | 1155 / 22315 (ST default) |

### 2.3 Original UART Command Grammar (For Reference)

These ASCII commands become HTTP REST endpoints:

| Command | Args | Action |
|---------|------|--------|
| `M <dx> <dy>` | int8 int8 | Relative move |
| `H <dx> <dy>` | int16 int16 | Human-like Bezier move |
| `L` / `R` / `C` | — | Left / Right / Middle click |
| `D <mask>` | uint8 | Press buttons |
| `U <mask>` | uint8 | Release buttons |
| `Z` | — | Release all |
| `W <delta>` | int8 | Scroll wheel |
| `A` | — | Toggle autowalk demo |
| `?` | — | Print help |

---

## 3. ESP32 Target Selection

### 3.1 Recommended MCU: ESP32-S3

| Feature | ESP32-S3 | ESP32-S2 | ESP32 (classic) | ESP32-C3 |
|---------|----------|----------|-----------------|----------|
| Native USB OTG | Yes (FS) | Yes (FS) | No | Yes (FS) |
| Wi-Fi | 2.4 GHz 802.11 b/g/n | 2.4 GHz | 2.4 GHz | 2.4 GHz |
| CPU | Xtensa LX7 dual-core 240 MHz | Xtensa LX7 single 240 MHz | Xtensa LX6 dual 240 MHz | RISC-V 160 MHz |
| RAM | 512 KB SRAM + 8 MB PSRAM opt | 320 KB + PSRAM | 520 KB | 400 KB |
| Flash | Up to 16 MB | Up to 16 MB | Up to 16 MB | Up to 16 MB |
| FPU | Yes | Yes | Yes | No |
| FreeRTOS | Yes ( SMP ) | Yes | Yes | Yes |
| HID Suitability | **Best** — dual-core lets USB task isolate | Good | Requires external USB chip | Good |

**Decision:** Primary target is **ESP32-S3**. Dual-core is important because the HTTP server task (handling TLS/TCP parsing) should not starve the USB HID task.

### 3.2 Why ESP32-S3 is Ideal

- Native USB OTG FS means **TinyUSB** (already integrated in ESP-IDF) can present the same HID mouse descriptor.
- Dual-core allows **isolation**: USB HID on Core 0, HTTP server + command engine on Core 1.
- On-chip Wi-Fi removes the need for a separate Ethernet PHY or wired UART dongle.
- 240 MHz + FPU handles trigonometric easing faster than the 96 MHz M4 reference.
- PSRAM option allows serving a small web UI (HTML/JS) if desired.

---

## 4. Top-Level Architecture

### 4.1 Task Model (FreeRTOS)

Replace the STM32 bare-metal superloop with **three FreeRTOS tasks**:

```
+---------------------+        +-------------------------+
|  usb_hid_task       |        |  http_cmd_task          |
|  (Core 0, High)     |<------>|  (Core 1, Normal)       |
|  Priority: 10       |  Queue |  Priority: 5            |
+---------------------+        +-------------------------+
         ^                              ^
         | TinyUSB callbacks            | HTTP GET/POST from Wi-Fi STA/AP
         |                              |
    +----+----+                   +-----+----------+
    | USB OTG |                   | Wi-Fi / LwIP   |
    | (HW)    |                   | (HW+Stack)     |
    +---------+                   +----------------+

+-------------------------+
|  button_poll_task       |
|  (Core 1, Low)          |
|  Priority: 3            |
+-------------------------+
```

| Task | Core | Priority | Role |
|------|------|----------|------|
| `usb_hid_task` | 0 (PRO) | 10 (high) | Runs TinyUSB device stack, services USB interrupts, sends HID reports from a queue. Never blocks for >1 ms. |
| `http_cmd_task` | 1 (APP) | 5 (normal) | Starts `esp_http_server`. Parses HTTP requests into mouse actions, runs `MoveMouseHuman()` (blocking OK here), handles autowalk. |
| `button_poll_task` | 1 (APP) | 3 (low) | Polls GPIO button state with de-bounce. Can be folded into `http_cmd_task` if desired. |

**Inter-task communication:**
- A **FreeRTOS Queue** (`hid_report_queue`) carries `MouseReport` structures from `http_cmd_task` to `usb_hid_task`.
- Queue depth: 16 reports (plenty for bursts; `MoveMouseHuman()` produces ~30–60 reports spaced 8–14 ms).
- `usb_hid_task` blocks on queue receive; when a report arrives it calls `tud_hid_mouse_report()`.
- `http_cmd_task` blocks inside `httpd_uri_handler_t` callbacks waiting for client requests.

### 4.2 ISR Model

| ISR | Core | Priority | Action |
|-----|------|----------|--------|
| `GPIO_BUTTON_ISR` | Any | Level 3 | Set event flag (`button_pressed`). De-bounce in task context. Optional: use polling instead of ISR. |
| `USB_OTG_ISR` | 0 | Level 1 (highest) | Delegated to TinyUSB IRQ handler. TinyUSB runs mostly in ISR context; our task just pumps `tud_task()`. |
| `Wi-Fi MAC ISR` | Any | Level 1 | Managed by ESP-IDF Wi-Fi driver; not directly visible to application code. |

> **Note:** TinyUSB on ESP-IDF already installs its ISR via the driver. The `usb_hid_task` primarily calls `tud_task()` in a tight loop to process events, and waits on the HID report queue.

---

## 5. Component Mapping (STM32 → ESP32)

### 5.1 USB HID Device Stack

| STM32 | ESP32 Equivalent |
|-------|------------------|
| ST USB Device Library + `USBD_HID_SendReport()` | **TinyUSB** (`tud_hid_mouse_report()`) — bundled in ESP-IDF |
| `USBD_HID_SendReport(&hUsbDeviceFS, buf, len)` | `tud_hid_mouse_report(report_id, buttons, x, y, vertical, horizontal)` |
| `USBD_State` enum (`USBD_STATE_CONFIGURED`) | `tud_mounted()` + `tud_ready()` |
| VID/PID in `usbd_desc.c` | Set in `tusb_config.h` or `sdkconfig` (`CONFIG_TINYUSB_DESC_CUSTOM_VPID`) |
| Serial number from unique ID | `esp_efuse_mac_get_default()` → hex-encode into string descriptor |

**TinyUSB descriptor changes:**
- Replace manufacturer string `"STMicroelectronics"` → `"Espressif"` or custom.
- Replace product string `"STM32 Human interface"` → `"ESP32 Human interface"`.
- Keep VID/PID or switch to Espressif test VID (e.g., `0x303A` / custom PID).

### 5.2 HTTP Server Command Interface

| STM32 UART | ESP32 HTTP Equivalent |
|------------|----------------------|
| USART1 115200 8N1 | **Wi-Fi STA mode** (joins existing AP) or **Soft-AP mode** (creates its own network). HTTP server on port 80 (or 8080). |
| ASCII line protocol (`M 10 20\r\n`) | **REST JSON API** (`POST /api/move` with JSON body `{ "dx": 10, "dy": 20 }`) |
| `HAL_UART_Transmit` replies (`OK\r\n`) | HTTP 200/204 responses with JSON status body |
| `strtol()` parsing | `cJSON` or lightweight string parser for query parameters |

**ESP-IDF approach:**
Use the built-in **HTTP Server** (`esp_http_server` component). It is event-driven (handler callbacks) and runs inside the task that calls `httpd_start()`. No custom ISR is needed — LwIP handles TCP in its own task.

**Wi-Fi mode recommendation:**
- **Primary:** **Soft-AP mode** (`esp_wifi_set_mode(WIFI_MODE_AP)`). The ESP32 creates a network (e.g., `SSID: mouseum`, no password or WPA2). The user connects their laptop/phone to this SSID and hits `http://192.168.4.1/`. Zero configuration, no dependency on external Wi-Fi.
- **Secondary:** **STA mode** (`WIFI_MODE_STA`). ESP32 joins an existing network. Useful for fixed installations. Store credentials in NVS.

**Security note:** For a development/hobby tool, plain HTTP on a local Soft-AP is acceptable. If running on a production network, consider enabling HTTPS (mbedTLS is built into ESP-IDF).

### 5.3 GPIO Button

| STM32 | ESP32 Equivalent |
|-------|------------------|
| `PA0`, input, pull-up, active-low | Any GPIO (e.g., `GPIO_NUM_0` — BOOT button on dev boards, already has pull-up), `gpio_set_direction()` + `gpio_set_pull_mode()` |
| `HAL_GPIO_ReadPin()` | `gpio_get_level()` |
| Debounce in main loop (`HAL_Delay(1000)` after press) | De-bounce in `button_poll_task` with `vTaskDelay(pdMS_TO_TICKS(1000))` |

### 5.4 Timing / Delay

| STM32 | ESP32 Equivalent |
|-------|------------------|
| `HAL_GetTick()` (1 ms systick) | `xTaskGetTickCount()` (FreeRTOS ticks, typically 1 ms) |
| `HAL_Delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` (yields CPU; preferred in tasks) |
| Busy-wait for `USBD_BUSY` | Do **not** busy-wait in ESP32. Instead, push to queue and let `usb_hid_task` handle retries, or use `tud_ready()` before sending. |

> **Critical difference:** The STM32 code busy-loops with `HAL_Delay(1)` while `USBD_HID_SendReport` returns `USBD_BUSY`. In FreeRTOS, busy-looping starves lower-priority tasks. The queue-based approach eliminates this: `http_cmd_task` never waits for USB; it just enqueues. If the queue is full (very unlikely), drop the report.

### 5.5 PRNG

| STM32 | ESP32 Equivalent |
|-------|------------------|
| `xorshift32` seeded from `DWT->CYCCNT` | Keep `xorshift32` for deterministic cross-platform behavior, or use `esp_random()` (hardware RNG) for better entropy |
| `GetRandom(min, max)` | Same algorithm, seeded from `esp_timer_get_time()` XOR `esp_random()` at boot |

**Recommendation:** Retain the `xorshift32` implementation to preserve the exact same mouse-movement feel, but seed it with `esp_random()` for stronger non-determinism.

### 5.6 Math / Easing

| STM32 | ESP32 Equivalent |
|-------|------------------|
| `cosf()` from newlib | `cosf()` from newlib / xtensa libm (identical) |
| `M_PI` | Same, or define if missing |
| `int8_t` / `int16_t` / `uint8_t` | `<stdint.h>` (identical) |

---

## 6. Proposed File Structure

```
mouseum-esp32/
├── CMakeLists.txt                 # Root CMake (project declaration)
├── sdkconfig.defaults             # Default config (USB HID + Wi-Fi + HTTP server)
├── main/
│   ├── CMakeLists.txt             # Component-level CMake
│   ├── main.c                     # app_main(): init NVS, Wi-Fi, HTTP server, GPIO, TinyUSB, create tasks
│   ├── usb_hid_task.c/.h          # TinyUSB init, HID task, descriptor config
│   ├── http_server.c/.h           # HTTP server init, URI handlers, JSON parsing/response
│   ├── mouse_engine.c/.h          # MoveMouseHuman(), PRNG, easing, helpers
│   ├── cmd_dispatcher.c/.h        # Transport-agnostic command dispatch (HTTP handlers call this)
│   ├── wifi_manager.c/.h          # Soft-AP or STA setup, event handlers
│   └── board_config.h             # Pin definitions, SSID, queue sizes, HTTP port
├── components/                    # (If needed) third-party libs
│   └── (tinyusb already in ESP-IDF; cJSON is bundled)
├── docs/
│   └── wiring.md                  # Pinout diagram for common dev boards
├── frontend/                      # (Optional) static HTML/JS control panel
│   └── index.html
└── host/
    └── mouseum_http.py            # Python host driver using requests library
```

### 6.1 Module Responsibilities

| File | Responsibility |
|------|----------------|
| `main.c` | `app_main()`: init NVS, start Wi-Fi (AP or STA), start HTTP server, init GPIO, init TinyUSB, create FreeRTOS tasks, start scheduler. |
| `usb_hid_task.c` | `usb_hid_task()`: register TinyUSB callbacks, loop on `tud_task()` + `xQueueReceive(hid_queue)`, send reports via `tud_hid_mouse_report()`. |
| `http_server.c` | `start_http_server()`: register URI handlers (`/api/move`, `/api/click`, etc.), parse JSON/query args, dispatch to `cmd_dispatcher.c`, format HTTP 200/400 responses. |
| `mouse_engine.c` | `move_mouse_human(dx, dy)`, `get_random(min, max)`, `ease_in_out(t)`, `mouse_report_t` definition. |
| `cmd_dispatcher.c` | Receives a structured `Cmd` (kind + args) from any transport. Executes the action by queueing HID reports or calling `move_mouse_human()`. Thread-safe via queue. |
| `wifi_manager.c` | Sets up Soft-AP (`mouseum` SSID) or STA mode. Handles Wi-Fi events (`IP_EVENT_AP_STAIPASSIGNED`, etc.). |

---

## 7. Detailed Design

### 7.1 Data Structures

```c
// mouse_engine.h

typedef struct {
    uint8_t buttons;   // bit0=left, bit1=right, bit2=middle
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} mouse_report_t;

typedef struct {
    uint8_t  report_id; // unused for boot mouse, but kept for TinyUSB compat
    mouse_report_t r;
} hid_queue_item_t;
```

### 7.2 Queue Contract

```c
// main.c
QueueHandle_t hid_queue;

void app_main(void) {
    hid_queue = xQueueCreate(16, sizeof(hid_queue_item_t));
    // ... init USB, Wi-Fi, HTTP, GPIO ...
    xTaskCreatePinnedToCore(usb_hid_task, "usb_hid", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(http_cmd_task, "http_cmd", 8192, NULL, 5, NULL, 1);
}
```

### 7.3 `move_mouse_human()` — Algorithm Port

Keep the algorithm **byte-for-byte identical** to STM32, but replace HAL calls with FreeRTOS APIs and replace direct USB send with queue push:

```c
void move_mouse_human(int16_t target_x, int16_t target_y) {
    int16_t start_x = 0, start_y = 0;
    int16_t control_x = (target_x / 2) + (int16_t)get_random(0, 40) - 20;
    int16_t control_y = (target_y / 2) + (int16_t)get_random(0, 40) - 20;
    float steps = (float)get_random(30, 60);
    float prev_px = 0, prev_py = 0;

    for (float i = 1; i <= steps; i++) {
        float t = i / steps;
        float eased_t = ease_in_out(t);

        float cur_px = (1 - eased_t)*(1 - eased_t)*start_x
                     + 2*(1 - eased_t)*eased_t*control_x
                     + eased_t*eased_t*target_x;
        float cur_py = (1 - eased_t)*(1 - eased_t)*start_y
                     + 2*(1 - eased_t)*eased_t*control_y
                     + eased_t*eased_t*target_y;

        mouse_report_t rep = {
            .buttons = current_buttons,   // from cmd_dispatcher state
            .x = (int8_t)(cur_px - prev_px),
            .y = (int8_t)(cur_py - prev_py),
            .wheel = 0
        };

        if (get_random(0, 10) > 8) {
            rep.x += (int8_t)get_random(0, 2) - 1;
            rep.y += (int8_t)get_random(0, 2) - 1;
        }

        // Push to USB task; if queue full, drop this step (rare)
        hid_queue_item_t item = { .r = rep };
        xQueueSend(hid_queue, &item, pdMS_TO_TICKS(5));

        prev_px = cur_px;
        prev_py = cur_py;

        vTaskDelay(pdMS_TO_TICKS(get_random(8, 14)));
    }
}
```

> **Rationale for queue-drop:** The queue is 16 deep, and the USB stack polls at 1 kHz (1 ms). Steps are 8–14 ms apart, so the queue drains faster than it fills. A 5 ms block wait is sufficient. In the pathological case (host OS suspends USB polling), dropping is better than blocking the command engine indefinitely.

### 7.4 `usb_hid_task()`

```c
void usb_hid_task(void *arg) {
    while (!tud_inited()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Wait for USB enumeration + OS driver binding
    vTaskDelay(pdMS_TO_TICKS(1500));

    hid_queue_item_t item;
    for (;;) {
        // Pump TinyUSB events
        tud_task();

        // Try to get a report without blocking forever
        if (xQueueReceive(hid_queue, &item, pdMS_TO_TICKS(1)) == pdPASS) {
            if (tud_hid_ready()) {
                tud_hid_mouse_report(
                    0,               // report id
                    item.r.buttons,
                    item.r.x,
                    item.r.y,
                    item.r.wheel,
                    0                // horizontal scroll
                );
            }
            // If not ready, report is dropped. In practice TinyUSB
            // buffers one report internally, so this is rare.
        }
    }
}
```

### 7.5 HTTP REST API Design

The server exposes a small REST API. All endpoints return `application/json`.

#### Base URL
`http://192.168.4.1/api/v1/` (default Soft-AP address)

#### Endpoints

| Method | Endpoint | Body / Query | Action | Response |
|--------|----------|--------------|--------|----------|
| `POST` | `/api/v1/move` | JSON `{"dx": int, "dy": int}` | Relative move | `{"status":"ok"}` |
| `POST` | `/api/v1/move_human` | JSON `{"dx": int, "dy": int}` | Human-like Bezier move | `{"status":"ok"}` |
| `POST` | `/api/v1/click` | JSON `{"button":"left"}` or `"right"` / `"middle"` | Click (press + 20 ms + release) | `{"status":"ok"}` |
| `POST` | `/api/v1/buttons/down` | JSON `{"mask": int}` (1=L,2=R,4=M) | Press buttons | `{"status":"ok"}` |
| `POST` | `/api/v1/buttons/up` | JSON `{"mask": int}` | Release buttons | `{"status":"ok"}` |
| `POST` | `/api/v1/buttons/release_all` | *(empty)* | Release all | `{"status":"ok"}` |
| `POST` | `/api/v1/wheel` | JSON `{"delta": int}` | Scroll wheel | `{"status":"ok"}` |
| `POST` | `/api/v1/autowalk/toggle` | *(empty)* | Toggle autowalk demo | `{"status":"ok","autowalk":true}` |
| `GET` | `/api/v1/status` | — | Get current state | `{"buttons":0,"autowalk":false,"usb_ready":true}` |
| `GET` | `/api/v1/help` | — | List commands | JSON array of endpoint descriptions |

#### Example Interaction (curl)

```bash
# Connect laptop to SSID "mouseum", then:

# Move 50 px right, 20 px down
curl -X POST http://192.168.4.1/api/v1/move \
  -H "Content-Type: application/json" \
  -d '{"dx":50,"dy":20}'

# Human-like move to (300, 100)
curl -X POST http://192.168.4.1/api/v1/move_human \
  -d '{"dx":300,"dy":100}'

# Left click
curl -X POST http://192.168.4.1/api/v1/click \
  -d '{"button":"left"}'

# Toggle autowalk
curl -X POST http://192.168.4.1/api/v1/autowalk/toggle

# Check status
curl http://192.168.4.1/api/v1/status
```

#### ESP-IDF Handler Skeleton

```c
static esp_err_t move_handler(httpd_req_t *req) {
    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    cJSON *jdx = cJSON_GetObjectItem(root, "dx");
    cJSON *jdy = cJSON_GetObjectItem(root, "dy");
    if (!cJSON_IsNumber(jdx) || !cJSON_IsNumber(jdy)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dx/dy");
        return ESP_FAIL;
    }

    Cmd cmd = {
        .kind = CMD_MOVE_REL,
        .a = (int16_t)jdx->valueint,
        .b = (int16_t)jdy->valueint
    };
    cmd_execute(&cmd);   // thread-safe, queues to USB task

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
```

> **Note:** `cmd_execute()` must be re-entrant safe because the HTTP server may call it from multiple worker threads (ESP-IDF default: 5 concurrent sockets). Use a mutex around shared state (`current_buttons`, `autowalk_enabled`) or move all state mutations into a single task via a second internal queue.

### 7.6 Button & Autowalk Integration

```c
void http_cmd_task(void *arg) {
    int8_t direction = 1;
    TickType_t last_auto_tick = xTaskGetTickCount();
    uint32_t next_auto_delay = get_random(700, 1500);

    // Start Wi-Fi and HTTP server
    wifi_manager_start();
    start_http_server();

    for (;;) {
        // Button polling (could be a separate task instead)
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            move_mouse_human(300 * direction, 100 * direction);
            direction = -direction;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Autowalk demo (toggled via HTTP POST /api/v1/autowalk/toggle)
        if (autowalk_enabled) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_auto_tick) >= pdMS_TO_TICKS(next_auto_delay)) {
                int16_t dx = (int16_t)get_random(120, 260) * direction;
                int16_t dy = (int16_t)get_random(40, 120) * direction;
                move_mouse_human(dx, dy);
                direction = -direction;
                last_auto_tick = xTaskGetTickCount();
                next_auto_delay = get_random(700, 1500);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // 50 ms poll interval; HTTP server runs in its own LwIP thread
    }
}
```

---

## 8. Pinout Mapping (ESP32-S3 DevKit-C-1 as Reference)

| Function | STM32 (Black Pill) | ESP32-S3 (DevKit-C-1) | Notes |
|----------|-------------------|-----------------------|-------|
| USB D- | PA11 | GPIO 19 (native USB D-) | Fixed pin for USB OTG |
| USB D+ | PA12 | GPIO 20 (native USB D+) | Fixed pin for USB OTG |
| Button | PA0 | GPIO 0 | BOOT button on dev board; already has pull-up. Or use any GPIO + external pull-up. |
| Wi-Fi | N/A | Internal 2.4 GHz radio | No external antenna needed for desk-range use |
| GND | GND | GND | |
| 3V3 | 3V3 | 3V3 | USB-powered |

> **Important:** The ESP32-S3 DevKit-C-1 has a **USB** OTG port (GPIO 19/20, micro-USB or USB-C) and a separate **UART** port (GPIO 43/44, for flashing / `idf.py monitor`). Connect the PC that receives HID input to the **USB OTG** port, not the UART port.

---

## 9. Build System & Configuration

### 9.1 ESP-IDF Project Configuration

Create `sdkconfig.defaults` in project root:

```
# USB HID
CONFIG_USB_ENABLED=y
CONFIG_TINYUSB_ENABLED=y
CONFIG_TINYUSB_HID_ENABLED=y
CONFIG_TINYUSB_DESC_HID_STRING="ESP32 Human interface"
CONFIG_TINYUSB_DESC_CUSTOM_VPID=y
CONFIG_TINYUSB_DESC_USE_DEFAULT_VPID=n
CONFIG_TINYUSB_DESC_VENDOR_ID=0x303A
CONFIG_TINYUSB_DESC_PRODUCT_ID=0x8234
CONFIG_TINYUSB_DESC_MANUFACTURER_STRING="Espressif"
CONFIG_TINYUSB_DESC_PRODUCT_STRING="ESP32 Human interface"
CONFIG_TINYUSB_DESC_SERIAL_STRING=""

# Wi-Fi
CONFIG_ESP_WIFI_ENABLED=y

# HTTP Server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=512
CONFIG_HTTPD_MAX_URI_LEN=512

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_UNICORE=n
```

> **Note:** TinyUSB descriptor strings can also be overridden at runtime in `usb_hid_task_init()` rather than relying solely on menuconfig.

### 9.2 CMakeLists.txt (root)

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mouseum-esp32)
```

### 9.3 main/CMakeLists.txt

```cmake
idf_component_register(SRCS
    "main.c"
    "usb_hid_task.c"
    "http_server.c"
    "mouse_engine.c"
    "cmd_dispatcher.c"
    "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES driver freertos esp_timer esp_wifi nvs_flash esp_http_server json tinyusb)
```

---

## 10. Host Software Compatibility

Control is now via **HTTP REST API** rather than UART serial. Host software must use standard HTTP clients (curl, Python `requests`, JavaScript `fetch`, etc.).

### Python Example (`mouseum_http.py`)

```python
import requests, time

BASE = "http://192.168.4.1/api/v1"

def move(dx, dy):
    requests.post(f"{BASE}/move", json={"dx": dx, "dy": dy}, timeout=5)

def move_human(dx, dy):
    requests.post(f"{BASE}/move_human", json={"dx": dx, "dy": dy}, timeout=30)

def click(button="left"):
    requests.post(f"{BASE}/click", json={"button": button}, timeout=5)

def scroll(delta):
    requests.post(f"{BASE}/wheel", json={"delta": delta}, timeout=5)

def toggle_autowalk():
    r = requests.post(f"{BASE}/autowalk/toggle", timeout=5)
    return r.json().get("autowalk")

if __name__ == "__main__":
    print("status:", requests.get(f"{BASE}/status").json())
    move_human(300, 100)
    click("left")
    toggle_autowalk()
```

### Browser Example (JavaScript)

```javascript
async function move(dx, dy) {
  await fetch('http://192.168.4.1/api/v1/move', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({dx, dy})
  });
}
```

No serial cables, no COM-port enumeration, no baud-rate configuration. Any device with a web browser or HTTP library can control the mouse.

---

## 11. Known Differences & Risk Mitigation

| Risk | STM32 Behavior | ESP32 Behavior | Mitigation |
|------|----------------|----------------|------------|
| USB busy retry | `HAL_Delay(1)` spin-loop | FreeRTOS queue + `tud_hid_ready()` check | Queue absorbs bursts; task never spins. |
| Long moves blocking | Superloop stalls for ~500 ms | `http_cmd_task` blocks, but `usb_hid_task` on other core keeps servicing USB | Dual-core isolation. On single-core variants, increase `tud_task()` call frequency from a timer ISR, or accept slight latency. |
| Control transport latency | UART ~1 ms character time | HTTP ~5–20 ms (Wi-Fi ACK + TCP stack) | Acceptable for human-cadence control; not suitable for real-time gaming. |
| Wi-Fi reliability | N/A (wired) | Soft-AP may have packet loss at range | Keep host within 2 m for Soft-AP; use STA mode on a robust AP for longer range. |
| Concurrent HTTP clients | N/A | ESP-IDF default allows 5 sockets | `cmd_execute()` must be thread-safe (mutex or atomic queue). |
| Button de-bounce | `HAL_Delay(1000)` after press | `vTaskDelay(1000)` | Identical effect, but yields CPU. |
| PRNG seed source | DWT cycle counter | `esp_random()` | Stronger entropy; behavior is still non-deterministic. |
| Power / USB connection | Black Pill via USB OTG | ESP32-S3 DevKit via USB OTG | Use the **USB** port (not UART) for the HID connection. |

---

## 12. Future Enhancements

| Feature | ESP32 Capability | Effort |
|---------|------------------|--------|
| **Web UI** | Serve static HTML/JS from SPIFFS/flash | Low — a simple control pad with directional buttons and sliders. |
| **WebSocket Control** | `esp_http_server` supports WebSocket upgrade | Medium — lower latency than HTTP polling for streaming moves. |
| **Bluetooth HID (BLE)** | ESP32-S3 has BLE 5.0 | Medium — present HID over GATT for wireless mouse *without* USB cable. |
| **mDNS / Zeroconf** | `mdns` component in ESP-IDF | Low — advertise as `mouseum.local` so no IP address needed. |
| **I2C / SPI Slave** | ESP-IDF native drivers | Low — if a wired companion MCU (e.g., Raspberry Pi) is preferred over Wi-Fi. |
| **Secure HTTPS** | mbedTLS built into ESP-IDF | Medium — generate self-signed cert, enable TLS on port 443. |

The **transport-agnostic dispatcher** (`cmd_dispatcher.c`) makes adding WebSocket, I2C, SPI, or BLE trivial: each transport parses into the same `Cmd` struct and calls `Cmd_Execute()`.

---

## 13. Bring-up Checklist

1. [ ] Create ESP-IDF project skeleton (`idf.py create-project`).
2. [ ] Copy `mouse_engine.c` algorithms (Bezier, PRNG, easing). Compile.
3. [ ] Configure TinyUSB HID descriptors. Verify device enumerates on PC as HID mouse.
4. [ ] Bring up Wi-Fi Soft-AP (`SSID: mouseum`). Verify laptop/phone can connect and obtain IP.
5. [ ] Start `esp_http_server`. Verify `GET /api/v1/status` returns JSON over Wi-Fi.
6. [ ] Implement REST endpoints one by one: `/move`, `/move_human`, `/click`, `/wheel`, `/buttons/*`, `/autowalk/toggle`.
7. [ ] Test `POST /api/v1/move_human`. Verify organic motion on screen.
8. [ ] Wire GPIO button. Test manual trigger.
9. [ ] Enable autowalk via HTTP. Test non-blocking pacer.
10. [ ] Stress test: rapid HTTP requests + autowalk + button presses concurrently.
11. [ ] (Optional) Add mDNS (`mouseum.local`) so users don't type raw IP.
12. [ ] (Optional) Add WebSocket or static HTML control panel.

---

## 14. Summary

This design ports the **mouseum** STM32F411 USB HID mouse emulator to ESP32-S3, replacing the wired UART control plane with a **wireless HTTP REST API** served over Wi-Fi:

- The same 4-byte HID mouse report format.
- The same `MoveMouseHuman()` Bezier + easing + jitter algorithm.
- The same button trigger and autowalk demo.
- **Control is now cable-free**: any laptop, phone, or script with Wi-Fi and an HTTP client can drive the mouse.

The architecture uses **FreeRTOS multi-task** to exploit the ESP32-S3's dual-core nature, isolating USB timing from the HTTP server and long blocking operations. Host software is simpler: no serial ports, no drivers — just `curl` or `requests`.
