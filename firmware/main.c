/*
 * App 5 — dual-core IPC pipeline
 * Theme: Space — AOCS sample -> attitude update -> downlink packet
 *
 * Scaffold provided the task skeletons, event-group/notification wiring,
 * button ISR, heartbeats, and the serial monitor (scaffold notes its own
 * AI-assisted portions). My parts: producer/consumer bodies, queue sizing,
 * back-pressure policy, theme, and the web monitor (ported from my App 1).
 * See README for the full breakdown and citations.
 *
 * USE_WEBSERVER: 0 = serial monitor (default, no Wi-Fi, runs in Wokwi),
 *                1 = web monitor on Core 0.
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 0
#endif

/* CAPSTONE ADDITIONS ------------------------------------------------------
 * MEASURE_WCET  1 -> wrap each task body in App 3's WCET-max macro and report
 *                    per-task worst-case execution times on the monitor line
 *                    (evidence for the capstone task table).
 * FAULT_INJECT  1 -> every 15 s the attitude_update task stalls for 600 ms,
 *                    simulating a wedged processing stage. The queue absorbs
 *                    ~8 samples of burst, then the producer's log+drop
 *                    back-pressure policy fires — the graceful-degradation
 *                    path for the demo video. System recovers on its own. */
#ifndef MEASURE_WCET_EN
#define MEASURE_WCET_EN 1
#endif
#ifndef FAULT_INJECT
#define FAULT_INJECT 0
#endif

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "app5-space";

/* One AOCS gyro sample. Small fixed struct so the queue copies by value. */
typedef struct {
    uint32_t timestamp_ms;
    int value;               /* simulated body rate, mdeg/s, 0..99 */
} aocs_sample_t;

/* IPC objects, created in app_main.
 * Queue depth: producer runs at 20 Hz, and I budget 400 ms for the worst
 * consumer stall (higher-prio tasks + log bursts). 20 * 0.4 = 8 items.
 * Full math in the README. */
static QueueHandle_t      data_q;
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;

#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Heartbeats for the monitor. 32-bit reads are atomic on Xtensa, so the
 * monitor can read these without a lock. */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;

/* Last processed sample, shown by the monitor (same atomicity argument). */
static volatile uint32_t last_ts_ms;
static volatile int      last_value;

/* Drop counter — makes the back-pressure policy visible on the monitors. */
static volatile uint32_t drop_count;

#if MEASURE_WCET_EN
/* App 3's WCET-max helper, reused for the capstone task table.
 * Variadic: commas inside the body (struct initializers, function args)
 * would otherwise be split into extra macro arguments by the preprocessor. */
static volatile uint64_t wcet_prod_us, wcet_cons_us, wcet_coord_us, wcet_resp_us;
#define MEASURE_WCET(_max_var, ...) do {                         \
    int64_t _t0 = esp_timer_get_time();                          \
    { __VA_ARGS__ }                                              \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
} while (0)
#else
#define MEASURE_WCET(_max_var, ...) do { __VA_ARGS__ } while (0)
#endif

/* Producer: fake AOCS gyro read at 20 Hz, push into the queue. */
static void producer_task(void *arg)
{
    int tick = 0;
    for (;;) {
        MEASURE_WCET(wcet_prod_us, {
        aocs_sample_t s = {
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .value        = (tick * 7) % 100,
        };

        /* Back-pressure: try for 10 ms, then drop this sample and log it.
         * The sampler keeps its 20 Hz cadence no matter what's downstream. */
        if (xQueueSend(data_q, &s, pdMS_TO_TICKS(10)) != pdTRUE) {
            drop_count++;
            ESP_LOGW(TAG, "[aocs_sensor] queue full — sample dropped (drops=%lu)",
                     (unsigned long)drop_count);
        }

        xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);
        });

        tick++;
        hb_prod++;
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz */
    }
}

/* Consumer: pop a sample, run the attitude update, flag rates over deadband.
 * xQueueReceive blocks for us, so no extra delay needed here. */
