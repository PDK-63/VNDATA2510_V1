#include "net_manager.h"

#include "app_config.h"
#include "app_events.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "provision_service.h"
#include "runtime_config.h"
#include "tm1638_wifi_ui.h"
#include "wifi_service.h"

#include <string.h>

static const char *TAG = "net_manager";

#ifndef APP_NET_MANAGER_TASK_STACK
#define APP_NET_MANAGER_TASK_STACK 4096
#endif

#ifndef APP_NET_MANAGER_TASK_PRIO
#define APP_NET_MANAGER_TASK_PRIO 8
#endif

#ifndef APP_WIFI_SETTLE_BEFORE_MQTT_MS
#define APP_WIFI_SETTLE_BEFORE_MQTT_MS 3000
#endif

#ifndef APP_WIFI_RETRY_DELAY_MS
#define APP_WIFI_RETRY_DELAY_MS 5000
#endif

typedef enum {
    NETM_MODE_IDLE = 0,
    NETM_MODE_PROVISION,
    NETM_MODE_WIFI_CONNECTING,
    NETM_MODE_WIFI_ONLINE,
} netm_mode_t;

static TaskHandle_t s_task;
static TaskHandle_t s_provision_task;
static bool s_started;
static bool s_net_ready;
static bool s_mqtt_ready;
static bool s_mqtt_start_pending;
static bool s_wifi_config_ready;
static TickType_t s_wifi_retry_due_tick;
static TickType_t s_wifi_settle_deadline_tick;
static runtime_config_t s_runtime_cfg;
static netm_mode_t s_mode = NETM_MODE_IDLE;
static app_net_type_t s_active_uplink = APP_NET_NONE;

static const char *mode_name(netm_mode_t mode)
{
    switch (mode) {
    case NETM_MODE_IDLE: return "IDLE";
    case NETM_MODE_PROVISION: return "PROVISION";
    case NETM_MODE_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case NETM_MODE_WIFI_ONLINE: return "WIFI_ONLINE";
    default: return "UNKNOWN";
    }
}

static void transition_mode(netm_mode_t new_mode, const char *reason)
{
    if (s_mode == new_mode) {
        return;
    }
    ESP_LOGI(TAG, "state %s -> %s (%s)",
             mode_name(s_mode), mode_name(new_mode), reason ? reason : "-");
    s_mode = new_mode;
}

static bool load_runtime_cfg(void)
{
    memset(&s_runtime_cfg, 0, sizeof(s_runtime_cfg));
    if (runtime_config_load(&s_runtime_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "runtime config load failed");
        return false;
    }

    if (s_runtime_cfg.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi ssid empty");
        return false;
    }

    return true;
}

static void reset_wifi_runtime_state(void)
{
    s_net_ready = false;
    s_mqtt_ready = false;
    s_mqtt_start_pending = false;
    s_wifi_retry_due_tick = 0;
    s_wifi_settle_deadline_tick = 0;
    s_active_uplink = APP_NET_NONE;
}

static void stop_online_services(void)
{
    s_mqtt_start_pending = false;
    s_mqtt_ready = false;
    mqtt_service_stop();
    wifi_service_stop_sta();
}

static void enter_provision_mode_now(const char *reason)
{
    ESP_LOGW(TAG, "enter provision mode (%s)", reason ? reason : "-");
    tm1638_wifi_set_state(WIFI_LED_CONFIG_MODE);

    stop_online_services();
    wifi_service_set_reconnect_enabled(false);
    provision_service_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    if (provision_service_start() == ESP_OK) {
        s_active_uplink = APP_NET_AP_ONLY;
        transition_mode(NETM_MODE_PROVISION, reason ? reason : "provision");
    } else {
        s_active_uplink = APP_NET_NONE;
        transition_mode(NETM_MODE_IDLE, "provision_start_failed");
    }
}

static void enter_provision_task(void *arg)
{
    const char *reason = (const char *)arg;
    enter_provision_mode_now(reason ? reason : "button_request");
    s_provision_task = NULL;
    vTaskDelete(NULL);
}

static void request_enter_provision_mode(const char *reason)
{
    if (s_provision_task != NULL) {
        ESP_LOGW(TAG, "provision task already running, ignore (%s)", reason ? reason : "-");
        return;
    }

    BaseType_t ok = xTaskCreate(enter_provision_task,
                                "enter_prov",
                                4096,
                                (void *)reason,
                                (tskIDLE_PRIORITY + 2),
                                &s_provision_task);
    if (ok != pdPASS) {
        s_provision_task = NULL;
        ESP_LOGE(TAG, "failed to create provision task");
    }
}

static void start_wifi_connect(void)
{
    if (!s_wifi_config_ready) {
        request_enter_provision_mode("wifi_config_missing");
        return;
    }

    provision_service_stop();
    wifi_service_set_reconnect_enabled(true);
    tm1638_wifi_set_state(WIFI_LED_SCANNING);
    reset_wifi_runtime_state();

    ESP_LOGI(TAG, "starting Wi-Fi STA for ssid=%s", s_runtime_cfg.wifi_ssid);
    esp_err_t err = wifi_service_start_sta(s_runtime_cfg.wifi_ssid, s_runtime_cfg.wifi_pass);
    if (err == ESP_OK) {
        transition_mode(NETM_MODE_WIFI_CONNECTING, "start_wifi_connect");
    } else {
        ESP_LOGE(TAG, "wifi_service_start_sta failed: %s", esp_err_to_name(err));
        s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
    }
}

