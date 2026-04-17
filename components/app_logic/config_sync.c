#include "config_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "app_logic.h"
#include "mqtt_service.h"
#include "runtime_config.h"
#include "esp_event.h"
#include "app_events.h"

static const char *TAG = "config_sync";
static bool s_wifi_reconnect_pending = false;

#ifndef APP_HW_VERSION
#define APP_HW_VERSION "1.0.0"
#endif

#ifndef APP_FW_VERSION_STR
#define APP_FW_VERSION_STR "0.9.0.5"
#endif

static bool json_get_string(cJSON *root, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    snprintf(out, out_len, "%s", item->valuestring);
    return true;
}

static bool json_get_float(cJSON *root, const char *key, float *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = (float)item->valuedouble;
    return true;
}

static bool json_get_int(cJSON *root, const char *key, int *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out = item->valueint;
    return true;
}

static bool json_get_bool(cJSON *root, const char *key, bool *out)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }

    *out = cJSON_IsTrue(item);
    return true;
}

static void publish_error_result(const char *request_id, const char *cmd_name, const char *msg)
{
    char body[320];

    snprintf(body, sizeof(body),
             "\"cmd\":\"%s\",\"ok\":false,\"msg\":\"%s\"",
             cmd_name ? cmd_name : "",
             msg ? msg : "");

    mqtt_service_publish_reply(request_id ? request_id : "", body);
}

// static void publish_full_config_response(const char *request_id, const runtime_config_t *cfg)
// {
//     static char body[2048];

//     int n = snprintf(body, sizeof(body),
//                      "\"cmd\":\"get config\","
//                      "\"version\":\"%s\","
//                      "\"hw_version\":\"%s\","
//                      "\"uuid\":\"%s\","
//                      "\"wifiuser\":\"%s\","
//                      "\"wifipass\":\"%s\","
//                      "\"ip\":\"%s\","
//                      "\"user\":\"%s\","
//                      "\"pass\":\"%s\","
//                      "\"sms1\":\"%s\","
//                      "\"sms2\":\"%s\","
//                      "\"sms3\":\"%s\","
//                      "\"sms4\":\"%s\","
//                      "\"sms5\":\"%s\","
//                      "\"message\":\"%s\","
//                      "\"phone1\":\"%s\","
//                      "\"phone2\":\"%s\","
//                      "\"phone3\":\"%s\","
//                      "\"phone4\":\"%s\","
//                      "\"phone5\":\"%s\","
//                      "\"ahi\":%.2f,"
//                      "\"alo\":%.2f,"
//                      "\"temp_sht_hi\":100,"
//                      "\"temp_sht_lo\":0,"
//                      "\"hum_sht_hi\":100,"
//                      "\"hum_sht_lo\":0,"
//                      "\"calib_ntc_pcb\":%.2f,"
//                      "\"calib_temp_sht\":%.2f,"
//                      "\"calib_hum_sht\":%.2f,"
//                      "\"time_delay\":%d,"
//                      "\"gps\":%s,"
//                      "\"temp_sht_enable\":false,"
//                      "\"hum_sht_enable\":false,"
//                      "\"ota_auto_enable\":false,"
//                      "\"ota_manual_enable\":false",
//                      APP_FW_VERSION_STR,
//                      APP_HW_VERSION,
//                      mqtt_service_get_device_key(),
//                      cfg->wifi_ssid,
//                      cfg->wifi_pass,
//                      cfg->broker_uri,
//                      cfg->mqtt_username,
//                      cfg->mqtt_password,
//                      cfg->alarm_number1,
//                      cfg->alarm_number2,
//                      cfg->alarm_number3,
//                      cfg->alarm_number4,
//                      cfg->alarm_number5,
//                      cfg->message,
//                      cfg->alarm_number6,
//                      "",
//                      "",
//                      "",
//                      "",
//                      cfg->ntc_high_limit_c,
//                      cfg->ntc_low_limit_c,
//                      cfg->ntc_calib_c,
//                      0.0f,
//                      cfg->hum_calib_pct,
//                      cfg->telemetry_interval_ms,
//                      cfg->gps_enabled ? "true" : "false");

