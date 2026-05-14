/**
 * @file home_data_store.cc
 * @brief 首页数据层实现 —— 课程表解析 + 天气 API 调用
 *
 * 实现 HomeDataStore 类的全部功能：
 * - NVS 读取课程表与天气配置
 * - cJSON 解析课程 JSON
 * - 单双周判断（基于学期起始日期）
 * - 心和天气 HTTP API 调用与 JSON 响应解析
 * - 天气数据 30 分钟缓存策略
 */

#include "home_data_store.h"
#include "settings.h"

#include <cstring>
#include <ctime>
#include <cmath>
#include <algorithm>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "HomeDataStore";
static const char* WEATHER_LAST_TODAY_KEY = "w_last";

/**
 * @brief URL 编码（百分号编码）
 *
 * 将非 ASCII 字符和特殊字符转换为 %XX 格式。
 * 用于城市名等可能包含中文的参数。
 */
static void UrlEncode(const char* src, char* dst, size_t dst_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 4 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0x0F];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

static std::string GetDateOffsetString(int offset_days) {
    time_t now = time(nullptr);
    if (now <= 0) {
        return "";
    }
    now += offset_days * 86400;
    struct tm time_info = {};
    if (localtime_r(&now, &time_info) == nullptr) {
        return "";
    }
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &time_info);
    return buf;
}

static std::string SerializeWeatherDay(const WeatherDay& day) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "";
    }
    cJSON_AddStringToObject(root, "date", day.date.c_str());
    cJSON_AddStringToObject(root, "high", day.high_temp.c_str());
    cJSON_AddStringToObject(root, "low", day.low_temp.c_str());
    cJSON_AddStringToObject(root, "text", day.description.c_str());
    cJSON_AddStringToObject(root, "humidity", day.humidity.c_str());
    cJSON_AddStringToObject(root, "wind_scale", day.wind_scale.c_str());
    cJSON_AddStringToObject(root, "precip", day.precip.c_str());
    char* json = cJSON_PrintUnformatted(root);
    std::string result = json != nullptr ? json : "";
    if (json != nullptr) {
        cJSON_free(json);
    }
    cJSON_Delete(root);
    return result;
}

static bool ParseWeatherDayJson(const std::string& json, WeatherDay& out) {
    if (json.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }
    auto get_str = [root](const char* key) -> const char* {
        cJSON* item = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(item) ? item->valuestring : nullptr;
    };
    const char* date = get_str("date");
    const char* high = get_str("high");
    const char* low = get_str("low");
    const char* text = get_str("text");
    const char* humidity = get_str("humidity");
    const char* wind_scale = get_str("wind_scale");
    const char* precip = get_str("precip");
    if (date) out.date = date;
    if (high) out.high_temp = high;
    if (low) out.low_temp = low;
    if (text) out.description = text;
    if (humidity) out.humidity = humidity;
    if (wind_scale) out.wind_scale = wind_scale;
    if (precip) out.precip = precip;
    cJSON_Delete(root);
    return !out.date.empty();
}

// ============================================================================
// 构造 / 析构
// ============================================================================

HomeDataStore::HomeDataStore()
    : schedule_parsed_(false)
{
    // 初始化首页数据结构
    data_.last_weather_update = 0;
    data_.has_schedule = false;
    data_.has_weather_config = false;

    // 初始化单双周课程数组的星期名称
    for (int i = 0; i < 5; ++i) {
        single_week_[i].day_name = DAY_NAMES[i];
        dual_week_[i].day_name = DAY_NAMES[i];
    }

    // 预分配 HTTP 响应缓冲区，避免运行时反复 malloc
    http_buf_.resize(HTTP_RESPONSE_BUF_SIZE);

    ESP_LOGI(TAG, "HomeDataStore 已创建");
}

HomeDataStore::~HomeDataStore() {
    ESP_LOGI(TAG, "HomeDataStore 已销毁");
}

