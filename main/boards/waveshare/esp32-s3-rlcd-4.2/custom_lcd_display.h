#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <string>
#include <vector>
#include "lcd_display.h"
#include "home_data_store.h"

struct _lv_timer_t;
typedef struct _lv_timer_t lv_timer_t;

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

struct MusicLyricLine {
    int time_ms = -1;
    std::string text;
};

// 反射式 LCD 所需的 SPI 引脚配置
typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    enum class UiPage {
        kBoot,
        kWifiConfig,
        kActivation,
        kHome,
        kMusic,
        kSchedule,
        kWeather,
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    // 这两个成员当前未实际使用，先保留以兼容既有实现。
    uint32_t            i2c_data_pdMS_TICKS = 0;
    uint32_t            i2c_done_pdMS_TICKS = 0;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    // RLCD 的整屏位图缓存，1 bit 表示一个像素的黑白状态。
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
    // 预计算像素坐标到缓冲区字节索引/位掩码的映射，减少刷新时运算量。
	uint16_t (*PixelIndexLUT)[300];
	uint8_t  (*PixelBitLUT  )[300];
	void InitPortraitLUT();
	void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);


private:
    UiPage current_page_ = UiPage::kBoot;
    std::string last_status_text_;
    std::string last_message_text_;
    std::string last_activation_code_;
    std::string shared_chat_text_ = "菲菲: 待命";

    lv_obj_t* boot_page_ = nullptr;
    lv_obj_t* boot_logo_label_ = nullptr;

    lv_obj_t* wifi_config_page_ = nullptr;
    lv_obj_t* wifi_config_top_bar_ = nullptr;
    lv_obj_t* wifi_config_temp_label_ = nullptr;
    lv_obj_t* wifi_config_humidity_label_ = nullptr;
    lv_obj_t* wifi_config_battery_label_ = nullptr;
    lv_obj_t* wifi_config_title_label_ = nullptr;
    lv_obj_t* wifi_config_desc_label_ = nullptr;
    lv_obj_t* wifi_config_ssid_card_ = nullptr;
    lv_obj_t* wifi_config_ssid_caption_label_ = nullptr;
    lv_obj_t* wifi_config_ssid_label_ = nullptr;
    lv_obj_t* wifi_config_url_card_ = nullptr;
    lv_obj_t* wifi_config_url_caption_label_ = nullptr;
    lv_obj_t* wifi_config_url_label_ = nullptr;
    lv_obj_t* wifi_config_hint_label_ = nullptr;

    lv_obj_t* home_page_ = nullptr;
    lv_obj_t* home_top_bar_ = nullptr;
    lv_obj_t* home_top_temp_label_ = nullptr;
    lv_obj_t* home_top_humidity_label_ = nullptr;
    // 主页不显示时间（时间在下方大字显示），所以没有 datetime_label
    lv_obj_t* home_top_wifi_icon_label_ = nullptr;
    lv_obj_t* home_top_battery_label_ = nullptr;

    // 时间条：日期 + 大字时钟
    lv_obj_t* home_date_label_ = nullptr;
    lv_obj_t* home_clock_label_ = nullptr;

    // 三日天气条（每列4行：日期/图标/温度/描述）
    lv_obj_t* home_weather_day0_label_ = nullptr;
    lv_obj_t* home_weather_icon0_label_ = nullptr;
    lv_obj_t* home_weather_temp0_label_ = nullptr;
    lv_obj_t* home_weather_desc0_label_ = nullptr;
    lv_obj_t* home_weather_day1_label_ = nullptr;
    lv_obj_t* home_weather_icon1_label_ = nullptr;
    lv_obj_t* home_weather_temp1_label_ = nullptr;
    lv_obj_t* home_weather_desc1_label_ = nullptr;
    lv_obj_t* home_weather_day2_label_ = nullptr;
    lv_obj_t* home_weather_icon2_label_ = nullptr;
    lv_obj_t* home_weather_temp2_label_ = nullptr;
    lv_obj_t* home_weather_desc2_label_ = nullptr;

    // 今日/明日课程（标题14号，内容16号）
    lv_obj_t* home_today_title_label_ = nullptr;
    lv_obj_t* home_today_courses_label_ = nullptr;
    lv_obj_t* home_tomorrow_title_label_ = nullptr;
    lv_obj_t* home_tomorrow_courses_label_ = nullptr;

    // 底部聊天条
    lv_obj_t* home_bottom_status_label_ = nullptr;

    HomeDataStore* home_data_store_ = nullptr;
    lv_timer_t* home_refresh_timer_ = nullptr;

    // 音乐播放器页面
    lv_obj_t* music_page_ = nullptr;
    lv_obj_t* music_top_bar_ = nullptr;
    lv_obj_t* music_temp_label_ = nullptr;
    lv_obj_t* music_humidity_label_ = nullptr;
    lv_obj_t* music_datetime_label_ = nullptr;
    lv_obj_t* music_wifi_icon_label_ = nullptr;
    lv_obj_t* music_battery_label_ = nullptr;
    lv_obj_t* music_title_label_ = nullptr;
    lv_obj_t* music_artist_label_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* music_progress_fill_ = nullptr;
    lv_obj_t* music_time_cur_label_ = nullptr;
    lv_obj_t* music_time_total_label_ = nullptr;
    lv_obj_t* music_play_icon_label_ = nullptr;
    lv_obj_t* music_lyrics_label_ = nullptr;
    lv_obj_t* music_chat_label_ = nullptr;
    lv_timer_t* music_chat_hide_timer_ = nullptr;
    lv_timer_t* music_mock_timer_ = nullptr;
    std::string music_title_text_;
    std::string music_artist_text_;
    std::string music_album_text_;
    std::string music_lyric_text_;
    std::vector<MusicLyricLine> music_lyric_lines_;
    std::string music_playback_state_;
    int music_position_ms_ = 0;
    int music_duration_ms_ = 0;

    // 课程表页面
    lv_obj_t* schedule_page_ = nullptr;
    lv_obj_t* schedule_top_bar_ = nullptr;
    lv_obj_t* schedule_temp_label_ = nullptr;
    lv_obj_t* schedule_humidity_label_ = nullptr;
    lv_obj_t* schedule_datetime_label_ = nullptr;
    lv_obj_t* schedule_wifi_icon_label_ = nullptr;
    lv_obj_t* schedule_battery_label_ = nullptr;
    lv_obj_t* schedule_single_label_ = nullptr;
    lv_obj_t* schedule_dual_label_ = nullptr;
    lv_obj_t* schedule_range_label_ = nullptr;
    lv_obj_t* schedule_table_ = nullptr;
    lv_obj_t* schedule_empty_label_ = nullptr;
    lv_obj_t* schedule_course_rows_[8] = {};
    lv_obj_t* schedule_course_labels_[8][5] = {};
    lv_obj_t* schedule_chat_label_ = nullptr;
    lv_timer_t* schedule_chat_hide_timer_ = nullptr;
    bool schedule_show_dual_ = false;
    bool schedule_week_manual_ = false;

    // 天气详情页面
    lv_obj_t* weather_page_ = nullptr;
    lv_obj_t* weather_top_bar_ = nullptr;
    lv_obj_t* weather_temp_label_ = nullptr;
    lv_obj_t* weather_humidity_label_ = nullptr;
    lv_obj_t* weather_datetime_label_ = nullptr;
    lv_obj_t* weather_wifi_icon_label_ = nullptr;
    lv_obj_t* weather_battery_label_ = nullptr;
    lv_obj_t* weather_day_labels_[4] = {};
    lv_obj_t* weather_icon_labels_[4] = {};
    lv_obj_t* weather_temp_range_labels_[4] = {};
    lv_obj_t* weather_desc_labels_[4] = {};
    lv_obj_t* weather_chart_area_ = nullptr;
    lv_obj_t* weather_high_line_ = nullptr;
    lv_obj_t* weather_low_line_ = nullptr;
    lv_point_precise_t weather_high_points_[4] = {};
    lv_point_precise_t weather_low_points_[4] = {};
    lv_obj_t* weather_high_value_labels_[4] = {};
    lv_obj_t* weather_low_value_labels_[4] = {};
    lv_obj_t* weather_city_label_ = nullptr;
    lv_obj_t* weather_metric_value_labels_[3] = {};
    lv_obj_t* weather_metric_labels_[3] = {};
    lv_obj_t* weather_chat_label_ = nullptr;

    lv_obj_t* activation_page_ = nullptr;
    lv_obj_t* activation_top_bar_ = nullptr;
    lv_obj_t* activation_temp_label_ = nullptr;
    lv_obj_t* activation_humidity_label_ = nullptr;
    lv_obj_t* activation_datetime_label_ = nullptr;
    lv_obj_t* activation_battery_label_ = nullptr;
    lv_obj_t* activation_title_label_ = nullptr;
    lv_obj_t* activation_code_card_ = nullptr;
    lv_obj_t* activation_code_caption_label_ = nullptr;
    lv_obj_t* activation_code_label_ = nullptr;
    lv_obj_t* activation_hint_label_ = nullptr;

    lv_timer_t* top_bar_timer_ = nullptr;

    lv_obj_t* CreateFullScreenPage(lv_obj_t* screen);
    static void TopBarTimerCb(lv_timer_t* timer);
    lv_obj_t* CreateTopBar(lv_obj_t* parent,
                           lv_obj_t** out_temp_label,
                           lv_obj_t** out_humidity_label,
                           lv_obj_t** out_datetime_label,
                           lv_obj_t** out_wifi_icon_label,
                           lv_obj_t** out_battery_label,
                           bool show_datetime,
                           bool show_wifi_icon,
                           bool show_battery);
    void CreateBootPage(lv_obj_t* screen);
    void CreateWifiConfigPage(lv_obj_t* screen);
    void CreateActivationPage(lv_obj_t* screen);
    void CreateHomePage(lv_obj_t* screen);
    void CreateMusicPage(lv_obj_t* screen);
    void CreateSchedulePage(lv_obj_t* screen);
    void CreateWeatherPage(lv_obj_t* screen);
    void SwitchPage(UiPage page);
    void UpdateWifiConfigMessage(const char* message);
    void UpdateTopBar(lv_obj_t* temp_label, lv_obj_t* humidity_label, lv_obj_t* datetime_label,
                      lv_obj_t* wifi_icon, lv_obj_t* battery_label);
    void UpdateWifiConfigPage();
    void UpdateActivationCode(const char* code);
    void UpdateHomeStatus(const char* status);
    void SetSharedStatus(const char* status);
    void SetSharedChatMessage(const char* role, const char* content);
    void SetSharedChatText(const std::string& text);
    void SyncSharedChatLabels();
    void UpdateHomePage();
    static void HomeRefreshTimerCb(lv_timer_t* timer);
    void UpdateMusicPage();
    void ResetMusicPage();
    std::string BuildMusicLyricsWindow() const;
    void UpdateMusicFromMessage(const char* role, const char* content);
    void ShowMusicChatMessage(const char* role, const char* content);
    static void MusicChatHideTimerCb(lv_timer_t* timer);
    static void MusicMockTimerCb(lv_timer_t* timer);
    void UpdateSchedulePage();
    void ShowScheduleChatMessage(const char* role, const char* content);
    static void ScheduleChatHideTimerCb(lv_timer_t* timer);
    void UpdateWeatherPage();

public:
    void SetupUI() override;
    void CyclePage();  // 页面循环：Home→Music→Schedule→Weather→Home
    bool HandleKeyLongPress();
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;


public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,spi_display_config_t spiconfig,spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
	void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};


#endif