//     if (n < 0 || n >= (int)sizeof(body)) {
//         ESP_LOGE(TAG, "full config response too long");
//         publish_error_result(request_id, "get config", "response_too_long");
//         return;
//     }

//     mqtt_service_publish_reply(request_id ? request_id : "", body);
// }
static void publish_get_config_response(const char *request_id, const runtime_config_t *cfg)
{
    static char body[1024];

    int n = snprintf(body, sizeof(body),
                     "\"version\":\"1.1.0\","
                     "\"wifiuser\":\"%s\","
                     "\"wifipass\":\"%s\","
                     "\"ip\":\"%s\","
                     "\"user\":\"%s\","
                     "\"pass\":\"%s\","
                     "\"sms1\":\"%s\","
                     "\"sms2\":\"%s\","
                     "\"sms3\":\"%s\","
                     "\"sms4\":\"%s\","
                     "\"sms5\":\"%s\","
                     "\"message\":\"%s\","
                     "\"phone1\":\"%s\","
                     "\"phone2\":\"\","
                     "\"phone3\":\"\","
                     "\"phone4\":\"\","
                     "\"phone5\":\"\","
                     "\"ahi\":%.2f,"
                     "\"alo\":%.2f,"
                     "\"temp_sht_hi\":100,"
                     "\"temp_sht_lo\":0,"
                     "\"hum_sht_hi\":100,"
                     "\"hum_sht_lo\":0,"
                     "\"calib\":%.2f,"
                     "\"calib_ntc_pcb\":%.2f,"
                     "\"calib_temp_sht\":0.0,"
                     "\"calib_hum_sht\":%.2f,"
                     "\"time_delay\":%d,"
                     "\"gps\":%s",
                     cfg->wifi_ssid,
                     cfg->wifi_pass,
                     cfg->broker_uri,
                     cfg->mqtt_username,
                     cfg->mqtt_password,
                     cfg->alarm_number1,
                     cfg->alarm_number2,
                     cfg->alarm_number3,
                     cfg->alarm_number4,
                     cfg->alarm_number5,
                     cfg->message,
                     cfg->alarm_number6,
                     cfg->ntc_high_limit_c,
                     cfg->ntc_low_limit_c,
                     cfg->ntc_calib_c,
                     cfg->ntc_calib_c,
                     cfg->hum_calib_pct,
                     cfg->telemetry_interval_ms,
                     cfg->gps_enabled ? "true" : "false");

    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "get config response too long");
        publish_error_result(request_id, "get config", "response_too_long");
        return;
    }

    mqtt_service_publish_reply(request_id ? request_id : "", body);
}