// ============================================================================
// NVS 加载入口
// ============================================================================

bool HomeDataStore::LoadFromNvs() {
    bool sched_ok = LoadScheduleFromNvs();
    bool weather_ok = LoadWeatherConfigFromNvs();

    data_.has_schedule = sched_ok;
    data_.has_weather_config = weather_ok;

    WeatherDay cached_yesterday;
    Settings settings("setup");
    if (ParseWeatherDayJson(settings.GetString(WEATHER_LAST_TODAY_KEY), cached_yesterday) &&
        cached_yesterday.date == GetDateOffsetString(-1)) {
        data_.weather_detail[0] = cached_yesterday;
        ESP_LOGI(TAG, "已从本地缓存恢复昨日天气: %s", cached_yesterday.date.c_str());
    }

    ESP_LOGI(TAG, "NVS 加载完成: 课程表=%s, 天气配置=%s",
             sched_ok ? "成功" : "失败",
             weather_ok ? "成功" : "失败");

    return sched_ok || weather_ok;
}

// ============================================================================
// 课程表 NVS 读取与解析
// ============================================================================

bool HomeDataStore::LoadScheduleFromNvs() {
    // 打开 NVS "setup" 命名空间（只读模式）
    Settings settings("setup");

    // 读取课程表完整 JSON（web 配网页面存储的格式）
    // NVS key: "schedule"，包含 semester_start, semester_end, week_type, schedule.single, schedule.dual
    std::string schedule_json = settings.GetString("schedule");
    if (schedule_json.empty()) {
        ESP_LOGW(TAG, "NVS 中未找到 schedule 键（可能还未配置课程表）");
        schedule_json_cache_.clear();
        schedule_parsed_ = false;
        data_.has_schedule = false;
        return false;
    }

    ESP_LOGI(TAG, "读取到课程表 JSON，长度: %d", (int)schedule_json.size());

    // 解析完整 JSON
    cJSON* root = cJSON_Parse(schedule_json.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "课程表 JSON 解析失败: %s", cJSON_GetErrorPtr());
        return false;
    }

    // 读取学期开始日期
    cJSON* j_semester_start = cJSON_GetObjectItem(root, "semester_start");
    if (cJSON_IsString(j_semester_start) && j_semester_start->valuestring) {
        semester_start_ = j_semester_start->valuestring;
    }

    // 读取学期结束日期
    cJSON* j_semester_end = cJSON_GetObjectItem(root, "semester_end");
    if (cJSON_IsString(j_semester_end) && j_semester_end->valuestring) {
        semester_end_ = j_semester_end->valuestring;
    }

    // 读取当前周类型（single/dual）
    cJSON* j_week_type = cJSON_GetObjectItem(root, "week_type");
    if (cJSON_IsString(j_week_type) && j_week_type->valuestring) {
        initial_week_type_ = j_week_type->valuestring;  // "single" 或 "dual"
    }

    ESP_LOGI(TAG, "学期范围: %s ~ %s, 初始周类型: %s",
             semester_start_.c_str(),
             semester_end_.empty() ? "未设置" : semester_end_.c_str(),
             initial_week_type_.empty() ? "未设置" : initial_week_type_.c_str());

    // 读取课程表子对象
    cJSON* j_schedule = cJSON_GetObjectItem(root, "schedule");
    if (!cJSON_IsObject(j_schedule)) {
        ESP_LOGE(TAG, "JSON 中缺少 schedule 对象");
        cJSON_Delete(root);
        return false;
    }

    // 解析单周课程
    cJSON* j_single = cJSON_GetObjectItem(j_schedule, "single");
    if (cJSON_IsObject(j_single)) {
        char* single_str = cJSON_PrintUnformatted(j_single);
        if (single_str) {
            ParseScheduleJson(single_str, single_week_);
            free(single_str);
        }
    }

    // 解析双周课程（可选，缺失时复制单周数据）
    cJSON* j_dual = cJSON_GetObjectItem(j_schedule, "dual");
    if (cJSON_IsObject(j_dual)) {
        char* dual_str = cJSON_PrintUnformatted(j_dual);
        if (dual_str) {
            ParseScheduleJson(dual_str, dual_week_);
            free(dual_str);
        }
    } else {
        ESP_LOGW(TAG, "双周课程数据缺失，使用单周数据代替");
        for (int i = 0; i < 5; ++i) {
            dual_week_[i] = single_week_[i];
        }
    }

    cJSON_Delete(root);

    schedule_json_cache_ = schedule_json;
    schedule_parsed_ = true;
    ESP_LOGI(TAG, "课程表解析成功（单周 + 双周）");
    return true;
}