static void consumer_task(void *arg)
{
    aocs_sample_t s;
    for (;;) {
        /* 500 ms is 10 producer periods — if we time out, the sensor task
         * is genuinely stuck, not just slow. */
        if (xQueueReceive(data_q, &s, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "[attitude_update] no AOCS sample in 500 ms");
            continue;
        }

#if FAULT_INJECT
        /* Fault injection for the degradation demo: every 15 s, stall the
         * attitude stage for 600 ms (> the 400 ms queue budget), so the
         * queue fills and the producer's drop policy visibly fires. */
        {
            static uint32_t fi_cycles = 0;
            if (++fi_cycles % 300 == 0) {   /* 300 cycles ~ 15 s at 20 Hz */
                ESP_LOGW(TAG, "[FAULT] attitude_update stalling 600 ms (injected)");
                vTaskDelay(pdMS_TO_TICKS(600));
                ESP_LOGW(TAG, "[FAULT] attitude_update recovered");
            }
        }
#endif

        MEASURE_WCET(wcet_cons_us, {
        if (s.value > 85) {
            ESP_LOGI(TAG, "[attitude_update] rate high: %d mdeg/s @ %lu ms",
                     s.value, (unsigned long)s.timestamp_ms);
        }
        last_ts_ms = s.timestamp_ms;
        last_value = s.value;
        });

        xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
        hb_cons++;
    }
}

/* Coordinator: rendezvous on both bits, then wake the downlink task. */
static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
            /* Sample taken AND processed = one full cycle -> downlink it. */
            MEASURE_WCET(wcet_coord_us, {
            xTaskNotifyGive(responder_handle);
            });
            hb_coord++;
        }
    }
}

/* Responder: emits a downlink packet when notified, either by the
 * coordinator (normal cycle) or by the button ISR (ground command). */
static void responder_task(void *arg)
{
    uint32_t pkts = 0;
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        pkts += n;
        MEASURE_WCET(wcet_resp_us, {
        /* Log every 20th packet (~1/sec at 20 Hz). Printing all of them
         * floods the serial port and visibly slows the Wokwi sim. */
        if (pkts % 20 < n) {
            ESP_LOGI(TAG, "[downlink] packet #%lu: rate=%d mdeg/s @ %lu ms",
                     (unsigned long)pkts, last_value, (unsigned long)last_ts_ms);
        }
        });
        hb_resp++;
    }
}