void config_sync_handle_cloud_command(const app_cloud_cmd_t *cmd)
{
    static runtime_config_t cfg;

    if (cmd == NULL) {
        return;
    }

    memset(&cfg, 0, sizeof(cfg));

    ESP_LOGW(TAG, "cloud cmd=%s params=%s request_id=%s",
             cmd->cmd,
             cmd->params,
             cmd->request_id);

    if (strcmp(cmd->cmd, "set config") == 0) {
        if (!cmd->params[0]) {
            publish_error_result(cmd->request_id, "set config", "empty_params");
            return;
        }

        cJSON *root = cJSON_Parse(cmd->params);
        if (!root) {
            ESP_LOGE(TAG, "invalid json for set config");
            publish_error_result(cmd->request_id, "set config", "invalid_json");
            return;
        }

        if (runtime_config_load(&cfg) != ESP_OK) {
            memset(&cfg, 0, sizeof(cfg));
        }

        json_get_string(root, "ip",   cfg.broker_uri,    sizeof(cfg.broker_uri));
        json_get_string(root, "user", cfg.mqtt_username, sizeof(cfg.mqtt_username));
        json_get_string(root, "pass", cfg.mqtt_password, sizeof(cfg.mqtt_password));

        if (json_get_string(root, "wifiuser", cfg.wifi_ssid, sizeof(cfg.wifi_ssid))) {
            cfg.wifi_enabled = (cfg.wifi_ssid[0] != '\0');
        }
        json_get_string(root, "wifipass", cfg.wifi_pass, sizeof(cfg.wifi_pass));

        json_get_string(root, "sms1", cfg.alarm_number1, sizeof(cfg.alarm_number1));
        json_get_string(root, "sms2", cfg.alarm_number2, sizeof(cfg.alarm_number2));
        json_get_string(root, "sms3", cfg.alarm_number3, sizeof(cfg.alarm_number3));
        json_get_string(root, "sms4", cfg.alarm_number4, sizeof(cfg.alarm_number4));
        json_get_string(root, "sms5", cfg.alarm_number5, sizeof(cfg.alarm_number5));

        json_get_string(root, "message", cfg.message, sizeof(cfg.message));
        json_get_string(root, "phone1", cfg.alarm_number6, sizeof(cfg.alarm_number6));

        json_get_float(root, "ahi", &cfg.ntc_high_limit_c);
        json_get_float(root, "alo", &cfg.ntc_low_limit_c);
        json_get_float(root, "humhi", &cfg.hum_high_limit_pct);
        json_get_float(root, "humlo", &cfg.hum_low_limit_pct);

        json_get_string(root, "apn", cfg.apn, sizeof(cfg.apn));
        json_get_bool(root, "gps", &cfg.gps_enabled);

        cJSON_Delete(root);

        if (runtime_config_save(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "runtime_config_save failed");
            publish_error_result(cmd->request_id, "set config", "save_failed");
            return;
        }

        app_logic_load_runtime_config();
        s_wifi_reconnect_pending = true;
        // esp_err_t evt_err = esp_event_post(APP_EVENTS,
        //                            APP_EVENT_WIFI_CONFIG_SAVED,
        //                            NULL,
        //                            0,
        //                            portMAX_DELAY);
        // if (evt_err != ESP_OK) {
        //     ESP_LOGW(TAG, "post APP_EVENT_WIFI_CONFIG_SAVED failed: %s", esp_err_to_name(evt_err));
        // }

        ESP_LOGW(TAG,
                 "set config ok: broker=%s sms1=%s phone1=%s ahi=%.2f alo=%.2f humhi=%.2f humlo=%.2f wifi=%s gps=%d",
                 cfg.broker_uri,
                 cfg.alarm_number1,
                 cfg.alarm_number6,
                 cfg.ntc_high_limit_c,
                 cfg.ntc_low_limit_c,
                 cfg.hum_high_limit_pct,
                 cfg.hum_low_limit_pct,
                 cfg.wifi_ssid,
                 cfg.gps_enabled ? 1 : 0);

        ESP_LOGW(TAG, "set config saved, wait admin set config for final response");
        return;
    }

    
    if (strcmp(cmd->cmd, "admin set config") == 0) {
        if (!cmd->params[0]) {
            publish_error_result(cmd->request_id, "admin set config", "empty_params");
            return;
        }

        cJSON *root = cJSON_Parse(cmd->params);
        if (!root) {
            ESP_LOGE(TAG, "invalid json for admin set config");
            publish_error_result(cmd->request_id, "admin set config", "invalid_json");
            return;
        }

        if (runtime_config_load(&cfg) != ESP_OK) {
            memset(&cfg, 0, sizeof(cfg));
        }

        json_get_float(root, "calib", &cfg.ntc_calib_c);
        json_get_int(root, "time_delay", &cfg.telemetry_interval_ms);
        /* time_repeat: runtime_config_t chua co field, tam thoi bo qua */

        cJSON_Delete(root);

        if (runtime_config_save(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "runtime_config_save failed for admin set config");
            publish_error_result(cmd->request_id, "admin set config", "save_failed");
            return;
        }

        app_logic_load_runtime_config();

        ESP_LOGW(TAG,
                 "admin set config ok: calib=%.2f time_delay=%d",
                 cfg.ntc_calib_c,
                 cfg.telemetry_interval_ms);

        publish_get_config_response(cmd->request_id, &cfg);
        if (s_wifi_reconnect_pending) {
            esp_err_t evt_err = esp_event_post(APP_EVENTS,
                                            APP_EVENT_WIFI_CONFIG_SAVED,
                                            NULL,
                                            0,
                                            portMAX_DELAY);
            if (evt_err != ESP_OK) {
                ESP_LOGW(TAG, "post APP_EVENT_WIFI_CONFIG_SAVED failed: %s", esp_err_to_name(evt_err));
            } else {
                ESP_LOGW(TAG, "admin set config reply done -> reconnect Wi-Fi");
                s_wifi_reconnect_pending = false;
            }
        }
        return;
    }

    if (strcmp(cmd->cmd, "get config") == 0) {
        if (runtime_config_load(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "runtime_config_load failed");
            publish_error_result(cmd->request_id, "get config", "load_failed");
            return;
        }

        ESP_LOGW(TAG, "get config ok");
        publish_get_config_response(cmd->request_id, &cfg);
        return;
    }

    ESP_LOGW(TAG, "ignore unsupported cmd=%s", cmd->cmd);
}
// void config_sync_handle_cloud_command(const app_cloud_cmd_t *cmd)
// {
//     // runtime_config_t cfg;
//     static runtime_config_t cfg;
//     memset(&cfg, 0, sizeof(cfg));
//     // if (!cfg) {
//     //     publish_error_result(cmd->request_id, cmd->cmd, "no_mem");
//     //     return;
//     // }
//     if (cmd == NULL) {
//         return;
//     }
//     memset(&cfg, 0, sizeof(cfg));
//     ESP_LOGW(TAG, "cloud cmd=%s params=%s request_id=%s",
//              cmd->cmd,
//              cmd->params,
//              cmd->request_id);