bool HomeDataStore::ParseScheduleJson(const char* json_str, DaySchedule out_schedule[5]) {
    if (json_str == nullptr || json_str[0] == '\0') {
        return false;
    }

    // 使用 cJSON 解析 JSON 字符串
    cJSON* root = cJSON_Parse(json_str);
    if (root == nullptr) {
        ESP_LOGE(TAG, "cJSON 解析失败: %s", cJSON_GetErrorPtr());
        return false;
    }

    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "课程 JSON 根节点不是对象");
        cJSON_Delete(root);
        return false;
    }

    // 清空输出数组中的旧数据
    for (int i = 0; i < 5; ++i) {
        out_schedule[i].courses.clear();
    }

    // 遍历 JSON 对象中的所有键值对
    // 键名格式: "mon_1", "tue_3", "fri_8" 等
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsString(item) || item->string == nullptr) {
            continue;
        }

        const char* key = item->string;
        const char* value = item->valuestring;

        // 跳过空课程名
        if (value == nullptr || value[0] == '\0') {
            continue;
        }

        // 解析键名: 格式为 "day_period"
        // 找到下划线分隔符
        const char* underscore = strchr(key, '_');
        if (underscore == nullptr) {
            ESP_LOGW(TAG, "无效的课程键名: %s（缺少下划线分隔符）", key);
            continue;
        }

        // 提取星期几部分
        std::string day_part(key, underscore - key);
        int day_idx = DayNameToIndex(day_part);
        if (day_idx < 0) {
            ESP_LOGW(TAG, "未知的星期标识: %s", day_part.c_str());
            continue;
        }

        // 提取节次编号
        int period = atoi(underscore + 1);
        if (period < 1 || period > 8) {
            ESP_LOGW(TAG, "无效的节次编号: %d（键名 %s）", period, key);
            continue;
        }

        // 添加到对应星期的课程列表
        CourseInfo course;
        course.period = period;
        course.name = value;
        out_schedule[day_idx].courses.push_back(course);
    }

    cJSON_Delete(root);

    // 对每天的课程按节次排序（确保显示顺序正确）
    for (int i = 0; i < 5; ++i) {
        std::sort(out_schedule[i].courses.begin(),
                  out_schedule[i].courses.end(),
                  [](const CourseInfo& a, const CourseInfo& b) {
                      return a.period < b.period;
                  });
    }

    return true;
}

// ============================================================================
// 单双周判断
// ============================================================================

