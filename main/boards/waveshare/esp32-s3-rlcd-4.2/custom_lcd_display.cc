#include <vector>
#include <cstring>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "home_data_store.h"

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
    // LVGL 输出的是 RGB565，这里按阈值转换成 RLCD 使用的黑白位图格式。
 	for(int y = area->y1; y <= area->y2; y++)
 	{
 	 	for(int x = area->x1; x <= area->x2; x++) 
 	 	{
 	 	  	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
 	 	  	Disp->RLCD_SetPixel(x,y,color);
 	 	  	buffer++;
 	 	}
 	}
 	Disp->RLCD_Display();
	lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
esp_lcd_panel_handle_t panel,
int width, 
int height, 
int offset_x, 
int offset_y,
bool mirror_x, 
bool mirror_y, 
bool swap_xy,
spi_display_config_t spiconfig,
spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
mosi_(spiconfig.mosi),
scl_(spiconfig.scl), 
dc_(spiconfig.dc), 
cs_(spiconfig.cs), 
rst_(spiconfig.rst), 
width_(width), 
height_(height)
{
	ESP_LOGI(TAG, "Initialize SPI");
	esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi_;
    buscfg.sclk_io_num                   = scl_;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = dc_;
    io_config.cs_gpio_num = cs_;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 7;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    // RLCD 采用 1bit 黑白像素格式，因此每 8 个像素占 1 字节。
    DisplayLen                = transfer >> 3;
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    // 根据屏幕方向提前生成像素查找表，刷新时直接定位到目标字节和位。
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 500;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 按水平和垂直像素分辨率完成显示对象基础初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_FULL);

    ESP_LOGI(TAG, "RLCD init");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // 注意：SetupUI() 不应在构造函数内调用，而应由 Application::Initialize() 调用，
    // 这样可以确保 LVGL 对象是在显示硬件和底层缓冲都初始化完成后再创建。
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (top_bar_timer_ != nullptr) {
        if (Lock(30000)) {
            lv_timer_del(top_bar_timer_);
            top_bar_timer_ = nullptr;
            Unlock();
        }
    }
    if (home_refresh_timer_ != nullptr) {
        if (Lock(30000)) {
            lv_timer_del(home_refresh_timer_);
            home_refresh_timer_ = nullptr;
            Unlock();
        }
    }
    delete home_data_store_;
}

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            // 竖屏模式下，显存按 4x2 像素块编码。
            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            // 横屏模式下，显存按 2x4 像素块编码，并对 Y 方向做翻转适配。
            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM 装载控制
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); // 使能升压电路
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); // 栅极电压控制
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); // VSHP 电压设置
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // 设置列地址范围
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // 设置页地址范围
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // 开始写入显存

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}


LV_FONT_DECLARE(taobao_logo_font_60);
LV_FONT_DECLARE(alibaba_puhui_title_24);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_14);
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(font_awesome_16_4);
#include "font_awesome.h"

/**
 * @brief 天气描述 → Font Awesome 图标映射
 */
static const char* get_weather_icon(const std::string& desc) {
    if (desc.empty()) return "";
    if (desc.find("晴") != std::string::npos) return FONT_AWESOME_SUN;
    if (desc.find("多云") != std::string::npos) return FONT_AWESOME_CLOUD_SUN;
    if (desc.find("阴") != std::string::npos) return FONT_AWESOME_CLOUD;
    if (desc.find("大雨") != std::string::npos || desc.find("暴雨") != std::string::npos) return FONT_AWESOME_CLOUD_SHOWERS_HEAVY;
    if (desc.find("雨") != std::string::npos || desc.find("阵雨") != std::string::npos) return FONT_AWESOME_CLOUD_RAIN;
    if (desc.find("雷") != std::string::npos) return FONT_AWESOME_CLOUD_BOLT;
    if (desc.find("雪") != std::string::npos) return FONT_AWESOME_SNOWFLAKE;
    if (desc.find("雾") != std::string::npos || desc.find("霾") != std::string::npos) return FONT_AWESOME_SMOG;
    return FONT_AWESOME_SUN;  // 默认晴天
}

lv_obj_t* CustomLcdDisplay::CreateFullScreenPage(lv_obj_t* screen) {
    auto* page = lv_obj_create(screen);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    return page;
}