//     if (strcmp(cmd->cmd, "set config") == 0) {
//         if (!cmd->params[0]) {
//             // publish_error_result(cmd->request_id, "set config", "empty_params");
//             // return;
//             ESP_LOGW(TAG, "set config saved, wait admin set config for final response");
//             return;
//         }

//         cJSON *root = cJSON_Parse(cmd->params);
//         if (!root) {
//             ESP_LOGE(TAG, "invalid json for set config");
//             publish_error_result(cmd->request_id, "set config", "invalid_json");
//             return;
//         }

//         if (runtime_config_load(&cfg) != ESP_OK) {
//             memset(&cfg, 0, sizeof(cfg));
//         }

//         json_get_string(root, "ip",   cfg.broker_uri,    sizeof(cfg.broker_uri));
//         json_get_string(root, "user", cfg.mqtt_username, sizeof(cfg.mqtt_username));
//         json_get_string(root, "pass", cfg.mqtt_password, sizeof(cfg.mqtt_password));

//         if (json_get_string(root, "wifiuser", cfg.wifi_ssid, sizeof(cfg.wifi_ssid))) {
//             cfg.wifi_enabled = (cfg.wifi_ssid[0] != '\0');
//         }
//         json_get_string(root, "wifipass", cfg.wifi_pass, sizeof(cfg.wifi_pass));

//         json_get_string(root, "sms1", cfg.alarm_number1, sizeof(cfg.alarm_number1));
//         json_get_string(root, "sms2", cfg.alarm_number2, sizeof(cfg.alarm_number2));
//         json_get_string(root, "sms3", cfg.alarm_number3, sizeof(cfg.alarm_number3));
//         json_get_string(root, "sms4", cfg.alarm_number4, sizeof(cfg.alarm_number4));
//         json_get_string(root, "sms5", cfg.alarm_number5, sizeof(cfg.alarm_number5));

//         json_get_string(root, "phone1", cfg.alarm_number6, sizeof(cfg.alarm_number6));