bool HomeDataStore::IsDualWeek(time_t target_date) const {
    if (semester_start_.empty()) {
        // 未设置学期开始日期，默认为单周
        return false;
    }

    // 解析学期开始日期字符串 "YYYY-MM-DD"
    struct tm start_tm = {};
    if (strptime(semester_start_.c_str(), "%Y-%m-%d", &start_tm) == nullptr) {
        ESP_LOGE(TAG, "学期开始日期格式无效: %s", semester_start_.c_str());
        return false;
    }
    // 将 start_tm 规范化（strptime 可能不设置 tm_isdst）
    start_tm.tm_hour = 0;
    start_tm.tm_min = 0;
    start_tm.tm_sec = 0;
    start_tm.tm_isdst = -1;
    time_t start_time = mktime(&start_tm);
    if (start_time < 0) {
        ESP_LOGE(TAG, "学期开始日期转换失败");
        return false;
    }

    struct tm normalized_start_tm = {};
    localtime_r(&start_time, &normalized_start_tm);
    int days_from_monday = normalized_start_tm.tm_wday == 0 ? 6 : normalized_start_tm.tm_wday - 1;
    start_time -= days_from_monday * 86400;

    // 获取目标日期的 0 点时间
    struct tm target_tm = {};
    localtime_r(&target_date, &target_tm);
    target_tm.tm_hour = 0;
    target_tm.tm_min = 0;
    target_tm.tm_sec = 0;
    target_tm.tm_isdst = -1;
    time_t target_midnight = mktime(&target_tm);

    // 计算天数差
    double diff_seconds = difftime(target_midnight, start_time);
    int64_t diff_days = static_cast<int64_t>(diff_seconds / 86400.0);

    if (diff_days < 0) {
        // 目标日期在学期开始之前，默认为单周
        ESP_LOGW(TAG, "当前日期在学期开始日期之前，天数差: %lld", (long long)diff_days);
        return false;
    }

    // 计算第几周（从 0 开始）：week_index = diff_days / 7
    // 根据 initial_week_type_ 校准：
    //   - 如果配置时是 single（单周），则 week_index % 2 == 0 → 单周
    //   - 如果配置时是 dual（双周），则 week_index % 2 == 0 → 双周
    int64_t week_index = diff_days / 7;
    bool base_is_dual = (initial_week_type_ == "dual");
    bool is_dual = ((week_index % 2) == 0) ? base_is_dual : !base_is_dual;

    ESP_LOGD(TAG, "距学期开始 %lld 天, 第 %lld 周, %s",
             (long long)diff_days, (long long)week_index,
             is_dual ? "双周" : "单周");

    return is_dual;
}

// ============================================================================
// 课程更新（基于当前时间）
// ============================================================================

const HomeData& HomeDataStore::UpdateSchedule() {
    Settings settings("setup");
    std::string current_schedule_json = settings.GetString("schedule");
    if (!current_schedule_json.empty() && current_schedule_json != schedule_json_cache_) {
        ESP_LOGI(TAG, "检测到课程表配置变更，重新加载");
        data_.has_schedule = LoadScheduleFromNvs();
    }

    if (!schedule_parsed_) {
        // 课程表未加载，返回空数据
        ESP_LOGD(TAG, "课程表未加载，跳过更新");
        return data_;
    }

    // 获取当前系统时间
    time_t now = time(nullptr);
    if (now < 946684800) {
        // 时间尚未同步（2000-01-01 之前），无法计算
        ESP_LOGW(TAG, "系统时间尚未同步，无法计算课程");
        return data_;
    }

    struct tm now_tm = {};
    localtime_r(&now, &now_tm);

    // tm_wday: 0=周日, 1=周一, ..., 6=周六
    // 我们的数组索引: 0=周一, 1=周二, ..., 4=周五
    int today_wday = now_tm.tm_wday;
    int today_idx = (today_wday == 0) ? -1 : today_wday - 1;  // 周日=-1
    // 周六 = 5（超出范围），周日 = -1
    bool is_weekend_today = (today_wday == 0 || today_wday == 6);

    // 计算明天
    time_t tomorrow = now + 86400;
    struct tm tmr_tm = {};
    localtime_r(&tomorrow, &tmr_tm);
    int tmr_wday = tmr_tm.tm_wday;
    int tmr_idx = (tmr_wday == 0) ? -1 : tmr_wday - 1;
    bool is_weekend_tomorrow = (tmr_wday == 0 || tmr_wday == 6);

    // 判断当前是单周还是双周
    bool is_dual = IsDualWeek(now);
    const DaySchedule* week_schedule = is_dual ? dual_week_ : single_week_;

    // 填充今日课程
    data_.today_schedule.day_name = DAY_NAMES[today_wday];
    data_.today_schedule.courses.clear();
    if (!is_weekend_today && today_idx >= 0 && today_idx < 5) {
        data_.today_schedule.courses = week_schedule[today_idx].courses;
    }

    // 填充明日课程
    // 注意：如果明天跨周，需要重新判断单双周
    bool is_dual_tmr = IsDualWeek(tomorrow);
    const DaySchedule* tmr_week_schedule = is_dual_tmr ? dual_week_ : single_week_;
    data_.tomorrow_schedule.day_name = DAY_NAMES[tmr_wday];
    data_.tomorrow_schedule.courses.clear();
    if (!is_weekend_tomorrow && tmr_idx >= 0 && tmr_idx < 5) {
        data_.tomorrow_schedule.courses = tmr_week_schedule[tmr_idx].courses;
    }

    ESP_LOGD(TAG, "课程更新: 今天=%s(%s, %zu节), 明天=%s(%s, %zu节)",
             data_.today_schedule.day_name.c_str(),
             is_weekend_today ? "周末" : (is_dual ? "双周" : "单周"),
             data_.today_schedule.courses.size(),
             data_.tomorrow_schedule.day_name.c_str(),
             is_weekend_tomorrow ? "周末" : (is_dual_tmr ? "双周" : "单周"),
             data_.tomorrow_schedule.courses.size());

    return data_;
}

