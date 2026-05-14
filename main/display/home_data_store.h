/**
 * @file home_data_store.h
 * @brief 首页数据层 —— 课程表 + 天气信息的统一数据源
 *
 * 本模块负责：
 * 1. 从 NVS 中读取课程表配置（学期起止日期、单双周课程 JSON）
 * 2. 从 NVS 中读取天气 API 配置（心和天气 Key、城市）
 * 3. 解析课程表 JSON，根据当前日期确定单/双周，推算今日/明日课程
 * 4. 通过 HTTP 调用心和天气 API 获取 3 日天气预报，并缓存结果
 * 5. 对外提供统一的 HomeData 结构，供主页 LVGL 布局使用
 *
 * @note  本模块运行在 ESP32-S3 (ESP-IDF 5.5.4) 环境中，需注意：
 *        - 内存有限，使用 std::vector 而非 std::map 以节省开销
 *        - 天气数据缓存周期为 1 小时，开机获取后按小时轮询
 *        - 课程表仅在 NVS 数据变更时重新加载，运行时保持静态
 *
 * @par NVS 键名映射（namespace: "setup"）
 * | 键名             | 类型   | 说明                         |
 * |------------------|--------|------------------------------|
 * | sched_start      | string | 学期开始日期 YYYY-MM-DD      |
 * | sched_end        | string | 学期结束日期 YYYY-MM-DD      |
 * | sched_single     | string | 单周课程 JSON                |
 * | sched_double     | string | 双周课程 JSON                |
 * | w_key            | string | 心和天气 API Key             |
 * | w_city           | string | 城市名（如 "shenzhen"）      |
 *
 * @par 课程 JSON 格式示例
 * @code
 * {
 *   "mon_1": "语文", "mon_2": "数学", ..., "mon_8": "自习",
 *   "tue_1": "数学", ..., "fri_8": "劳动"
 * }
 * @endcode
 * 键名规则：{day}_{period}，day ∈ {mon,tue,wed,thu,fri}，period ∈ {1..8}
 */

#ifndef HOME_DATA_STORE_H
#define HOME_DATA_STORE_H

#include <string>
#include <vector>
#include <ctime>
#include <cstdint>

// 前向声明，避免在头文件中引入 cJSON.h
struct cJSON;

/**
 * @brief 单节课程信息
 *
 * 表示课程表中的一节课，包含节次编号和课程名称。
 * 节次编号 1-4 为上午，5-8 为下午。
 */
struct CourseInfo {
    int         period;  ///< 节次编号 (1-8)，1-4 上午，5-8 下午
    std::string name;    ///< 课程名称，如 "语文"、"数学"
};

/**
 * @brief 一天的课程安排
 *
 * 包含星期名称和当天所有有课的课程列表。
 * 无课的节次不会出现在 courses 向量中。
 */
struct DaySchedule {
    std::string            day_name;  ///< 星期名称，如 "周一"、"周二"
    std::vector<CourseInfo> courses;   ///< 当天课程列表（仅包含有课的节次）
};

/**
 * @brief 单日天气预报信息
 *
 * 对应心和天气 API 返回的 daily 数组中的一项。
 */
struct WeatherDay {
    std::string date;         ///< 日期，格式 "YYYY-MM-DD"
    std::string high_temp;    ///< 最高温度，如 "22"
    std::string low_temp;     ///< 最低温度，如 "14"
    std::string description;  ///< 天气描述，如 "晴"、"多云"、"小雨"
    std::string humidity;     ///< 湿度，如 "65"
    std::string wind_scale;   ///< 风力等级，如 "3"
    std::string precip;       ///< 降水概率，0-100；接口不返回时为空
};

/**
 * @brief 首页完整数据结构
 *
 * 聚合了主页 LVGL 布局所需的全部数据：今日/明日课程 + 3 日天气。
 * 由 HomeDataStore::GetHomeData() 返回，供 UI 层直接使用。
 */
