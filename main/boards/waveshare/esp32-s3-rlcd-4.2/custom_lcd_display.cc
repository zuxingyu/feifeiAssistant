#include <vector>
#include <cstring>
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
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 按水平和垂直像素分辨率完成显示对象基础初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

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
LV_FONT_DECLARE(alibaba_puhui_14);
LV_FONT_DECLARE(alibaba_puhui_16);

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
        &wifi_config_battery_label_,
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

    UpdateTopBar(wifi_config_temp_label_, wifi_config_humidity_label_, wifi_config_battery_label_);

    lv_obj_add_flag(wifi_config_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::CreateHomePage(lv_obj_t* screen) {
    home_page_ = CreateFullScreenPage(screen);

    home_title_label_ = lv_label_create(home_page_);
    lv_label_set_text(home_title_label_, "主页");
    lv_obj_set_style_text_color(home_title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(home_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(home_title_label_, LV_ALIGN_TOP_MID, 0, 40);

    home_status_label_ = lv_label_create(home_page_);
    lv_obj_set_width(home_status_label_, LV_HOR_RES - 40);
    lv_obj_set_style_text_color(home_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(home_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(home_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(home_status_label_, Lang::Strings::STANDBY);
    lv_obj_center(home_status_label_);

    lv_obj_add_flag(home_page_, LV_OBJ_FLAG_HIDDEN);
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
    set_visible(home_page_, page == UiPage::kHome);
}

void CustomLcdDisplay::UpdateWifiConfigMessage(const char* message) {
    last_message_text_ = message != nullptr ? message : "";

    UpdateWifiConfigPage();
}

lv_obj_t* CustomLcdDisplay::CreateTopBar(lv_obj_t* parent,
                                        lv_obj_t** out_temp_label,
                                        lv_obj_t** out_humidity_label,
                                        lv_obj_t** out_wifi_icon_label,
                                        lv_obj_t** out_battery_label,
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

    return top_bar;
}

void CustomLcdDisplay::TopBarTimerCb(lv_timer_t* timer) {
    auto* self = timer != nullptr ? static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer)) : nullptr;
    if (self == nullptr) {
        return;
    }
    self->UpdateTopBar(self->wifi_config_temp_label_, self->wifi_config_humidity_label_, self->wifi_config_battery_label_);
}

void CustomLcdDisplay::UpdateTopBar(lv_obj_t* temp_label, lv_obj_t* humidity_label, lv_obj_t* battery_label) {
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
    if (home_status_label_ != nullptr) {
        lv_label_set_text(home_status_label_,
            last_status_text_.empty() ? Lang::Strings::STANDBY : last_status_text_.c_str());
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
    CreateHomePage(screen);
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
        UpdateTopBar(wifi_config_temp_label_, wifi_config_humidity_label_, wifi_config_battery_label_);
        UpdateWifiConfigPage();
    } else if (strcmp(safe_status, Lang::Strings::STANDBY) == 0) {
        SwitchPage(UiPage::kHome);
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