// ============================================================================
// 天气配置读取
// ============================================================================

bool HomeDataStore::LoadWeatherConfigFromNvs() {
    Settings settings("setup");

    // 读取心和天气 API Key（配网页面存储在 "weather_secret" 字段）
    weather_key_ = settings.GetString("weather_secret");
    if (weather_key_.empty()) {
        ESP_LOGW(TAG, "NVS 中未找到天气 API Key (weather_secret)");
        return false;
    }

    // 读取城市名
    weather_city_ = settings.GetString("weather_city");
    if (weather_city_.empty()) {
        ESP_LOGW(TAG, "NVS 中未找到城市配置 (weather_city)，使用默认值 shenzhen");
        weather_city_ = "shenzhen";
    }

    ESP_LOGI(TAG, "天气配置已加载: 城市=%s, Key=%s...",
             weather_city_.c_str(),
             weather_key_.length() > 4
                 ? (std::string(weather_key_.substr(0, 4)) + "***").c_str()
                 : "***");

    return true;
}

// ============================================================================
// 天气 API 调用
// ============================================================================

bool HomeDataStore::RefreshWeather() {
    ESP_LOGI(TAG, "RefreshWeather() called");
    
    if (!data_.has_weather_config) {
        ESP_LOGW(TAG, "天气配置未加载，跳过天气刷新");
        return false;
    }

    // 检查缓存有效期（30 分钟内不重复请求）
    time_t now = time(nullptr);
    ESP_LOGI(TAG, "Current time: %ld, last update: %ld", (long)now, (long)data_.last_weather_update);
    
    if (data_.last_weather_update > 0 &&
        (now - data_.last_weather_update) < WEATHER_CACHE_SECONDS) {
        ESP_LOGI(TAG, "天气数据仍在缓存有效期内（%ld 秒前更新）",
                 (long)(now - data_.last_weather_update));
        return true;
    }

    ESP_LOGI(TAG, "开始获取天气数据...");
    bool ok = FetchWeatherFromApi();
    if (ok) {
        data_.last_weather_update = now;
        ESP_LOGI(TAG, "天气数据更新成功");
    } else {
        ESP_LOGW(TAG, "天气数据获取失败");
    }
    return ok;
}

