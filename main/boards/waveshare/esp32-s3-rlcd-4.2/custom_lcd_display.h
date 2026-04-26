#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include "lcd_display.h"

struct _lv_timer_t;
typedef struct _lv_timer_t lv_timer_t;

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
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
    lv_obj_t* home_title_label_ = nullptr;
    lv_obj_t* home_status_label_ = nullptr;

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
    void SwitchPage(UiPage page);
    void UpdateWifiConfigMessage(const char* message);
    void UpdateTopBar(lv_obj_t* temp_label, lv_obj_t* humidity_label, lv_obj_t* datetime_label, lv_obj_t* battery_label);
    void UpdateWifiConfigPage();
    void UpdateActivationCode(const char* code);
    void UpdateHomeStatus(const char* status);

public:
    void SetupUI() override;
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
