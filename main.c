/*
 * FINAL CAPSTONE: Digital VLSI Circuit Testing Pipeline
 * 
 * CORE 1 (Real-Time Plane):
 *  - vlsi_probe_task (Prod): Samples wafer test points, sends to Queue.
 *  - signal_eval_task (Cons): Evaluates signal integrity, sets Event Group.
 *  - yield_coord_task: Rendezvous point. Uses Mutex to update shared stats.
 *  - estop_responder_task: Highest priority. Halts line via Notification.
 *  - background_log_task: Low-priority compute load to demonstrate preemption.
 * 
 * CORE 0 (Observability Plane):
 *  - webmonitor_task: Wi-Fi HTTP server serving a live diagnostic dashboard.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

/* ---------- Configuration ---------- */
#define WIFI_SSID           "Wokwi-GUEST"
#define WIFI_PASS           ""
#define BUTTON_GPIO         GPIO_NUM_18
#define ALARM_LED_GPIO      GPIO_NUM_2
#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1

static const char *TAG = "VLSI_CAPSTONE";

/* ---------- Data Structures ---------- */
typedef struct {
    uint32_t timestamp_ms;
    int16_t voltage_mv;
    uint16_t propagation_delay_ps;
    bool timing_violation;
} vlsi_packet_t;

/* ---------- IPC Objects ---------- */
static QueueHandle_t      probe_queue;         
static EventGroupHandle_t sync_group;
static SemaphoreHandle_t  stats_mutex;
static TaskHandle_t       estop_responder_handle;

#define EV_BIT_PROBED    (1 << 0)
#define EV_BIT_EVALUATED (1 << 1)

/* ---------- Shared State & Telemetry ---------- */
static volatile uint32_t hb_probe = 0, hb_eval = 0, hb_coord = 0, hb_log = 0;
static uint64_t wcet_log_max_us = 0;
static vlsi_packet_t last_packet = {0};

/* Mutex-protected stats */
static uint32_t total_wafers_tested = 0;
static uint32_t total_defects_found = 0;
static bool system_halted = false;

/* WCET Macro */
#define MEASURE_WCET(_max_var, _body) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    _body;                                                       \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
} while (0)

/* ---------- Hardware ISR (E-STOP) ---------- */
static volatile int64_t last_edge_us = 0;
static void IRAM_ATTR button_isr(void *arg) {
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200000) return; /* 200ms debounce */
    last_edge_us = now;
    
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(estop_responder_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------- CORE 1: Real-Time Tasks ---------- */

/* 1. Producer: Probes physical VLSI circuit pathways (20ms Period) */
static void vlsi_probe_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);
    uint32_t sample_idx = 0;

    for (;;) {
        if (!system_halted) {
            vlsi_packet_t pkt;
            pkt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            
            /* Simulate hardware probe logic */
            if (sample_idx % 45 == 0) {
                pkt.voltage_mv = 850; 
                pkt.propagation_delay_ps = 145; // Setup time violation
            } else {
                pkt.voltage_mv = 1200; // Nominal 1.2V core
                pkt.propagation_delay_ps = 45; // Nominal delay
            }
            pkt.timing_violation = false;

            if (xQueueSend(probe_queue, &pkt, pdMS_TO_TICKS(5)) == pdPASS) {
                xEventGroupSetBits(sync_group, EV_BIT_PROBED);
            } else {
                ESP_LOGW(TAG, "[Probe] Probe queue full! Skipping test vector.");
            }
            sample_idx++;
        }
        hb_probe++;
        vTaskDelayUntil(&last, period);
    }
}

/* 2. Consumer: Evaluates propagation delays and signal integrity */
static void signal_eval_task(void *arg) {
    vlsi_packet_t pkt;
    for (;;) {
        if (xQueueReceive(probe_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            /* Timing / Voltage Validation Logic */
            if (pkt.voltage_mv < 900 || pkt.propagation_delay_ps > 100) {
                pkt.timing_violation = true;
                ESP_LOGW(TAG, "[Eval] TIMING VIOLATION @ %lu ms! VDD: %d mV, Delay: %u ps",
                         pkt.timestamp_ms, pkt.voltage_mv, pkt.propagation_delay_ps);
            }
            last_packet = pkt;
            xEventGroupSetBits(sync_group, EV_BIT_EVALUATED);
            hb_eval++;
        }
    }
}

/* 3. Coordinator: N-Way Rendezvous & Mutex protected state updates */
static void yield_coord_task(void *arg) {
    const EventBits_t wait_mask = EV_BIT_PROBED | EV_BIT_EVALUATED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(sync_group, wait_mask, pdTRUE, pdTRUE, portMAX_DELAY);
        
        if ((got & wait_mask) == wait_mask) {
            /* Protect global yield statistics with Mutex */
            if (xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                total_wafers_tested++;
                if (last_packet.timing_violation) {
                    total_defects_found++;
                    xTaskNotifyGive(estop_responder_handle); // Notify responder of defect
                }
                xSemaphoreGive(stats_mutex);
            }
            hb_coord++;
        }
    }
}

