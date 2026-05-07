#include "setup_service.h"
#include "settings.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>

static const char* TAG = "SetupService";

// ========== External embedded HTML assets ==========
extern const char setup_schedule_html_start[] asm("_binary_setup_schedule_html_start");
extern const char setup_schedule_html_end[]   asm("_binary_setup_schedule_html_end");
extern const char setup_weather_html_start[]  asm("_binary_setup_weather_html_start");
extern const char setup_weather_html_end[]    asm("_binary_setup_weather_html_end");
extern const char setup_done_html_start[]     asm("_binary_setup_done_html_start");
extern const char setup_done_html_end[]       asm("_binary_setup_done_html_end");

// ========== Helpers ==========

static void send_json(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void send_html(httpd_req_t* req, const char* start, const char* end) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, start, end - start);
}

static void register_route(httpd_handle_t server, const httpd_uri_t* uri, const char* name) {
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Route registered: %s", name);
    } else {
        ESP_LOGE(TAG, "Failed to register route %s: %s", name, esp_err_to_name(err));
    }
}

// ========== /api/config/status ==========

static esp_err_t api_config_status_handler(httpd_req_t* req) {
    // Schedule: namespace "setup", key "schedule" (single JSON)
    Settings settings("setup", false);
    std::string schedule_json = settings.GetString("schedule", "");

    // Weather: namespace "setup", key "weather_secret" and "weather_city"
    std::string weather_key = settings.GetString("weather_secret", "");
    std::string weather_city = settings.GetString("weather_city", "");
    std::string wifi_ssid = settings.GetString("wifi_ssid", "");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "schedule_configured", !schedule_json.empty());
    cJSON_AddBoolToObject(root, "weather_configured", !weather_key.empty());
    cJSON_AddBoolToObject(root, "wifi_configured", !wifi_ssid.empty());
    cJSON_AddStringToObject(root, "weather_key", weather_key.c_str());
    cJSON_AddStringToObject(root, "weather_city", weather_city.c_str());
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid.c_str());

    // Parse schedule JSON to extract individual fields
    if (!schedule_json.empty()) {
        cJSON* schedule_root = cJSON_Parse(schedule_json.c_str());
        if (schedule_root) {
            cJSON* ss = cJSON_GetObjectItem(schedule_root, "semester_start");
            if (cJSON_IsString(ss)) cJSON_AddStringToObject(root, "semester_start", ss->valuestring);
            cJSON* se = cJSON_GetObjectItem(schedule_root, "semester_end");
            if (cJSON_IsString(se)) cJSON_AddStringToObject(root, "semester_end", se->valuestring);
            // Extract schedule.single and schedule.dual
            cJSON* sched = cJSON_GetObjectItem(schedule_root, "schedule");
            if (sched) {
                cJSON* single = cJSON_GetObjectItem(sched, "single");
                if (single) cJSON_AddItemToObject(root, "schedule_single", cJSON_Duplicate(single, 1));
                cJSON* dual = cJSON_GetObjectItem(sched, "dual");
                if (dual) cJSON_AddItemToObject(root, "schedule_double", cJSON_Duplicate(dual, 1));
            }
            cJSON_Delete(schedule_root);
        }
    }

    char* str = cJSON_PrintUnformatted(root);
    send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ========== POST /api/schedule ==========