struct HomeData {
    DaySchedule  today_schedule;      ///< 今日课程（周末时 courses 为空）
    DaySchedule  tomorrow_schedule;   ///< 明日课程（周末时 courses 为空）
    WeatherDay   weather[3];          ///< 今日、明日、后天天气预报
    WeatherDay   weather_detail[4];   ///< 昨日、今日、明日、后天天气详情
    std::string  weather_city;        ///< 天气城市名称
    time_t       last_weather_update; ///< 上次天气更新的 Unix 时间戳 (0 = 从未更新)
    bool         has_schedule;        ///< 是否已成功加载课程表配置
    bool         schedule_expired;     ///< 当前日期是否已超过课程表结束日期
    bool         has_weather_config;  ///< 是否已配置天气 API
};

/**
 * @brief 首页数据存储与服务类
 *
 * 单例模式（可选），统一管理课程表解析和天气数据获取。
 * 外部通过 LoadFromNvs() 初始化，然后调用 GetHomeData() 获取当前数据。
 * 天气数据通过 RefreshWeather() 手动触发刷新（内部按 1 小时缓存）。
 */
class HomeDataStore {
public:
    HomeDataStore();
    ~HomeDataStore();

    /**
     * @brief 从 NVS 加载课程表和天气配置
     *
     * 读取 namespace "setup" 下的课程表和天气相关键值。
     * 成功后 has_schedule / has_weather_config 标志位会被设置。
     * 可重复调用以重新加载（例如用户修改配置后）。
     *
     * @return true  至少课程表或天气配置之一加载成功
     * @return false 全部加载失败（NVS 中无数据）
     */
    bool LoadFromNvs();

    /**
     * @brief 根据当前时间更新今日/明日课程
     *
     * 根据系统时钟获取当前日期，判断单/双周，推算今日和明日的课程列表。
     * 建议在每分钟定时器中调用，或在页面显示时调用。
     *
     * @return 更新后的 HomeData 引用
     */
    const HomeData& UpdateSchedule();

    /**
     * @brief 从心和天气 API 获取天气数据
     *
     * 发起 HTTP GET 请求获取 3 日天气预报。如果距上次更新不足 1 小时，
     * 则跳过请求直接返回缓存数据，避免频繁调用浪费流量。
     *
     * @return true  天气数据更新成功或仍在缓存有效期内
     * @return false 请求失败且无缓存数据可用
     */
    bool RefreshWeather();

    /**
     * @brief 获取当前首页数据（只读）
     *
     * @return const HomeData& 当前缓存的首页数据
     */
    const HomeData& GetHomeData() const { return data_; }

    /**
     * @brief 检查课程表是否已加载
     */
    bool HasSchedule() const { return data_.has_schedule; }

    /**
     * @brief 检查天气配置是否存在
     */
    bool HasWeatherConfig() const { return data_.has_weather_config; }

    /**
     * @brief 获取单周或双周的整周课程表（周一到周五）
     */
    const DaySchedule* GetWeekSchedule(bool dual_week) const {
        return dual_week ? dual_week_ : single_week_;
    }

    /**
     * @brief 根据当前日期判断当前应显示双周还是单周
     */
    bool IsCurrentDualWeek() const {
        return IsDualWeek(time(nullptr));
    }

    /**
     * @brief 计算指定日期属于单周还是双周
     *
     * 算法：计算从学期开始日期到指定日期的天数差 N，
     *       week_index = N / 7，week_index % 2 == 0 为单周，否则为双周。
     *       （开学第一周为单周，即 week_index = 0）
     *
     * @param[in] target_date  目标日期 (Unix 时间戳)
     * @return true  双周
     * @return false 单周
     */
    bool IsDualWeek(time_t target_date) const;

private:
    // ========== 课程表解析相关 ==========