bool HomeDataStore::FetchWeatherFromApi() {
    if (weather_key_.empty() || weather_city_.empty()) {
        ESP_LOGE(TAG, "天气 API 配置不完整");
        return false;
    }

    // 拼接心和天气 API 请求 URL
    // 使用 HTTP 代替 HTTPS 避免 TLS 握手内存不足
    char url[512];
    char encoded_city[128];
    UrlEncode(weather_city_.c_str(), encoded_city, sizeof(encoded_city));
    int url_len = snprintf(url, sizeof(url),
             "http://api.seniverse.com/v3/weather/daily.json"
             "?key=%s&location=%s&language=zh-Hans&unit=c&start=-1&days=4",
             weather_key_.c_str(), encoded_city);

    ESP_LOGI(TAG, "天气请求 URL 长度: %d", url_len);
    ESP_LOGI(TAG, "天气请求 URL: %.80s...", url);

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;  // 15 秒超时
    config.buffer_size = HTTP_RESPONSE_BUF_SIZE;
    config.buffer_size_tx = 1024;

    ESP_LOGI(TAG, "正在初始化 HTTP 客户端...");
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        return false;
    }
    ESP_LOGI(TAG, "HTTP 客户端初始化成功，正在发起请求...");

    // 分步执行 HTTP 请求：open → fetch_headers → read，避免 perform() 内部消费响应体
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP 获取响应头失败");
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP 状态码: %d, 内容长度: %d", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP 请求失败，状态码: %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取响应体
    int total_read = 0;
    int remaining = HTTP_RESPONSE_BUF_SIZE - 1;
    while (remaining > 0) {
        int read_len = esp_http_client_read(client,
                                             http_buf_.data() + total_read,
                                             remaining);
        if (read_len <= 0) {
            break;
        }
        total_read += read_len;
        remaining -= read_len;
    }
    http_buf_[total_read] = '\0';

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "HTTP 响应体为空");
        return false;
    }

    ESP_LOGI(TAG, "收到 %d 字节天气数据", total_read);

    // 解析 JSON 响应
    return ParseWeatherResponse(http_buf_.data());
}