static esp_err_t api_schedule_handler(httpd_req_t* req) {
    char buf[4096] = {};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json(req, "{\"success\":false,\"error\":\"empty request\"}");
        return ESP_OK;
    }
    buf[ret] = '\0';

    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        send_json(req, "{\"success\":false,\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    // Build the schedule JSON in the format home_data_store expects:
    // { semester_start, semester_end, week_type, schedule: { single: {...}, dual: {...} } }
    cJSON* schedule_obj = cJSON_CreateObject();

    cJSON* semester_start = cJSON_GetObjectItem(json, "semester_start");
    if (cJSON_IsString(semester_start)) {
        cJSON_AddStringToObject(schedule_obj, "semester_start", semester_start->valuestring);
    }

    cJSON* semester_end = cJSON_GetObjectItem(json, "semester_end");
    if (cJSON_IsString(semester_end)) {
        cJSON_AddStringToObject(schedule_obj, "semester_end", semester_end->valuestring);
    }

    cJSON_AddStringToObject(schedule_obj, "week_type", "single");

    // Nest schedule under "schedule" key with "single" and "dual"
    cJSON* inner_schedule = cJSON_CreateObject();
    cJSON* sched_single = cJSON_GetObjectItem(json, "schedule_single");
    if (sched_single) {
        cJSON_AddItemToObject(inner_schedule, "single", cJSON_Duplicate(sched_single, 1));
    }
    cJSON* sched_double = cJSON_GetObjectItem(json, "schedule_double");
    if (sched_double) {
        cJSON_AddItemToObject(inner_schedule, "dual", cJSON_Duplicate(sched_double, 1));
    } else if (sched_single) {
        // If no double, use single as fallback
        cJSON_AddItemToObject(inner_schedule, "dual", cJSON_Duplicate(sched_single, 1));
    }
    cJSON_AddItemToObject(schedule_obj, "schedule", inner_schedule);

    char* schedule_str = cJSON_PrintUnformatted(schedule_obj);
    ESP_LOGI(TAG, "Saving schedule JSON (len=%d)", (int)strlen(schedule_str));

    Settings settings("setup", true);
    settings.SetString("schedule", schedule_str);

    cJSON_free(schedule_str);
    cJSON_Delete(schedule_obj);
    cJSON_Delete(json);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

// ========== POST /api/weather ==========

static esp_err_t api_weather_handler(httpd_req_t* req) {
    char buf[1024] = {};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json(req, "{\"success\":false,\"error\":\"empty request\"}");
        return ESP_OK;
    }
    buf[ret] = '\0';

    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        send_json(req, "{\"success\":false,\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    Settings settings("setup", true);

    cJSON* api_key = cJSON_GetObjectItem(json, "api_key");
    if (cJSON_IsString(api_key) && strlen(api_key->valuestring) > 0) {
        settings.SetString("weather_secret", api_key->valuestring);
        ESP_LOGI(TAG, "Weather API key saved (len=%d)", (int)strlen(api_key->valuestring));
    }

    cJSON* city = cJSON_GetObjectItem(json, "city");
    if (cJSON_IsString(city) && strlen(city->valuestring) > 0) {
        settings.SetString("weather_city", city->valuestring);
        ESP_LOGI(TAG, "Weather city saved: %s", city->valuestring);
    }

    cJSON_Delete(json);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

// ========== Registration ==========

void SetupService::RegisterRoutes(httpd_handle_t server) {
    if (!server) {
        ESP_LOGE(TAG, "NULL server handle");
        return;
    }

    // ---- Static HTML pages ----
    httpd_uri_t schedule_page = {
        .uri = "/schedule.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            send_html(req, setup_schedule_html_start, setup_schedule_html_end);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    register_route(server, &schedule_page, "/schedule.html");

    httpd_uri_t weather_page = {
        .uri = "/weather.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            send_html(req, setup_weather_html_start, setup_weather_html_end);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    register_route(server, &weather_page, "/weather.html");

    httpd_uri_t done_page = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            send_html(req, setup_done_html_start, setup_done_html_end);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    register_route(server, &done_page, "/done.html");

    // ---- API endpoints ----
    httpd_uri_t config_status = {
        .uri = "/api/config/status",
        .method = HTTP_GET,
        .handler = api_config_status_handler,
        .user_ctx = nullptr
    };
    register_route(server, &config_status, "/api/config/status");

    httpd_uri_t schedule_api = {
        .uri = "/api/schedule",
        .method = HTTP_POST,
        .handler = api_schedule_handler,
        .user_ctx = nullptr
    };
    register_route(server, &schedule_api, "/api/schedule");

    httpd_uri_t weather_api = {
        .uri = "/api/weather",
        .method = HTTP_POST,
        .handler = api_weather_handler,
        .user_ctx = nullptr
    };
    register_route(server, &weather_api, "/api/weather");

    ESP_LOGI(TAG, "All setup routes registered");
}