//         json_get_float(root, "ahi", &cfg.ntc_high_limit_c);
//         json_get_float(root, "alo", &cfg.ntc_low_limit_c);
//         json_get_float(root, "humhi", &cfg.hum_high_limit_pct);
//         json_get_float(root, "humlo", &cfg.hum_low_limit_pct);

//         json_get_float(root, "calib", &cfg.ntc_calib_c);
//         json_get_float(root, "hum_calib", &cfg.hum_calib_pct);

//         json_get_int(root, "telemetry_interval_ms", &cfg.telemetry_interval_ms);
//         json_get_int(root, "time_delay", &cfg.telemetry_interval_ms);

//         json_get_string(root, "apn", cfg.apn, sizeof(cfg.apn));
//         json_get_string(root, "message", cfg.message, sizeof(cfg.message));
//         json_get_bool(root, "gps", &cfg.gps_enabled);

//         cJSON_Delete(root);

//         if (runtime_config_save(&cfg) != ESP_OK) {
//             ESP_LOGE(TAG, "runtime_config_save failed");
//             publish_error_result(cmd->request_id, "set config", "save_failed");
//             return;
//         }

//         app_logic_load_runtime_config();

//         ESP_LOGW(TAG,
//                  "set config ok: broker=%s sms1=%s phone1=%s ahi=%.2f alo=%.2f humhi=%.2f humlo=%.2f wifi=%s gps=%d",
//                  cfg.broker_uri,
//                  cfg.alarm_number1,
//                  cfg.alarm_number6,
//                  cfg.ntc_high_limit_c,
//                  cfg.ntc_low_limit_c,
//                  cfg.hum_high_limit_pct,
//                  cfg.hum_low_limit_pct,
//                  cfg.wifi_ssid,
//                  cfg.gps_enabled ? 1 : 0);

//         publish_full_config_response(cmd->request_id, &cfg);
//         return;
//     }

//     if (strcmp(cmd->cmd, "admin set config") == 0) {
//         if (!cmd->params[0]) {
//             publish_error_result(cmd->request_id, "admin set config", "empty_params");
//             return;
//         }

//         cJSON *root = cJSON_Parse(cmd->params);
//         if (!root) {
//             ESP_LOGE(TAG, "invalid json for admin set config");
//             publish_error_result(cmd->request_id, "admin set config", "invalid_json");
//             return;
//         }

//         if (runtime_config_load(&cfg) != ESP_OK) {
//             memset(&cfg, 0, sizeof(cfg));
//         }

//         json_get_float(root, "calib", &cfg.ntc_calib_c);
//         json_get_int(root, "time_delay", &cfg.telemetry_interval_ms);
//         /* time_repeat: runtime_config_t chua co field, tam bo qua */

//         cJSON_Delete(root);

//         if (runtime_config_save(&cfg) != ESP_OK) {
//             ESP_LOGE(TAG, "runtime_config_save failed for admin set config");
//             publish_error_result(cmd->request_id, "admin set config", "save_failed");
//             return;
//         }

//         app_logic_load_runtime_config();

//         ESP_LOGW(TAG,
//                  "admin set config ok: calib=%.2f time_delay=%d",
//                  cfg.ntc_calib_c,
//                  cfg.telemetry_interval_ms);

//         publish_full_config_response(cmd->request_id, &cfg);
//         return;
//     }

//     if (strcmp(cmd->cmd, "get config") == 0) {
//         if (runtime_config_load(&cfg) != ESP_OK) {
//             ESP_LOGE(TAG, "runtime_config_load failed");
//             publish_error_result(cmd->request_id, "get config", "load_failed");
//             return;
//         }

//         ESP_LOGW(TAG, "get config ok");
//         publish_full_config_response(cmd->request_id, &cfg);
//         return;
//     }

//     ESP_LOGW(TAG, "ignore unsupported cmd=%s", cmd->cmd);
// }

void config_sync_publish_current_config_for_web(const char *request_id)
{
    runtime_config_t cfg = {0};

    if (runtime_config_load(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "runtime_config_load failed for sms sync");
        return;
    }

    publish_get_config_response(request_id ? request_id : "sms", &cfg);
}