bool HomeDataStore::ParseWeatherResponse(const char* json_str) {
    if (json_str == nullptr || json_str[0] == '\0') {
        return false;
    }

    cJSON* root = cJSON_Parse(json_str);
    if (root == nullptr) {
        ESP_LOGE(TAG, "天气 JSON 解析失败: %s", cJSON_GetErrorPtr());
        return false;
    }

    // 心和天气 API 响应格式:
    // {
    //   "results": [{
    //     "location": { "name": "深圳" },
    //     "daily": [
    //       { "date": "2025-01-04", "text_day": "晴", "high": "22", "low": "14", "humidity": "65" },
    //       ...
    //     ]
    //   }]
    // }
    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (results == nullptr || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        ESP_LOGE(TAG, "天气 JSON 缺少 results 数组");
        cJSON_Delete(root);
        return false;
    }

    cJSON* first_result = cJSON_GetArrayItem(results, 0);
    if (first_result == nullptr) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* location = cJSON_GetObjectItem(first_result, "location");
    cJSON* location_name = location != nullptr ? cJSON_GetObjectItem(location, "name") : nullptr;
    if (location_name && cJSON_IsString(location_name)) {
        data_.weather_city = location_name->valuestring;
    } else if (data_.weather_city.empty()) {
        data_.weather_city = weather_city_;
    }

    cJSON* daily = cJSON_GetObjectItem(first_result, "daily");
    if (daily == nullptr || !cJSON_IsArray(daily)) {
        ESP_LOGE(TAG, "天气 JSON 缺少 daily 数组");
        cJSON_Delete(root);
        return false;
    }

    // 清空详情缓存，避免接口返回天数减少时残留旧数据。
    for (int i = 0; i < 4; ++i) {
        data_.weather_detail[i] = WeatherDay{};
    }

    auto parse_day = [](cJSON* day, WeatherDay& out) {
        if (day == nullptr) {
            return;
        }
        cJSON* j_date = cJSON_GetObjectItem(day, "date");
        cJSON* j_text = cJSON_GetObjectItem(day, "text_day");
        cJSON* j_high = cJSON_GetObjectItem(day, "high");
        cJSON* j_low = cJSON_GetObjectItem(day, "low");
        cJSON* j_humidity = cJSON_GetObjectItem(day, "humidity");
        cJSON* j_wind_scale = cJSON_GetObjectItem(day, "wind_scale");
        cJSON* j_precip = cJSON_GetObjectItem(day, "precip");

        if (j_date && cJSON_IsString(j_date)) out.date = j_date->valuestring;
        if (j_text && cJSON_IsString(j_text)) out.description = j_text->valuestring;
        if (j_high && cJSON_IsString(j_high)) out.high_temp = j_high->valuestring;
        if (j_low && cJSON_IsString(j_low)) out.low_temp = j_low->valuestring;
        if (j_humidity && cJSON_IsString(j_humidity)) out.humidity = j_humidity->valuestring;
        if (j_wind_scale && cJSON_IsString(j_wind_scale)) out.wind_scale = j_wind_scale->valuestring;
        if (j_precip && cJSON_IsString(j_precip)) out.precip = j_precip->valuestring;
    };

    int raw_count = cJSON_GetArraySize(daily);
    int detail_offset = raw_count >= 4 ? 0 : 1;
    int detail_count = std::min(raw_count, 4 - detail_offset);
    for (int i = 0; i < detail_count; ++i) {
        parse_day(cJSON_GetArrayItem(daily, i), data_.weather_detail[i + detail_offset]);
    }

    // 首页保持今日/明日/后天三列。若接口支持 start=-1，则 detail[1..3] 对应首页；
    // 若接口只返回未来三天，则 detail[0] 为空，detail[1..3] 仍按今日起填充。
    for (int i = 0; i < 3; ++i) {
        data_.weather[i] = data_.weather_detail[i + 1];
        if (data_.weather[i].description.empty() && raw_count > i) {
            parse_day(cJSON_GetArrayItem(daily, i), data_.weather[i]);
        }

        ESP_LOGD(TAG, "天气[%d]: %s %s %s/%s°C humidity=%s wind=%s",
                 i,
                 data_.weather[i].date.c_str(),
                 data_.weather[i].description.c_str(),
                 data_.weather[i].high_temp.c_str(),
                 data_.weather[i].low_temp.c_str(),
                 data_.weather[i].humidity.c_str(),
                 data_.weather[i].wind_scale.c_str());
    }

    WeatherDay cached_yesterday;
    Settings read_settings("setup");
    if (data_.weather_detail[0].description.empty() &&
        ParseWeatherDayJson(read_settings.GetString(WEATHER_LAST_TODAY_KEY), cached_yesterday) &&
        cached_yesterday.date == GetDateOffsetString(-1)) {
        data_.weather_detail[0] = cached_yesterday;
        ESP_LOGI(TAG, "接口未返回昨日天气，使用本地缓存: %s", cached_yesterday.date.c_str());
    }

    if (!data_.weather[0].date.empty() && data_.weather[0].date == GetDateOffsetString(0)) {
        Settings write_settings("setup", true);
        write_settings.SetString(WEATHER_LAST_TODAY_KEY, SerializeWeatherDay(data_.weather[0]));
        ESP_LOGI(TAG, "已保存今日天气到本地，供次日作为昨日天气使用: %s", data_.weather[0].date.c_str());
    }

    cJSON_Delete(root);
    return true;
}

// ============================================================================
// 工具函数
// ============================================================================

int HomeDataStore::DayNameToIndex(const std::string& day_name) {
    // 星期几英文缩写到数组索引的映射
    // mon=0, tue=1, wed=2, thu=3, fri=4
    for (int i = 0; i < 5; ++i) {
        if (day_name == DAY_KEYS[i]) {
            return i;
        }
    }
    return -1;  // 无效输入
}