    /**
     * @brief 解析课程表 JSON 字符串
     *
     * 将 JSON 格式的课程数据解析为内部的 WeekSchedule 结构。
     * JSON 格式：{"mon_1": "语文", "tue_2": "数学", ...}
     *
     * @param[in]  json_str   JSON 字符串
     * @param[out] out_schedule 输出的 5 天课程数组（周一到周五）
     * @return true  解析成功
     * @return false JSON 格式错误或为空
     */
    bool ParseScheduleJson(const char* json_str, DaySchedule out_schedule[5]);

    /**
     * @brief 从星期几名称映射到数组索引
     *
     * mon=0, tue=1, wed=2, thu=3, fri=4
     *
     * @param day_name 星期几的英文缩写（小写）
     * @return 索引值 (0-4)，无效输入返回 -1
     */
    static int DayNameToIndex(const std::string& day_name);

    /**
     * @brief 从 NVS 读取课程表配置
     *
     * 读取 sched_start、sched_end、sched_single、sched_double 四个键。
     *
     * @return true  读取成功
     * @return false 关键键缺失
     */
    bool LoadScheduleFromNvs();
    bool IsScheduleExpired(time_t target_date) const;

    /**
     * @brief 从 NVS 读取天气 API 配置
     *
     * 读取 w_key、w_city 两个键。
     *
     * @return true  读取成功
     * @return false 关键键缺失
     */
    bool LoadWeatherConfigFromNvs();
    bool LoadWeatherCacheFromNvs();
    void SaveWeatherCacheToNvs();

    // ========== 天气 HTTP 请求相关 ==========

    /**
     * @brief 执行 HTTP GET 请求获取天气数据
     *
     * 拼接心和天气 API URL，发起请求，解析 JSON 响应。
     *
     * @return true  请求成功，数据已更新
     * @return false 请求失败
     */
    bool FetchWeatherFromApi();

    /**
     * @brief 解析天气 API 的 JSON 响应
     *
     * @param json_str 响应 JSON 字符串
     * @return true  解析成功
     * @return false JSON 格式错误
     */
    bool ParseWeatherResponse(const char* json_str);

    // ========== 内部数据 ==========

    /** @brief 一周七天的中文名称（用于 UI 显示） */
    static constexpr const char* DAY_NAMES[] = {
        "周一", "周二", "周三", "周四", "周五", "周六", "周日"
    };

    /** @brief 星期几的英文缩写（用于 JSON 键名匹配） */
    static constexpr const char* DAY_KEYS[] = {
        "mon", "tue", "wed", "thu", "fri"
    };

    /** @brief 天气数据缓存有效期（秒），默认 1 小时 */
    static constexpr int WEATHER_CACHE_SECONDS = 60 * 60;

    /** @brief HTTP 响应缓冲区大小 */
    static constexpr int HTTP_RESPONSE_BUF_SIZE = 2048;

    // NVS 配置数据
    std::string semester_start_;     ///< 学期开始日期 "YYYY-MM-DD"
    std::string semester_end_;       ///< 学期结束日期 "YYYY-MM-DD"
    std::string initial_week_type_;  ///< 配网时设置的初始周类型 "single" 或 "dual"
    std::string weather_key_;        ///< 心和天气 API Key
    std::string weather_city_;       ///< 城市名

    // 解析后的课程表（单周和双周各 5 天）
    DaySchedule single_week_[5];     ///< 单周课程（周一到周五）
    DaySchedule dual_week_[5];       ///< 双周课程（周一到周五）
    bool        schedule_parsed_;    ///< 课程表是否已成功解析
    std::string schedule_json_cache_; ///< 上次成功加载的课程表原始 JSON，用于检测浏览器修改
    bool        weather_loaded_from_cache_ = false; ///< 本次启动是否已先显示本地天气缓存

    // 对外输出的首页数据
    HomeData data_;

    // HTTP 响应临时缓冲区（避免频繁分配）
    std::vector<char> http_buf_;
};

#endif // HOME_DATA_STORE_H