//设置初始化页面
void CustomLcdDisplay::CreateBootPage(lv_obj_t* screen) {
    boot_page_ = CreateFullScreenPage(screen);

    boot_logo_label_ = lv_label_create(boot_page_);
    lv_label_set_text(boot_logo_label_, "苗苗最棒");
    lv_obj_set_style_text_font(boot_logo_label_, &taobao_logo_font_60, 0);
    lv_obj_set_style_text_color(boot_logo_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(boot_logo_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(boot_logo_label_);
}

void CustomLcdDisplay::CreateWifiConfigPage(lv_obj_t* screen) {
    // 配网模式页（参照 docs/v2/prototype/ui-r2/setup-start.html）：
    // - 顶部信息栏：显示温度/湿度（当前板级只接了温度，湿度先占位）
    // - 主内容区：标题、两块信息卡片（WiFi 名称、访问 URL）、底部提示文案
    wifi_config_page_ = CreateFullScreenPage(screen);

    // 首次配网页：顶栏不展示 WiFi 图标，电池保留占位
    wifi_config_top_bar_ = CreateTopBar(
        wifi_config_page_,
        &wifi_config_temp_label_,
        &wifi_config_humidity_label_,
        nullptr,
        nullptr,
        &wifi_config_battery_label_,
        false,
        false,
        true);

    // 主内容区：从 y=28 开始，占满剩余高度，内部用 flex 纵向排版
    auto* main_area = lv_obj_create(wifi_config_page_);
    lv_obj_set_size(main_area, LV_HOR_RES, LV_VER_RES - 28);
    lv_obj_align(main_area, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_bg_color(main_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_left(main_area, 14, 0);
    lv_obj_set_style_pad_right(main_area, 14, 0);
    lv_obj_set_style_pad_top(main_area, 8, 0);
    lv_obj_set_style_pad_bottom(main_area, 0, 0);
    lv_obj_set_style_pad_row(main_area, 8, 0);
    lv_obj_set_scrollbar_mode(main_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 标题：固定文案“首次配网”，用 title_24 字体
    wifi_config_title_label_ = lv_label_create(main_area);
    lv_label_set_text(wifi_config_title_label_, "首次配网");
    lv_obj_set_style_text_font(wifi_config_title_label_, &alibaba_puhui_title_24, 0);
    lv_obj_set_style_text_color(wifi_config_title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(wifi_config_title_label_, 6, 0);

    // 说明：一行指引文案（用正文字体 16）
    wifi_config_desc_label_ = lv_label_create(main_area);
    lv_obj_set_width(wifi_config_desc_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(wifi_config_desc_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(wifi_config_desc_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_desc_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_config_desc_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(wifi_config_desc_label_, "");
    lv_obj_add_flag(wifi_config_desc_label_, LV_OBJ_FLAG_HIDDEN);

    // 齿轮图标：放在“首次配网”标题上方（使用 LVGL 内置符号，避免依赖中文字库）
    lv_obj_t* gear_label = lv_label_create(main_area);
    lv_obj_set_style_text_font(gear_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(gear_label, lv_color_black(), 0);
    lv_obj_set_style_text_align(gear_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_zoom(gear_label, 512, 0);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);

    lv_obj_move_to_index(gear_label, 0);
    lv_obj_move_to_index(wifi_config_title_label_, 1);

    // 卡片 1：WiFi 名称（caption + value）
    wifi_config_ssid_card_ = lv_obj_create(main_area);
    lv_obj_set_width(wifi_config_ssid_card_, LV_HOR_RES - 28);
    lv_obj_set_height(wifi_config_ssid_card_, 52);
    lv_obj_set_style_bg_color(wifi_config_ssid_card_, lv_color_white(), 0);
    lv_obj_set_style_border_width(wifi_config_ssid_card_, 2, 0);
    lv_obj_set_style_border_color(wifi_config_ssid_card_, lv_color_black(), 0);
    lv_obj_set_style_pad_left(wifi_config_ssid_card_, 10, 0);
    lv_obj_set_style_pad_right(wifi_config_ssid_card_, 10, 0);
    lv_obj_set_style_pad_top(wifi_config_ssid_card_, 6, 0);
    lv_obj_set_style_pad_bottom(wifi_config_ssid_card_, 6, 0);
    lv_obj_set_style_pad_row(wifi_config_ssid_card_, 2, 0);
    lv_obj_set_scrollbar_mode(wifi_config_ssid_card_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(wifi_config_ssid_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_config_ssid_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    wifi_config_ssid_caption_label_ = lv_label_create(wifi_config_ssid_card_);
    lv_obj_set_width(wifi_config_ssid_caption_label_, LV_PCT(100));
    lv_obj_set_style_text_font(wifi_config_ssid_caption_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(wifi_config_ssid_caption_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_ssid_caption_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wifi_config_ssid_caption_label_, "WiFi名称");

    wifi_config_ssid_label_ = lv_label_create(wifi_config_ssid_card_);
    lv_obj_set_width(wifi_config_ssid_label_, LV_PCT(100));
    lv_obj_set_style_text_font(wifi_config_ssid_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(wifi_config_ssid_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_ssid_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_config_ssid_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(wifi_config_ssid_label_, "waiting");

    // 卡片 2：访问地址（caption + value）
    wifi_config_url_card_ = lv_obj_create(main_area);
    lv_obj_set_width(wifi_config_url_card_, LV_HOR_RES - 28);
    lv_obj_set_height(wifi_config_url_card_, 52);
    lv_obj_set_style_bg_color(wifi_config_url_card_, lv_color_white(), 0);
    lv_obj_set_style_border_width(wifi_config_url_card_, 2, 0);
    lv_obj_set_style_border_color(wifi_config_url_card_, lv_color_black(), 0);
    lv_obj_set_style_pad_left(wifi_config_url_card_, 10, 0);
    lv_obj_set_style_pad_right(wifi_config_url_card_, 10, 0);
    lv_obj_set_style_pad_top(wifi_config_url_card_, 6, 0);
    lv_obj_set_style_pad_bottom(wifi_config_url_card_, 6, 0);
    lv_obj_set_style_pad_row(wifi_config_url_card_, 2, 0);
    lv_obj_set_scrollbar_mode(wifi_config_url_card_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(wifi_config_url_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_config_url_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    wifi_config_url_caption_label_ = lv_label_create(wifi_config_url_card_);
    lv_obj_set_width(wifi_config_url_caption_label_, LV_PCT(100));
    lv_obj_set_style_text_font(wifi_config_url_caption_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(wifi_config_url_caption_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_url_caption_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wifi_config_url_caption_label_, "访问地址");

    wifi_config_url_label_ = lv_label_create(wifi_config_url_card_);
    lv_obj_set_width(wifi_config_url_label_, LV_PCT(100));
    lv_obj_set_style_text_font(wifi_config_url_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(wifi_config_url_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_url_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_config_url_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(wifi_config_url_label_, "waiting");

    // 底部提示：用于说明“先连 WiFi，再访问 URL”
    wifi_config_hint_label_ = lv_label_create(main_area);
    lv_obj_set_width(wifi_config_hint_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(wifi_config_hint_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(wifi_config_hint_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(wifi_config_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_config_hint_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(wifi_config_hint_label_, "请先连接上方热点，再在浏览器中打开访问地址完成配置");

    UpdateTopBar(wifi_config_temp_label_, wifi_config_humidity_label_, nullptr, wifi_config_battery_label_);

    lv_obj_add_flag(wifi_config_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::CreateHomePage(lv_obj_t* screen) {
    // ========================================================================
    // 主页布局（参照 docs/v2/prototype/ui-r2/device-home.html）
    // ┌──────────────────────────────────────┐
    // │ 1月2日 12:33          WiFi  电池 82% │  <- 顶部栏 (28px)
    // ├──────────────────────────────────────┤
    // │ 今天 周四                            │  <- 时间条
    // │ 12:33                               │
    // ├──────────────────────────────────────┤
    // │ 今天 晴 22/14 │明天 多云│后天 小雨   │  <- 三日天气条
    // ├──────────────────────────────────────┤
    // │ 今日课程                             │  <- 课程区
    // │ 1.语文 2.数学 3.英语 4.科学          │
    // │ 明日课程                             │
    // │ 1.数学 2.语文 ...                    │
    // ├──────────────────────────────────────┤
    // │ 小智: 待命                           │  <- 底部状态条
    // └──────────────────────────────────────┘
    // ========================================================================

    home_page_ = CreateFullScreenPage(screen);

    // —— 初始化 HomeDataStore 并加载 NVS 数据 ——
    if (home_data_store_ == nullptr) {
        home_data_store_ = new HomeDataStore();
        home_data_store_->LoadFromNvs();
    }

    // ====================== 顶部栏（28px）======================
    // 主页的顶部栏不显示时间（时间在下方大字显示），只显示温湿度/WiFi/电池
    home_top_bar_ = CreateTopBar(
        home_page_,
        &home_top_temp_label_,
        &home_top_humidity_label_,
        nullptr,  // 主页不显示时间（时间在下方大字显示）
        &home_top_wifi_icon_label_,
        &home_top_battery_label_,
        false,    // 不显示日期时间
        true,     // 显示 WiFi 图标
        true);    // 显示电池

    // ====================== 时间条（日期 + 大字时钟）======================
    // 主内容区从 y=28 开始，使用 flex 纵向排版
    auto* main_area = lv_obj_create(home_page_);
    lv_obj_set_size(main_area, LV_HOR_RES, LV_VER_RES - 28);
    lv_obj_align(main_area, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_bg_color(main_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);
    lv_obj_set_scrollbar_mode(main_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_area, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // —— 时间条容器（日期左对齐、时间居中）——
    auto* time_strip = lv_obj_create(main_area);
    lv_obj_set_size(time_strip, LV_HOR_RES, 44);
    lv_obj_set_style_radius(time_strip, 0, 0);
    lv_obj_set_style_bg_color(time_strip, lv_color_white(), 0);
    lv_obj_set_style_border_width(time_strip, 1, 0);
    lv_obj_set_style_border_side(time_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(time_strip, lv_color_black(), 0);
    lv_obj_set_style_pad_left(time_strip, 12, 0);
    lv_obj_set_style_pad_right(time_strip, 12, 0);
    lv_obj_set_style_pad_top(time_strip, 6, 0);
    lv_obj_set_style_pad_bottom(time_strip, 6, 0);
    lv_obj_set_scrollbar_mode(time_strip, LV_SCROLLBAR_MODE_OFF);

    // 日期：左对齐
    home_date_label_ = lv_label_create(time_strip);
    lv_obj_set_style_text_font(home_date_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(home_date_label_, lv_color_black(), 0);
    lv_label_set_text(home_date_label_, "---- --");
    lv_obj_align(home_date_label_, LV_ALIGN_LEFT_MID, 0, 0);

    // 大字时钟：居中
    home_clock_label_ = lv_label_create(time_strip);
    lv_obj_set_style_text_font(home_clock_label_, &alibaba_puhui_title_24, 0);
    lv_obj_set_style_text_color(home_clock_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(home_clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(home_clock_label_, "--:--");
    lv_obj_align(home_clock_label_, LV_ALIGN_CENTER, 0, 0);

    // ====================== 三日天气条（4行/列：日期+图标+温度+描述）======================
    auto* weather_strip = lv_obj_create(main_area);
    lv_obj_set_size(weather_strip, LV_HOR_RES, 76);
    lv_obj_set_style_radius(weather_strip, 0, 0);
    lv_obj_set_style_bg_color(weather_strip, lv_color_white(), 0);
    lv_obj_set_style_border_width(weather_strip, 1, 0);
    lv_obj_set_style_border_side(weather_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(weather_strip, lv_color_black(), 0);
    lv_obj_set_style_pad_left(weather_strip, 2, 0);
    lv_obj_set_style_pad_right(weather_strip, 2, 0);
    lv_obj_set_style_pad_top(weather_strip, 2, 0);
    lv_obj_set_style_pad_bottom(weather_strip, 2, 0);
    lv_obj_set_scrollbar_mode(weather_strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(weather_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 今天天气列
    {
        auto* col0 = lv_obj_create(weather_strip);
        lv_obj_set_width(col0, LV_PCT(30));
        lv_obj_set_flex_grow(col0, 1);
        lv_obj_set_style_radius(col0, 0, 0);
        lv_obj_set_style_bg_color(col0, lv_color_white(), 0);
        lv_obj_set_style_border_width(col0, 0, 0);
        lv_obj_set_style_pad_all(col0, 1, 0);
        lv_obj_set_style_pad_row(col0, 0, 0);
        lv_obj_set_scrollbar_mode(col0, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(col0, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col0, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        home_weather_day0_label_ = lv_label_create(col0);
        lv_obj_set_style_text_font(home_weather_day0_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_day0_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_day0_label_, "今天");

        home_weather_icon0_label_ = lv_label_create(col0);
        lv_obj_set_style_text_font(home_weather_icon0_label_, &font_awesome_16_4, 0);
        lv_obj_set_style_text_color(home_weather_icon0_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_icon0_label_, "");

        home_weather_temp0_label_ = lv_label_create(col0);
        lv_obj_set_style_text_font(home_weather_temp0_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_temp0_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_temp0_label_, "--°C");

        home_weather_desc0_label_ = lv_label_create(col0);
        lv_obj_set_style_text_font(home_weather_desc0_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_desc0_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_desc0_label_, "--");
    }
    {
        // 列分隔线 1
        auto* wsep1 = lv_obj_create(weather_strip);
        lv_obj_set_size(wsep1, 1, 78);
        lv_obj_set_style_radius(wsep1, 0, 0);
        lv_obj_set_style_bg_color(wsep1, lv_color_black(), 0);
        lv_obj_set_style_border_width(wsep1, 0, 0);
        lv_obj_set_style_pad_all(wsep1, 0, 0);
    }
    // 明天天气列
    {
        auto* col1 = lv_obj_create(weather_strip);
        lv_obj_set_width(col1, LV_PCT(30));
        lv_obj_set_flex_grow(col1, 1);
        lv_obj_set_style_radius(col1, 0, 0);
        lv_obj_set_style_bg_color(col1, lv_color_white(), 0);
        lv_obj_set_style_border_width(col1, 0, 0);
        lv_obj_set_style_pad_all(col1, 1, 0);
        lv_obj_set_style_pad_row(col1, 0, 0);
        lv_obj_set_scrollbar_mode(col1, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(col1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        home_weather_day1_label_ = lv_label_create(col1);
        lv_obj_set_style_text_font(home_weather_day1_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_day1_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_day1_label_, "明天");

        home_weather_icon1_label_ = lv_label_create(col1);
        lv_obj_set_style_text_font(home_weather_icon1_label_, &font_awesome_16_4, 0);
        lv_obj_set_style_text_color(home_weather_icon1_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_icon1_label_, "");

        home_weather_temp1_label_ = lv_label_create(col1);
        lv_obj_set_style_text_font(home_weather_temp1_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_temp1_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_temp1_label_, "--°C");

        home_weather_desc1_label_ = lv_label_create(col1);
        lv_obj_set_style_text_font(home_weather_desc1_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_desc1_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_desc1_label_, "--");
    }
    {
        // 列分隔线 2
        auto* wsep2 = lv_obj_create(weather_strip);
        lv_obj_set_size(wsep2, 1, 70);
        lv_obj_set_style_radius(wsep2, 0, 0);
        lv_obj_set_style_bg_color(wsep2, lv_color_black(), 0);
        lv_obj_set_style_border_width(wsep2, 0, 0);
        lv_obj_set_style_pad_all(wsep2, 0, 0);
    }
    // 后天天气列
    {
        auto* col2 = lv_obj_create(weather_strip);
        lv_obj_set_width(col2, LV_PCT(30));
        lv_obj_set_flex_grow(col2, 1);
        lv_obj_set_style_radius(col2, 0, 0);
        lv_obj_set_style_bg_color(col2, lv_color_white(), 0);
        lv_obj_set_style_border_width(col2, 0, 0);
        lv_obj_set_style_pad_all(col2, 1, 0);
        lv_obj_set_style_pad_row(col2, 0, 0);
        lv_obj_set_scrollbar_mode(col2, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(col2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        home_weather_day2_label_ = lv_label_create(col2);
        lv_obj_set_style_text_font(home_weather_day2_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_day2_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_day2_label_, "后天");

        home_weather_icon2_label_ = lv_label_create(col2);
        lv_obj_set_style_text_font(home_weather_icon2_label_, &font_awesome_16_4, 0);
        lv_obj_set_style_text_color(home_weather_icon2_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_icon2_label_, "");

        home_weather_temp2_label_ = lv_label_create(col2);
        lv_obj_set_style_text_font(home_weather_temp2_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_temp2_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_temp2_label_, "--°C");

        home_weather_desc2_label_ = lv_label_create(col2);
        lv_obj_set_style_text_font(home_weather_desc2_label_, &alibaba_puhui_14, 0);
        lv_obj_set_style_text_color(home_weather_desc2_label_, lv_color_black(), 0);
        lv_label_set_text(home_weather_desc2_label_, "--");
    }

    // ====================== 课程区 ======================
    // 使用 flex_grow 填充剩余空间，自动适配内容
    auto* course_area = lv_obj_create(main_area);
    lv_obj_set_width(course_area, LV_HOR_RES);
    lv_obj_set_flex_grow(course_area, 1);
    lv_obj_set_style_radius(course_area, 0, 0);
    lv_obj_set_style_bg_color(course_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(course_area, 0, 0);  // 无边框，由内部分隔线区分
    lv_obj_set_style_pad_left(course_area, 14, 0);
    lv_obj_set_style_pad_right(course_area, 14, 0);
    lv_obj_set_style_pad_top(course_area, 0, 0);
    lv_obj_set_style_pad_bottom(course_area, 0, 0);
    lv_obj_set_style_pad_row(course_area, 2, 0);
    lv_obj_set_scrollbar_mode(course_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(course_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(course_area, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    // 今日课程标题（14号字）
    home_today_title_label_ = lv_label_create(course_area);
    lv_obj_set_width(home_today_title_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(home_today_title_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(home_today_title_label_, lv_color_black(), 0);
    lv_label_set_text(home_today_title_label_, "今日课程");

    // 今日课程内容（14号字）
    home_today_courses_label_ = lv_label_create(course_area);
    lv_obj_set_width(home_today_courses_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(home_today_courses_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(home_today_courses_label_, lv_color_black(), 0);
    lv_label_set_long_mode(home_today_courses_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(home_today_courses_label_, "未配置课程表");

    // ===== 分隔线（今日课程和明日课程之间）=====
    auto* course_divider = lv_obj_create(course_area);
    lv_obj_set_size(course_divider, LV_HOR_RES - 28, 1);
    lv_obj_set_style_radius(course_divider, 0, 0);
    lv_obj_set_style_bg_color(course_divider, lv_color_black(), 0);
    lv_obj_set_style_border_width(course_divider, 0, 0);
    lv_obj_set_style_pad_all(course_divider, 0, 0);

    // 明日课程标题（14号字）
    home_tomorrow_title_label_ = lv_label_create(course_area);
    lv_obj_set_width(home_tomorrow_title_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(home_tomorrow_title_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(home_tomorrow_title_label_, lv_color_black(), 0);
    lv_label_set_text(home_tomorrow_title_label_, "明日课程");

    // 明日课程内容（14号字）
    home_tomorrow_courses_label_ = lv_label_create(course_area);
    lv_obj_set_width(home_tomorrow_courses_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(home_tomorrow_courses_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(home_tomorrow_courses_label_, lv_color_black(), 0);
    lv_label_set_long_mode(home_tomorrow_courses_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(home_tomorrow_courses_label_, "未配置课程表");

    // ====================== 底部状态条 ======================
    auto* bottom_strip = lv_obj_create(main_area);
    lv_obj_set_size(bottom_strip, LV_HOR_RES, 24);
    lv_obj_set_style_radius(bottom_strip, 0, 0);
    lv_obj_set_style_bg_color(bottom_strip, lv_color_white(), 0);
    lv_obj_set_style_border_width(bottom_strip, 1, 0);
    lv_obj_set_style_border_side(bottom_strip, LV_BORDER_SIDE_TOP, 0);  // 只有顶部边框
    lv_obj_set_style_border_color(bottom_strip, lv_color_black(), 0);
    lv_obj_set_style_pad_left(bottom_strip, 14, 0);
    lv_obj_set_style_pad_right(bottom_strip, 14, 0);
    lv_obj_set_style_pad_top(bottom_strip, 4, 0);
    lv_obj_set_style_pad_bottom(bottom_strip, 4, 0);
    lv_obj_set_scrollbar_mode(bottom_strip, LV_SCROLLBAR_MODE_OFF);

    home_bottom_status_label_ = lv_label_create(bottom_strip);
    lv_obj_set_width(home_bottom_status_label_, LV_HOR_RES - 28);
    lv_obj_set_style_text_font(home_bottom_status_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(home_bottom_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(home_bottom_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(home_bottom_status_label_,
                      std::string("小智: ").append(Lang::Strings::STANDBY).c_str());

    // ====================== 初始化顶栏数据 ======================
    // 主页的顶栏不显示时间，所以 datetime_label 传 nullptr
    UpdateTopBar(home_top_temp_label_, home_top_humidity_label_,
                 nullptr, home_top_battery_label_);

    // ====================== 创建定时刷新器（60 秒周期）======================
    if (home_refresh_timer_ == nullptr) {
        home_refresh_timer_ = lv_timer_create(HomeRefreshTimerCb, 60000, this);
        // 一发定时器：首次天气数据获取后快速刷新 UI（不等 60s）
        lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t));
            self->UpdateHomePage();
            lv_timer_del(t);
        }, 8000, this);
    }

    // 隐藏页面，等 SwitchPage(kHome) 时再显示
    lv_obj_add_flag(home_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::CreateMusicPage(lv_obj_t* screen) {
    // ========================================================================
    // 音乐播放器布局（参照 docs/v2/prototype/ui-r2/device-music.html）
    // ┌──────────────────────────────────────┐
    // │ 日期 时间 温度 湿度   WiFi  电池     │  <- 顶栏 (28px)
    // ├──────────────────────────────────────┤
    // │          小星星                      │  <- 歌曲名
    // │         儿童合唱团                   │  <- 歌手
    // ├──────────────────────────────────────┤
    // │ ████████░░░░░░░░░░  1:23    3:45    │  <- 进度条
    // ├──────────────────────────────────────┤
    // │       ⏮     ⏸     ⏭              │  <- 控制
    // ├──────────────────────────────────────┤
    // │ ┌──────────────────────────────────┐ │
    // │ │   一闪一闪亮晶晶                  │ │  <- 歌词
    // │ │   满天都是小星星                  │ │
    // │ │   挂在天上放光明                  │ │
    // │ └──────────────────────────────────┘ │
    // └──────────────────────────────────────┘
    // ========================================================================

    music_page_ = CreateFullScreenPage(screen);

    // 顶栏
    music_top_bar_ = CreateTopBar(
        music_page_,
        &music_temp_label_,
        &music_humidity_label_,
        &music_datetime_label_,
        nullptr,
        &music_battery_label_,
        true,   // show datetime
        true,    // show wifi icon
        true);  // show battery

    auto* main_area = lv_obj_create(music_page_);
    lv_obj_set_size(main_area, LV_HOR_RES, LV_VER_RES - 28);
    lv_obj_align(main_area, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_bg_color(main_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);
    lv_obj_set_scrollbar_mode(main_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_area, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // —— 歌曲信息 ——
    auto* song_info = lv_obj_create(main_area);
    lv_obj_set_size(song_info, LV_HOR_RES, 44);
    lv_obj_set_style_radius(song_info, 0, 0);
    lv_obj_set_style_bg_color(song_info, lv_color_white(), 0);
    lv_obj_set_style_border_width(song_info, 1, 0);
    lv_obj_set_style_border_side(song_info, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(song_info, lv_color_black(), 0);
    lv_obj_set_style_pad_all(song_info, 3, 0);
    lv_obj_set_scrollbar_mode(song_info, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(song_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(song_info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    music_title_label_ = lv_label_create(song_info);
    lv_obj_set_style_text_font(music_title_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(music_title_label_, lv_color_black(), 0);
    lv_label_set_text(music_title_label_, "未在播放");

    music_artist_label_ = lv_label_create(song_info);
    lv_obj_set_style_text_font(music_artist_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(music_artist_label_, lv_color_black(), 0);
    lv_label_set_text(music_artist_label_, "");

    // —— 进度条 ——
    auto* progress_area = lv_obj_create(main_area);
    lv_obj_set_size(progress_area, LV_HOR_RES, 40);
    lv_obj_set_style_radius(progress_area, 0, 0);
    lv_obj_set_style_bg_color(progress_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(progress_area, 1, 0);
    lv_obj_set_style_border_side(progress_area, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(progress_area, lv_color_black(), 0);
    lv_obj_set_style_pad_left(progress_area, 14, 0);
    lv_obj_set_style_pad_right(progress_area, 14, 0);
    lv_obj_set_style_pad_top(progress_area, 4, 0);
    lv_obj_set_scrollbar_mode(progress_area, LV_SCROLLBAR_MODE_OFF);

    // 进度条背景（居中放置）
    music_progress_bar_ = lv_obj_create(progress_area);
    lv_obj_set_size(music_progress_bar_, LV_HOR_RES - 28, 6);
    lv_obj_align(music_progress_bar_, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(music_progress_bar_, 0, 0);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_border_width(music_progress_bar_, 1, 0);
    lv_obj_set_style_border_color(music_progress_bar_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(music_progress_bar_, 0, 0);

    // 进度填充
    music_progress_fill_ = lv_obj_create(music_progress_bar_);
    lv_obj_set_width(music_progress_fill_, 0);
    lv_obj_set_height(music_progress_fill_, LV_PCT(100));
    lv_obj_set_style_radius(music_progress_fill_, 0, 0);
    lv_obj_set_style_bg_color(music_progress_fill_, lv_color_black(), 0);
    lv_obj_set_style_border_width(music_progress_fill_, 0, 0);
    lv_obj_set_style_pad_all(music_progress_fill_, 0, 0);
    lv_obj_align(music_progress_fill_, LV_ALIGN_LEFT_MID, 0, 0);

    // 时间标签（进度条下方）
    music_time_cur_label_ = lv_label_create(progress_area);
    lv_obj_set_style_text_font(music_time_cur_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(music_time_cur_label_, lv_color_black(), 0);
    lv_obj_align(music_time_cur_label_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(music_time_cur_label_, "0:00");

    music_time_total_label_ = lv_label_create(progress_area);
    lv_obj_set_style_text_font(music_time_total_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(music_time_total_label_, lv_color_black(), 0);
    lv_obj_align(music_time_total_label_, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(music_time_total_label_, "0:00");

    // —— 控制按钮（用 Font Awesome 图标） ——
    auto* controls = lv_obj_create(main_area);
    lv_obj_set_size(controls, LV_HOR_RES, 36);
    lv_obj_set_style_radius(controls, 0, 0);
    lv_obj_set_style_bg_color(controls, lv_color_white(), 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 2, 0);
    lv_obj_set_scrollbar_mode(controls, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 注意：控件按钮用于展示，实际用物理按键操作
    lv_obj_t* btn_prev = lv_label_create(controls);
    lv_obj_set_style_text_font(btn_prev, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(btn_prev, lv_color_black(), 0);
    lv_obj_set_style_pad_right(btn_prev, 14, 0);
    lv_label_set_text(btn_prev, FONT_AWESOME_BACKWARD_STEP);

    lv_obj_t* btn_play = lv_label_create(controls);
    lv_obj_set_style_text_font(btn_play, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(btn_play, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(btn_play, 8, 0);
    lv_label_set_text(btn_play, FONT_AWESOME_PLAY);

    lv_obj_t* btn_next = lv_label_create(controls);
    lv_obj_set_style_text_font(btn_next, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(btn_next, lv_color_black(), 0);
    lv_obj_set_style_pad_left(btn_next, 14, 0);
    lv_label_set_text(btn_next, FONT_AWESOME_FORWARD_STEP);

    // —— 歌词区 ——
    auto* lyrics_box = lv_obj_create(main_area);
    lv_obj_set_width(lyrics_box, LV_HOR_RES - 28);
    lv_obj_set_flex_grow(lyrics_box, 1);
    lv_obj_set_style_radius(lyrics_box, 0, 0);
    lv_obj_set_style_bg_color(lyrics_box, lv_color_white(), 0);
    lv_obj_set_style_border_width(lyrics_box, 1, 0);
    lv_obj_set_style_border_color(lyrics_box, lv_color_black(), 0);
    lv_obj_set_style_pad_all(lyrics_box, 6, 0);
    lv_obj_set_style_margin_left(lyrics_box, 14, 0);
    lv_obj_set_style_margin_right(lyrics_box, 14, 0);
    lv_obj_set_style_margin_bottom(lyrics_box, 4, 0);
    lv_obj_set_scrollbar_mode(lyrics_box, LV_SCROLLBAR_MODE_OFF);

    music_lyrics_label_ = lv_label_create(lyrics_box);
    lv_obj_set_width(music_lyrics_label_, LV_HOR_RES - 44);
    lv_obj_set_style_text_font(music_lyrics_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(music_lyrics_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyrics_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(music_lyrics_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(music_lyrics_label_, "暂无歌词");

    // —— 聊天浮层（默认隐藏，唤醒小智时显示）——
    music_chat_label_ = lv_label_create(music_page_);
    lv_obj_set_width(music_chat_label_, LV_HOR_RES - 16);
    lv_obj_set_style_text_font(music_chat_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(music_chat_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_color(music_chat_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(music_chat_label_, 1, 0);
    lv_obj_set_style_border_side(music_chat_label_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(music_chat_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(music_chat_label_, 4, 0);
    lv_obj_align(music_chat_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(music_chat_label_, "");
    lv_obj_add_flag(music_chat_label_, LV_OBJ_FLAG_HIDDEN);

    UpdateTopBar(music_temp_label_, music_humidity_label_, music_datetime_label_, music_battery_label_);

    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::CreateActivationPage(lv_obj_t* screen) {
    activation_page_ = CreateFullScreenPage(screen);

    activation_top_bar_ = CreateTopBar(
        activation_page_,
        &activation_temp_label_,
        &activation_humidity_label_,
        &activation_datetime_label_,
        nullptr,
        &activation_battery_label_,
        true,
        false,
        true);

    auto* main_area = lv_obj_create(activation_page_);
    lv_obj_set_size(main_area, LV_HOR_RES, LV_VER_RES - 28);
    lv_obj_align(main_area, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_bg_color(main_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_left(main_area, 14, 0);
    lv_obj_set_style_pad_right(main_area, 14, 0);
    lv_obj_set_style_pad_top(main_area, 8, 0);
    lv_obj_set_style_pad_bottom(main_area, 0, 0);
    lv_obj_set_style_pad_row(main_area, 8, 0);
    lv_obj_set_scrollbar_mode(main_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    activation_title_label_ = lv_label_create(main_area);
    lv_label_set_text(activation_title_label_, "激活设备");
    lv_obj_set_style_text_font(activation_title_label_, &alibaba_puhui_title_24, 0);
    lv_obj_set_style_text_color(activation_title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(activation_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(activation_title_label_, 6, 0);

    activation_code_card_ = lv_obj_create(main_area);
    lv_obj_set_width(activation_code_card_, LV_HOR_RES - 28);
    lv_obj_set_height(activation_code_card_, 72);
    lv_obj_set_style_bg_color(activation_code_card_, lv_color_white(), 0);
    lv_obj_set_style_border_width(activation_code_card_, 2, 0);
    lv_obj_set_style_border_color(activation_code_card_, lv_color_black(), 0);
    lv_obj_set_style_pad_left(activation_code_card_, 10, 0);
    lv_obj_set_style_pad_right(activation_code_card_, 10, 0);
    lv_obj_set_style_pad_top(activation_code_card_, 6, 0);
    lv_obj_set_style_pad_bottom(activation_code_card_, 6, 0);
    lv_obj_set_style_pad_row(activation_code_card_, 4, 0);
    lv_obj_set_scrollbar_mode(activation_code_card_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(activation_code_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(activation_code_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    activation_code_caption_label_ = lv_label_create(activation_code_card_);
    lv_obj_set_width(activation_code_caption_label_, LV_PCT(100));
    lv_obj_set_style_text_font(activation_code_caption_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(activation_code_caption_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(activation_code_caption_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(activation_code_caption_label_, "激活码");

    activation_code_label_ = lv_label_create(activation_code_card_);
    lv_obj_set_width(activation_code_label_, LV_PCT(100));
    lv_obj_set_style_text_font(activation_code_label_, &alibaba_puhui_title_24, 0);
    lv_obj_set_style_text_color(activation_code_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(activation_code_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(activation_code_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(activation_code_label_, "----");

    activation_hint_label_ = lv_label_create(main_area);
    lv_obj_set_width(activation_hint_label_, LV_HOR_RES - 40);
    lv_obj_set_style_text_font(activation_hint_label_, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(activation_hint_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(activation_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(activation_hint_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(activation_hint_label_, "请在小智控制台输入验证码完成激活");

    UpdateTopBar(activation_temp_label_, activation_humidity_label_, activation_datetime_label_, activation_battery_label_);
    lv_obj_add_flag(activation_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::SwitchPage(UiPage page) {
    current_page_ = page;

    auto set_visible = [](lv_obj_t* obj, bool visible) {
        if (obj == nullptr) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };

    set_visible(boot_page_, page == UiPage::kBoot);
    set_visible(wifi_config_page_, page == UiPage::kWifiConfig);
    set_visible(activation_page_, page == UiPage::kActivation);
    set_visible(home_page_, page == UiPage::kHome);
    set_visible(music_page_, page == UiPage::kMusic);
}

void CustomLcdDisplay::CyclePage() {
    if (!Lock(30000)) {
        ESP_LOGW(TAG, "CyclePage lock failed");
        return;
    }
    if (current_page_ == UiPage::kHome) {
        SwitchPage(UiPage::kMusic);
    } else if (current_page_ == UiPage::kMusic) {
        SwitchPage(UiPage::kHome);
    }
    Unlock();
}

void CustomLcdDisplay::UpdateWifiConfigMessage(const char* message) {
    last_message_text_ = message != nullptr ? message : "";

    UpdateWifiConfigPage();
}

lv_obj_t* CustomLcdDisplay::CreateTopBar(lv_obj_t* parent,
                                        lv_obj_t** out_temp_label,
                                        lv_obj_t** out_humidity_label,
                                        lv_obj_t** out_datetime_label,
                                        lv_obj_t** out_wifi_icon_label,
                                        lv_obj_t** out_battery_label,
                                        bool show_datetime,
                                        bool show_wifi_icon,
                                        bool show_battery) {
    // 顶部状态栏（公共组件）：
    // - 左侧：温度/湿度
    // - 右侧：按页面需要展示图标（WiFi/电池）
    // 注意：图标使用 LVGL 内置 LV_SYMBOL_*，必须用默认字体（LV_FONT_DEFAULT）才能确保有字形
    lv_obj_t* top_bar = lv_obj_create(parent);
    lv_obj_set_size(top_bar, LV_HOR_RES, 28);
    lv_obj_align(top_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top_bar, lv_color_black(), 0);
    lv_obj_set_style_pad_left(top_bar, 10, 0);
    lv_obj_set_style_pad_right(top_bar, 10, 0);
    lv_obj_set_style_pad_top(top_bar, 0, 0);
    lv_obj_set_style_pad_bottom(top_bar, 0, 0);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* top_left = lv_obj_create(top_bar);
    lv_obj_set_style_bg_opa(top_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_left, 0, 0);
    lv_obj_set_style_pad_all(top_left, 0, 0);
    lv_obj_set_size(top_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top_left, 6, 0);

    lv_obj_t* datetime_label = lv_label_create(top_left);
    lv_obj_set_style_text_font(datetime_label, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(datetime_label, lv_color_black(), 0);
    lv_label_set_text(datetime_label, "----年--月--日 --:--  ");

    lv_obj_t* temp_label = lv_label_create(top_left);
    lv_obj_set_style_text_font(temp_label, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(temp_label, lv_color_black(), 0);
    lv_label_set_text(temp_label, "--°C");

    lv_obj_t* humidity_label = lv_label_create(top_left);
    lv_obj_set_style_text_font(humidity_label, &alibaba_puhui_14, 0);
    lv_obj_set_style_text_color(humidity_label, lv_color_black(), 0);
    lv_label_set_text(humidity_label, "--%");

    auto* top_right = lv_obj_create(top_bar);
    lv_obj_set_style_bg_opa(top_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_right, 0, 0);
    lv_obj_set_style_pad_all(top_right, 0, 0);
    lv_obj_set_size(top_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top_right, 6, 0);

    lv_obj_t* wifi_icon_label = lv_label_create(top_right);
    lv_obj_set_style_text_font(wifi_icon_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(wifi_icon_label, lv_color_black(), 0);
    lv_label_set_text(wifi_icon_label, LV_SYMBOL_WIFI);

    lv_obj_t* battery_label = lv_label_create(top_right);
    lv_obj_set_style_text_font(battery_label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_black(), 0);
    lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_FULL " --%");

    if (!show_wifi_icon) {
        lv_obj_add_flag(wifi_icon_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show_battery) {
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show_datetime) {
        lv_obj_add_flag(datetime_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (out_wifi_icon_label != nullptr) {
        *out_wifi_icon_label = wifi_icon_label;
    }
    if (out_battery_label != nullptr) {
        *out_battery_label = battery_label;
    }

    if (out_temp_label != nullptr) {
        *out_temp_label = temp_label;
    }
    if (out_humidity_label != nullptr) {
        *out_humidity_label = humidity_label;
    }
    if (out_datetime_label != nullptr) {
        *out_datetime_label = datetime_label;
    }

    return top_bar;
}

void CustomLcdDisplay::TopBarTimerCb(lv_timer_t* timer) {
    auto* self = timer != nullptr ? static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer)) : nullptr;
    if (self == nullptr) {
        return;
    }
    self->UpdateTopBar(self->wifi_config_temp_label_, self->wifi_config_humidity_label_, nullptr, self->wifi_config_battery_label_);
    self->UpdateTopBar(self->activation_temp_label_, self->activation_humidity_label_, self->activation_datetime_label_, self->activation_battery_label_);
    // 同步更新主页顶栏的温度/湿度/电池（主页不显示时间，datetime_label 传 nullptr）
    self->UpdateTopBar(self->home_top_temp_label_, self->home_top_humidity_label_,
                       nullptr, self->home_top_battery_label_);
}

void CustomLcdDisplay::UpdateTopBar(lv_obj_t* temp_label, lv_obj_t* humidity_label, lv_obj_t* datetime_label, lv_obj_t* battery_label) {
    float temp = 0.0f;
    bool temp_ok = Board::GetInstance().GetTemperature(temp);

    if (temp_label != nullptr) {
        if (temp_ok) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f°C", temp);
            lv_label_set_text(temp_label, buf);
        } else {
            lv_label_set_text(temp_label, "--°C");
        }
    }

    if (humidity_label != nullptr) {
        float humidity = 0.0f;
        bool humidity_ok = Board::GetInstance().GetHumidity(humidity);
        if (humidity_ok) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f%%", humidity);
            lv_label_set_text(humidity_label, buf);
        } else {
            lv_label_set_text(humidity_label, "--%");
        }
    }

    if (datetime_label != nullptr) {
        time_t now = time(nullptr);
        struct tm time_info = {};
        bool time_valid = (now > 0) && (localtime_r(&now, &time_info) != nullptr) && (time_info.tm_year >= 2025 - 1900);
        if (time_valid) {
            char datetime_buf[48];
            strftime(datetime_buf, sizeof(datetime_buf), "%m月%d日 %H:%M  ", &time_info);
            std::string datetime_with_gap = std::string(datetime_buf) + "  ";
            lv_label_set_text(datetime_label, datetime_with_gap.c_str());
        } else {
            lv_label_set_text(datetime_label, "----年--月--日 --:--  ");
        }
    }

    if (battery_label != nullptr) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            if (level < 0) level = 0;
            if (level > 100) level = 100;
            char buf[32];
            snprintf(buf, sizeof(buf), LV_SYMBOL_BATTERY_FULL " %d%%%s", level, charging ? "+" : "");
            lv_label_set_text(battery_label, buf);
        } else {
            lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_FULL " --%");
        }
    }
}

void CustomLcdDisplay::UpdateWifiConfigPage() {
    std::string desc = "请连接配网WiFi";
    std::string ssid = "waiting";
    std::string url = "waiting";
    std::string hint = "请先连接上方热点，再在浏览器中打开访问地址完成配置";

    if (!last_message_text_.empty()) {
        const std::string hotspot_prefix = Lang::Strings::CONNECT_TO_HOTSPOT;
        const std::string browser_prefix = Lang::Strings::ACCESS_VIA_BROWSER;
        size_t hotspot_pos = last_message_text_.find(hotspot_prefix);
        size_t browser_pos = last_message_text_.find(browser_prefix);

        if (hotspot_pos != std::string::npos && browser_pos != std::string::npos &&
            browser_pos > hotspot_pos + hotspot_prefix.size()) {
            std::string parsed_ssid = last_message_text_.substr(
                hotspot_pos + hotspot_prefix.size(),
                browser_pos - (hotspot_pos + hotspot_prefix.size()));
            std::string parsed_url = last_message_text_.substr(browser_pos + browser_prefix.size());

            if (!parsed_ssid.empty()) {
                ssid = parsed_ssid;
            }
            if (!parsed_url.empty()) {
                url = parsed_url;
            }
            hint = "请先连接上方热点，再在浏览器中打开访问地址完成配置";
        } else {
            hint = "等待连接";
        }
    }

    if (wifi_config_desc_label_ != nullptr) {
        lv_label_set_text(wifi_config_desc_label_, desc.c_str());
    }
    if (wifi_config_ssid_label_ != nullptr) {
        lv_label_set_text(wifi_config_ssid_label_, ssid.c_str());
    }
    if (wifi_config_url_label_ != nullptr) {
        lv_label_set_text(wifi_config_url_label_, url.c_str());
    }
    if (wifi_config_hint_label_ != nullptr) {
        lv_label_set_text(wifi_config_hint_label_, hint.c_str());
    }
}

void CustomLcdDisplay::UpdateHomeStatus(const char* status) {
    last_status_text_ = status != nullptr ? status : "";
    std::string display_text = "小智: ";
    display_text += last_status_text_.empty() ? Lang::Strings::STANDBY : last_status_text_;
    if (home_bottom_status_label_ != nullptr) {
        lv_label_set_text(home_bottom_status_label_, display_text.c_str());
    }
}

// ============================================================================
// 首页数据刷新
// ============================================================================

/**
 * @brief 定时器回调：周期性刷新首页数据
 *
 * 每 60 秒被 LVGL 定时器调用，更新课程表和天气数据。
 * 内部调用 UpdateHomePage() 完成实际的数据读取与 UI 刷新。
 */
void CustomLcdDisplay::HomeRefreshTimerCb(lv_timer_t* timer) {
    auto* self = timer != nullptr
        ? static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer))
        : nullptr;
    if (self == nullptr) {
        return;
    }
    self->UpdateHomePage();
}

/**
 * @brief 从 HomeDataStore 读取最新数据并刷新首页所有标签
 *
 * 刷新内容包括：
 * - 时间条：日期（"今天 周四"）+ 大字时钟（"12:33"）
 * - 三日天气：今日/明日/后天天气描述和温度
 * - 今日/明日课程：显示课程名称列表
 * - 天气数据通过 RefreshWeather() 异步触发（带 30 分钟缓存）
 */
void CustomLcdDisplay::UpdateHomePage() {
    if (home_data_store_ == nullptr) {
        return;
    }

    // —— 更新课程表（UpdateSchedule 会根据当前日期推算今日/明日课程）——
    home_data_store_->UpdateSchedule();

    // —— 触发天气刷新（在后台任务中执行，避免阻塞 LVGL）——
    // RefreshWeather() 内部有 30 分钟缓存检查，去掉一次性标志以支持定时重试
    static bool s_weather_task_running = false;
    
    if (!s_weather_task_running && home_data_store_->HasWeatherConfig()) {
        time_t now = time(nullptr);
        struct tm time_info = {};
        bool time_valid = (localtime_r(&now, &time_info) != nullptr && time_info.tm_year >= 2025 - 1900);
        
        if (time_valid) {
            s_weather_task_running = true;
            xTaskCreatePinnedToCore(
                [](void* arg) {
                    auto* store = static_cast<HomeDataStore*>(arg);
                    ESP_LOGI("WeatherTask", "Weather fetch running...");
                    store->RefreshWeather();
                    ESP_LOGI("WeatherTask", "Weather fetch done");
                    s_weather_task_running = false;
                    vTaskDelete(nullptr);
                },
                "weather_fetch", 16384, home_data_store_, 5, nullptr, 0
            );
        }
    }

    const HomeData& data = home_data_store_->GetHomeData();

    // ====================== 时间条 ======================
    time_t now = time(nullptr);
    struct tm time_info = {};
    bool time_valid = (now > 0) &&
                      (localtime_r(&now, &time_info) != nullptr) &&
                      (time_info.tm_year >= 2025 - 1900);

    if (time_valid) {
        // 日期行："今天 周四"
        const char* wday_names[] = {"周日", "周一", "周二", "周三",
                                     "周四", "周五", "周六"};
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s",
                 time_info.tm_mon + 1, time_info.tm_mday,
                 wday_names[time_info.tm_wday]);
        if (home_date_label_ != nullptr) {
            lv_label_set_text(home_date_label_, date_buf);
        }

        // 大字时钟："12:33"
        char clock_buf[16];
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d",
                 time_info.tm_hour, time_info.tm_min);
        if (home_clock_label_ != nullptr) {
            lv_label_set_text(home_clock_label_, clock_buf);
        }
    }

    // ====================== 三日天气（4行/列：日期+图标+温度+描述）======================
    // 辅助：设置一列天气
    auto set_weather_col = [](lv_obj_t* day_l, lv_obj_t* icon_l, lv_obj_t* temp_l,
                               lv_obj_t* desc_l, const WeatherDay& w, const char* day_text) {
        if (w.description.empty() && w.high_temp.empty()) {
            if (day_l) lv_label_set_text(day_l, day_text);
            if (icon_l) lv_label_set_text(icon_l, "");
            if (temp_l) lv_label_set_text(temp_l, "--°C");
            if (desc_l) lv_label_set_text(desc_l, "--");
            return;
        }
        if (day_l) lv_label_set_text(day_l, day_text);
        if (icon_l) lv_label_set_text(icon_l, get_weather_icon(w.description));
        if (temp_l) {
            char tbuf[32];
            snprintf(tbuf, sizeof(tbuf), "%s°C-%s°C",
                     w.low_temp.c_str(), w.high_temp.c_str());
            lv_label_set_text(temp_l, tbuf);
        }
        if (desc_l) lv_label_set_text(desc_l, w.description.c_str());
    };

    set_weather_col(home_weather_day0_label_, home_weather_icon0_label_,
                    home_weather_temp0_label_, home_weather_desc0_label_,
                    data.weather[0], "今天");
    set_weather_col(home_weather_day1_label_, home_weather_icon1_label_,
                    home_weather_temp1_label_, home_weather_desc1_label_,
                    data.weather[1], "明天");
    set_weather_col(home_weather_day2_label_, home_weather_icon2_label_,
                    home_weather_temp2_label_, home_weather_desc2_label_,
                    data.weather[2], "后天");

    // 天气数据诊断
    ESP_LOGI(TAG, "Weather: d0=%s(%s/%s) d1=%s(%s/%s) d2=%s(%s/%s)",
             data.weather[0].description.c_str(), data.weather[0].low_temp.c_str(), data.weather[0].high_temp.c_str(),
             data.weather[1].description.c_str(), data.weather[1].low_temp.c_str(), data.weather[1].high_temp.c_str(),
             data.weather[2].description.c_str(), data.weather[2].low_temp.c_str(), data.weather[2].high_temp.c_str());

    // ====================== 今日课程 ======================
    // 根据原型：周末显示 "周末愉快! :-D"，工作日显示课程列表
    if (home_today_title_label_ != nullptr) {
        // 显示单双周信息
        if (data.has_schedule) {
            time_t now = time(nullptr);
            bool is_dual = home_data_store_->IsDualWeek(now);
            const char* week_str = is_dual ? "双周" : "单周";
            
            struct tm time_info = {};
            bool is_weekend = false;
            if (localtime_r(&now, &time_info) != nullptr) {
                is_weekend = (time_info.tm_wday == 0 || time_info.tm_wday == 6);
            }
            
            char title_buf[48];
            snprintf(title_buf, sizeof(title_buf), "今日课程（%s）：", week_str);
            lv_label_set_text(home_today_title_label_, title_buf);
        } else {
            lv_label_set_text(home_today_title_label_, "今日课程");
        }
    }

    if (home_today_courses_label_ != nullptr) {
        if (!data.has_schedule) {
            lv_label_set_text(home_today_courses_label_, "未配置课程表");
        } else if (data.today_schedule.courses.empty()) {
            lv_label_set_text(home_today_courses_label_, "周末愉快! :-D");
        } else {
            // 分组：上午(1-4节)、下午(5-8节)
            std::string am_str, pm_str;
            for (const auto& c : data.today_schedule.courses) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d.%s",
                         c.period <= 4 ? c.period : c.period - 4,
                         c.name.c_str());
                if (c.period <= 4) {
                    if (!am_str.empty()) am_str += " ";
                    am_str += buf;
                } else {
                    if (!pm_str.empty()) pm_str += " ";
                    pm_str += buf;
                }
            }
            std::string result;
            if (!am_str.empty()) result += "上午：" + am_str;
            if (!pm_str.empty()) {
                if (!result.empty()) result += "\n";
                result += "下午：" + pm_str;
            }
            lv_label_set_text(home_today_courses_label_, result.c_str());
        }
    }

    // ====================== 明日课程 ======================
    // 根据原型：周末/休息日显示 "好好休息 ^_^"，工作日显示课程列表
    if (home_tomorrow_title_label_ != nullptr) {
        if (data.has_schedule) {
            // 明天的单双周可能和今天不同（跨周时）
            time_t now = time(nullptr);
            time_t tomorrow = now + 86400;  // 加一天
            bool is_dual_tomorrow = home_data_store_->IsDualWeek(tomorrow);
            const char* week_str = is_dual_tomorrow ? "双周" : "单周";
            
            char title_buf[48];
            snprintf(title_buf, sizeof(title_buf), "明日课程（%s）：", week_str);
            lv_label_set_text(home_tomorrow_title_label_, title_buf);
        } else {
            lv_label_set_text(home_tomorrow_title_label_, "明日课程");
        }
    }

    if (home_tomorrow_courses_label_ != nullptr) {
        if (!data.has_schedule) {
            lv_label_set_text(home_tomorrow_courses_label_, "未配置课程表");
        } else if (data.tomorrow_schedule.courses.empty()) {
            lv_label_set_text(home_tomorrow_courses_label_, "好好休息 ^_^");
        } else {
            // 分组：上午(1-4节)、下午(5-8节)
            std::string am_str, pm_str;
            for (const auto& c : data.tomorrow_schedule.courses) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d.%s",
                         c.period <= 4 ? c.period : c.period - 4,
                         c.name.c_str());
                if (c.period <= 4) {
                    if (!am_str.empty()) am_str += " ";
                    am_str += buf;
                } else {
                    if (!pm_str.empty()) pm_str += " ";
                    pm_str += buf;
                }
            }
            std::string result;
            if (!am_str.empty()) result += "上午：" + am_str;
            if (!pm_str.empty()) {
                if (!result.empty()) result += "\n";
                result += "下午：" + pm_str;
            }
            lv_label_set_text(home_tomorrow_courses_label_, result.c_str());
        }
    }
}

void CustomLcdDisplay::UpdateActivationCode(const char* code) {
    if (code != nullptr && code[0] != '\0') {
        bool all_digits = true;
        for (const char* p = code; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            last_activation_code_ = code;
        }
    }
    const char* safe_code = last_activation_code_.empty() ? "----" : last_activation_code_.c_str();
    if (activation_code_label_ != nullptr) {
        lv_label_set_text(activation_code_label_, safe_code);
    }
}

void CustomLcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    setup_ui_called_ = true;

    if (!Lock(30000)) {
        ESP_LOGE(TAG, "Failed to lock display in SetupUI");
        return;
    }

    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);

    CreateBootPage(screen);
    CreateWifiConfigPage(screen);
    CreateActivationPage(screen);
    CreateHomePage(screen);
    CreateMusicPage(screen);
    SwitchPage(UiPage::kBoot);

    if (top_bar_timer_ == nullptr) {
        top_bar_timer_ = lv_timer_create(TopBarTimerCb, 1000, this);
    }

    Unlock();
}

void CustomLcdDisplay::SetStatus(const char* status) {
    if (!Lock(30000)) {
        ESP_LOGE(TAG, "Failed to lock display in SetStatus");
        return;
    }

    const char* safe_status = status != nullptr ? status : "";

    if (strcmp(safe_status, Lang::Strings::WIFI_CONFIG_MODE) == 0) {
        SwitchPage(UiPage::kWifiConfig);
        UpdateTopBar(wifi_config_temp_label_, wifi_config_humidity_label_, nullptr, wifi_config_battery_label_);
        UpdateWifiConfigPage();
    } else if (strcmp(safe_status, Lang::Strings::ACTIVATION) == 0) {
        SwitchPage(UiPage::kActivation);
        UpdateTopBar(activation_temp_label_, activation_humidity_label_, activation_datetime_label_, activation_battery_label_);
        if (activation_hint_label_ != nullptr) {
            lv_label_set_text(activation_hint_label_, "请在小智控制台输入验证码完成激活");
        }
        UpdateActivationCode(nullptr);
    } else if (strcmp(safe_status, Lang::Strings::STANDBY) == 0) {
        if (current_page_ == UiPage::kMusic) {
            SwitchPage(UiPage::kHome);
        }
        UpdateHomePage();  // 立即刷新首页数据，不等 60s 定时器
    }

    if (current_page_ == UiPage::kHome) {
        UpdateHomeStatus(safe_status);
    }

    Unlock();
}

void CustomLcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    SetStatus(notification);
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    (void)emotion;
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;

    if (!Lock(30000)) {
        ESP_LOGE(TAG, "Failed to lock display in SetChatMessage");
        return;
    }

    if (current_page_ == UiPage::kWifiConfig) {
        UpdateWifiConfigMessage(content);
    } else if (current_page_ == UiPage::kActivation) {
        UpdateActivationCode(content);
    } else if (current_page_ == UiPage::kMusic && content != nullptr && content[0] != '\0') {
        // 更新音乐页歌曲信息，尝试解析 "歌手 - 歌名" 格式
        const char* dash = strstr(content, " - ");
        if (dash != nullptr) {
            std::string artist(content, dash - content);
            lv_label_set_text(music_artist_label_, artist.c_str());
            lv_label_set_text(music_title_label_, dash + 3);
        } else {
            lv_label_set_text(music_artist_label_, "");
            lv_label_set_text(music_title_label_, content);
        }
    } else if (content != nullptr && strstr(content, " - ") != nullptr) {
        // 检测到 "歌手 - 歌名" 格式，自动切到音乐页
        SwitchPage(UiPage::kMusic);
        UpdateTopBar(music_temp_label_, music_humidity_label_, music_datetime_label_, music_battery_label_);
        const char* dash = strstr(content, " - ");
        std::string artist(content, dash - content);
        lv_label_set_text(music_artist_label_, artist.c_str());
        lv_label_set_text(music_title_label_, dash + 3);
    } else if (current_page_ == UiPage::kHome && content != nullptr && content[0] != '\0') {
        UpdateHomeStatus(content);
    }

    Unlock();
}

void CustomLcdDisplay::ClearChatMessages() {
    if (!Lock(30000)) {
        ESP_LOGE(TAG, "Failed to lock display in ClearChatMessages");
        return;
    }

    last_message_text_.clear();
    UpdateWifiConfigPage();

    Unlock();
}