static void maybe_start_mqtt(void)
{
    if (!s_net_ready || s_active_uplink != APP_NET_WIFI) {
        return;
    }
    if (s_mqtt_ready || mqtt_service_is_connected() || mqtt_service_is_started()) {
        return;
    }
    if (s_wifi_settle_deadline_tick != 0 && xTaskGetTickCount() < s_wifi_settle_deadline_tick) {
        return;
    }

    ESP_LOGI(TAG, "starting mqtt over Wi-Fi");
    if (mqtt_service_start() == ESP_OK) {
        s_mqtt_start_pending = true;
    } else {
        s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
    }
}

static void handle_timers(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_wifi_retry_due_tick != 0 && now >= s_wifi_retry_due_tick) {
        s_wifi_retry_due_tick = 0;
        if (!wifi_service_is_connected()) {
            ESP_LOGW(TAG, "wifi retry timer expired -> reconnect");
            start_wifi_connect();
        }
    }

    if (s_wifi_settle_deadline_tick != 0 && now >= s_wifi_settle_deadline_tick) {
        s_wifi_settle_deadline_tick = 0;
        maybe_start_mqtt();
    }
}

static void app_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;

    switch (event_id) {
    case APP_EVENT_NET_UP: {
        app_net_status_t *st = (app_net_status_t *)data;
        if (!st || st->type != APP_NET_WIFI) {
            break;
        }

        ESP_LOGI(TAG, "Wi-Fi uplink online");
        tm1638_wifi_set_state(WIFI_LED_READY);
        s_net_ready = true;
        s_active_uplink = APP_NET_WIFI;
        s_wifi_retry_due_tick = 0;
        s_wifi_settle_deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_SETTLE_BEFORE_MQTT_MS);
        transition_mode(NETM_MODE_WIFI_ONLINE, "wifi_net_up");
        break;
    }

    case APP_EVENT_NET_DOWN: {
        app_net_status_t *st = (app_net_status_t *)data;
        if (!st || st->type != APP_NET_WIFI) {
            break;
        }

        ESP_LOGW(TAG, "Wi-Fi uplink down");

        s_net_ready = false;
        s_mqtt_ready = false;
        s_mqtt_start_pending = false;
        s_wifi_settle_deadline_tick = 0;
        mqtt_service_stop();

        /* Neu dang o che do provision/AP thi giu LED CONFIG_MODE,
        khong ep ve OFF khi STA bi stop/disconnect */
        if (s_mode == NETM_MODE_PROVISION || s_active_uplink == APP_NET_AP_ONLY || provision_service_is_running()) {
            ESP_LOGI(TAG, "ignore NET_DOWN LED_OFF because provision mode is active");
            tm1638_wifi_set_state(WIFI_LED_CONFIG_MODE);
            s_active_uplink = APP_NET_AP_ONLY;
            transition_mode(NETM_MODE_PROVISION, "wifi_net_down_in_provision");
            s_wifi_retry_due_tick = 0;
        } else {
            tm1638_wifi_set_state(WIFI_LED_OFF);
            s_active_uplink = APP_NET_NONE;
            transition_mode(NETM_MODE_WIFI_CONNECTING, "wifi_net_down");
            s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        }
        break;
    }

    case APP_EVENT_MQTT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_ready = true;
        s_mqtt_start_pending = false;
        break;

    case APP_EVENT_MQTT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_ready = false;
        s_mqtt_start_pending = false;
        if (s_net_ready && s_active_uplink == APP_NET_WIFI) {
            s_wifi_retry_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS);
        }
        break;

    case APP_EVENT_WIFI_CONFIG_SAVED:
        ESP_LOGI(TAG, "Wi-Fi config saved -> reload and reconnect");
        s_wifi_config_ready = load_runtime_cfg();
        start_wifi_connect();
        break;

    case APP_EVENT_PROVISION_START:
        request_enter_provision_mode("button_request");
        break;

    default:
        break;
    }
}

static void net_manager_task(void *arg)
{
    (void)arg;

    for (;;) {
        handle_timers();

        if (s_mode == NETM_MODE_WIFI_ONLINE || s_mode == NETM_MODE_WIFI_CONNECTING) {
            maybe_start_mqtt();
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t net_manager_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_wifi_config_ready = load_runtime_cfg();
    reset_wifi_runtime_state();
    transition_mode(NETM_MODE_IDLE, "init");

    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID, app_event_handler, NULL));
    return ESP_OK;
}

esp_err_t net_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_started = true;

    if (!s_wifi_config_ready) {
        request_enter_provision_mode("wifi_config_missing");
    } else {
        start_wifi_connect();
    }

    BaseType_t ok = xTaskCreate(net_manager_task,
                                "net_manager",
                                APP_NET_MANAGER_TASK_STACK,
                                NULL,
                                APP_NET_MANAGER_TASK_PRIO,
                                &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