/* Button ISR (GPIO18) — treated as a ground-station "send telemetry now"
 * command. Notifies the downlink task directly, with a crude debounce. */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    /* 200 us gate suppresses retrigger; works here because the Wokwi button
     * has bounce disabled in diagram.json. On real hardware this should be
     * raised to the ms scale — see README (measurement pitfall). */
    if (now - last_edge_us < 200) return;
    last_edge_us = now;

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(responder_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

#if USE_WEBSERVER
/* Web monitor, ported from my App 1 (wifi_init_sta / wifi_event_handler /
 * start_webserver, plus the HTML-shell + /state JSON polling page).
 * Only real changes: App 5's fields, and the poll slowed from 4 Hz to 1 Hz
 * so the HTTP handler stays out of the latency measurements. */

#define HTTP_PORT  80
#define WIFI_SSID  "Wokwi-GUEST"
#define WIFI_PASS  ""             /* Wokwi virtual AP is open */

/* /state — live pipeline state as JSON, polled by the page. */
static esp_err_t handle_state(httpd_req_t *req)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
        "{\"depth\":%u,\"last_value\":%d,\"last_ts\":%lu,\"evt\":%u,"
        "\"hb_prod\":%lu,\"hb_cons\":%lu,\"hb_coord\":%lu,\"hb_resp\":%lu}",
        (unsigned)uxQueueMessagesWaiting(data_q),
        last_value, (unsigned long)last_ts_ms,
        (unsigned)xEventGroupGetBits(evt_group),
        (unsigned long)hb_prod, (unsigned long)hb_cons,
        (unsigned long)hb_coord, (unsigned long)hb_resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* Root page — HTML served once, JS polls /state and updates in place. */
static esp_err_t handle_root(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<title>SAT-1 AOCS pipeline monitor</title>"
        "<style>"
        "  body { font-family: -apple-system, sans-serif; background: #FAFAF5; "
        "         color: #1A1A1A; padding: 2rem; }"
        "  h1 { color: #6B4F09; border-bottom: 3px solid #FFC904; "
        "       display: inline-block; padding-bottom: 4px; }"
        "  .meta { color: #6B4F09; font-variant-numeric: tabular-nums; }"
        "  td { padding: 2px 12px; font-variant-numeric: tabular-nums; }"
        "</style></head>"
        "<body>"
        "<h1>SAT-1 AOCS Pipeline Telemetry</h1>"
        "<table>"
        "<tr><td>Queue depth</td><td id=\"depth\">--</td></tr>"
        "<tr><td>Last sample</td><td><span id=\"lv\">--</span> mdeg/s @ "
        "<span id=\"lt\">--</span> ms</td></tr>"
        "<tr><td>Event bits</td><td id=\"evt\">--</td></tr>"
        "<tr><td>hb aocs_sensor</td><td id=\"hp\">--</td></tr>"
        "<tr><td>hb attitude_update</td><td id=\"hc\">--</td></tr>"
        "<tr><td>hb coordinator</td><td id=\"hk\">--</td></tr>"
        "<tr><td>hb downlink</td><td id=\"hr\">--</td></tr>"
        "</table>"
        "<p class=\"meta\">Polling at 1 Hz via <code>/state</code> JSON endpoint.</p>"
        "<script>"
        "async function poll(){"
        "  try{"
        "    const r = await fetch('/state',{cache:'no-store'});"
        "    const s = await r.json();"
        "    document.getElementById('depth').textContent = s.depth;"
        "    document.getElementById('lv').textContent = s.last_value;"
        "    document.getElementById('lt').textContent = s.last_ts;"
        "    document.getElementById('evt').textContent = '0x' + s.evt.toString(16).padStart(2,'0');"
        "    document.getElementById('hp').textContent = s.hb_prod;"
        "    document.getElementById('hc').textContent = s.hb_cons;"
        "    document.getElementById('hk').textContent = s.hb_coord;"
        "    document.getElementById('hr').textContent = s.hb_resp;"
        "  }catch(e){/* ignore transient network blips */}"
        "}"
        "setInterval(poll, 1000);"
        "poll();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.core_id = 0;                    /* networking stays on Core 0 */
    cfg.task_priority = 5;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_root,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t state = {
            .uri = "/state",
            .method = HTTP_GET,
            .handler = handle_state,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &state);

        ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
    return server;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void webmonitor_task(void *arg)
{
    wifi_init_sta();   /* server starts from the GOT_IP event, same as App 1 */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#else
/* Serial monitor (scaffold, unchanged) — same fields the web page shows. */
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG,
                 "[monitor] q_depth=%u  evt=0x%02x  drops=%lu  hb: prod=%lu cons=%lu coord=%lu resp=%lu"
#if MEASURE_WCET_EN
                 "  wcet_us: prod=%llu cons=%llu coord=%llu resp=%llu"
#endif
                 ,
                 (unsigned)depth, (unsigned)bits, (unsigned long)drop_count,
                 (unsigned long)hb_prod, (unsigned long)hb_cons,
                 (unsigned long)hb_coord, (unsigned long)hb_resp
#if MEASURE_WCET_EN
                 , (unsigned long long)wcet_prod_us, (unsigned long long)wcet_cons_us,
                 (unsigned long long)wcet_coord_us, (unsigned long long)wcet_resp_us
#endif
                 );
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* USE_WEBSERVER */

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App 5 [SPACE] starting — AOCS IPC pipeline ====");

#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1)");
#else
    ESP_LOGI(TAG, "Monitor: SERIAL (USE_WEBSERVER=0), no Wi-Fi");
#endif

    /* 8 deep x 8 bytes — 20 Hz producer x 400 ms stall budget, see README. */
    data_q = xQueueCreate(8, sizeof(aocs_sample_t));

    evt_group = xEventGroupCreate();

    /* Pipeline on Core 1. 4096-byte stacks because ESP_LOGI's vprintf
     * formatting overflows 2048.
     * Responder goes FIRST: these tasks start running on Core 1 while
     * app_main is still executing on Core 0, so responder_handle has to be
     * valid before the coordinator can wake up and notify it. (Creating it
     * last caused an xTaskGenericNotify assert on a NULL handle.) */
    xTaskCreatePinnedToCore(responder_task,   "downlink",     4096, NULL, 12, &responder_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(producer_task,    "aocs_sensor",  4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(consumer_task,    "attitude_upd", 4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(coordinator_task, "coordinator",  4096, NULL,  9, NULL, APP_CPU_NUM);

    /* Monitor on Core 0, next to the network stack. */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,    "webmon",  4096, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "monitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}