/* 4. Responder: Wakes instantly via ISR or Coordinator (Highest Priority) */
static void estop_responder_task(void *arg) {
    gpio_reset_pin(ALARM_LED_GPIO);
    gpio_set_direction(ALARM_LED_GPIO, GPIO_MODE_OUTPUT);

    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n > 0) {
            system_halted = true;
            gpio_set_level(ALARM_LED_GPIO, 1);
            ESP_LOGE(TAG, "[RESPONDER] LINE HALTED! Defect binned or E-STOP pressed.");
            vTaskDelay(pdMS_TO_TICKS(2000)); // Hold alarm state
            system_halted = false;
            gpio_set_level(ALARM_LED_GPIO, 0);
        }
    }
}

/* 5. Background task: Mathematical modeling (Low Priority) */
#define LOG_TERMS 15000
static volatile float log_sink;
static void background_log_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(100);
    for (;;) {
        MEASURE_WCET(wcet_log_max_us, {
            float pi = log_sink * 0.0f;
            float sign = 1.0f;
            for (int k = 0; k < LOG_TERMS; k++) {
                pi += sign / (float)(2 * k + 1);
                sign = -sign;
            }
            log_sink = pi * 4.0f;
        });
        hb_log++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---------- CORE 0: Observability Plane ---------- */
static esp_err_t http_root_handler(httpd_req_t *req) {
    char resp[1024];
    UBaseType_t depth = uxQueueMessagesWaiting(probe_queue);
    
    /* Safely read stats */
    uint32_t tested = 0, defects = 0;
    if (xSemaphoreTake(stats_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        tested = total_wafers_tested;
        defects = total_defects_found;
        xSemaphoreGive(stats_mutex);
    }

    snprintf(resp, sizeof(resp),
        "<html><head><meta http-equiv='refresh' content='1'></head>"
        "<body style='font-family: sans-serif; background: #f4f4f9; padding: 20px;'>"
        "<h2 style='color: #2c3e50; border-bottom: 2px solid #3498db;'>Digital VLSI Diagnostics</h2>"
        "<h3>Real-Time Hardware Telemetry</h3>"
        "<p><b>System Status:</b> %s</p>"
        "<p><b>Test Queue Depth:</b> %u / 16</p>"
        "<p><b>Latest VDD:</b> %d mV | <b>Delay:</b> %u ps | <b>Violation:</b> %s</p>"
        "<h3>Manufacturing Yield</h3>"
        "<p><b>Circuits Tested:</b> %lu</p>"
        "<p><b>Defects Binned:</b> %lu</p>"
        "<h3>Task Heartbeats (Core 1)</h3>"
        "<ul>"
        "<li>Wafer Probe: %lu</li>"
        "<li>Signal Eval: %lu</li>"
        "<li>Yield Coord: %lu</li>"
        "<li>Background Log: %lu (WCET: %llu &micro;s)</li>"
        "</ul></body></html>",
        system_halted ? "<span style='color:red; font-weight:bold;'>HALTED</span>" : "<span style='color:green;'>NOMINAL</span>",
        (unsigned)depth, 
        last_packet.voltage_mv, last_packet.propagation_delay_ps,
        last_packet.timing_violation ? "<font color='red'>YES</font>" : "NO",
        (unsigned long)tested, (unsigned long)defects,
        (unsigned long)hb_probe, (unsigned long)hb_eval, 
        (unsigned long)hb_coord, (unsigned long)hb_log, (unsigned long long)wcet_log_max_us);

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void webmonitor_task(void *arg) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
    
    vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for connection

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri = "/", .method = HTTP_GET,
            .handler = http_root_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);
        ESP_LOGI(TAG, "VLSI Dashboard started on Core 0");
    }
    vTaskDelete(NULL);
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== VLSI Capstone Starting ====");

    /* Initialize Primitives */
    probe_queue = xQueueCreate(16, sizeof(vlsi_packet_t));
    sync_group  = xEventGroupCreate();
    stats_mutex = xSemaphoreCreateMutex();

    /* Core 1 Real-Time Plane */
    xTaskCreatePinnedToCore(estop_responder_task, "estop_task", 4096, NULL, 16, &estop_responder_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(vlsi_probe_task,      "probe_task", 4096, NULL, 15, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(signal_eval_task,     "eval_task",  4096, NULL, 10, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(yield_coord_task,     "coord_task", 4096, NULL, 8,  NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(background_log_task,  "log_task",   4096, NULL, 2,  NULL, APP_CPU_NUM);

    /* Core 0 Observability Plane */
    xTaskCreatePinnedToCore(webmonitor_task, "web_task", 8192, NULL, 5, NULL, PRO_CPU_NUM);

    /* Configure E-STOP Button */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}
