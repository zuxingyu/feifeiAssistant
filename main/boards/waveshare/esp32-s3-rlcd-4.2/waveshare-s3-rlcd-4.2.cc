#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/temperature_sensor.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "lvgl.h"
#include "custom_lcd_display.h"
#define TAG "waveshare_rlcd_4_2"

static temperature_sensor_handle_t temp_sensor = NULL;

class CustomBoard : public WifiBoard {
private:
    enum class EnvSensorType {
        None,
        Sht3x,
        Sht4x,
        Shtc3,
        Hdc1080,
        Si7021,
        Aht2x
    };

    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;  // GPIO18 KEY 按键
    CustomLcdDisplay *display_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;
    EnvSensorType env_sensor_type_ = EnvSensorType::None;
    i2c_master_dev_handle_t env_sensor_dev_ = nullptr;
    uint8_t env_sensor_addr_ = 0;
    TickType_t env_cache_tick_ = 0;
    bool env_cache_ok_ = false;
    float env_cache_temp_c_ = 0.0f;
    float env_cache_humidity_ = 0.0f;

    void InitializeI2c() {
        // 初始化音频编解码器使用的 I2C 主总线。
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (temp_sensor == NULL) {
            temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
            ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
            ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
        }
    }

    void InitializeEnvSensor() {
        constexpr uint8_t kShtc3Addr = 0x70;
        if (i2c_master_probe(i2c_bus_, kShtc3Addr, pdMS_TO_TICKS(100)) != ESP_OK) {
            env_sensor_type_ = EnvSensorType::None;
            env_sensor_dev_ = nullptr;
            env_sensor_addr_ = 0;
            ESP_LOGW(TAG, "SHTC3 not found on I2C (0x70)");
            return;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kShtc3Addr,
            .scl_speed_hz = 100 * 1000,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = 0,
            },
        };
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &env_sensor_dev_) != ESP_OK) {
            env_sensor_type_ = EnvSensorType::None;
            env_sensor_dev_ = nullptr;
            env_sensor_addr_ = 0;
            ESP_LOGW(TAG, "SHTC3 add I2C device failed");
            return;
        }

        env_sensor_type_ = EnvSensorType::Shtc3;
        env_sensor_addr_ = kShtc3Addr;

        float t = 0.0f;
        float h = 0.0f;
        if (!ReadEnvFromShtc3(t, h)) {
            ESP_LOGW(TAG, "SHTC3 init read failed");
            i2c_master_bus_rm_device(env_sensor_dev_);
            env_sensor_dev_ = nullptr;
            env_sensor_type_ = EnvSensorType::None;
            env_sensor_addr_ = 0;
            return;
        }

        ESP_LOGI(TAG, "Env sensor detected: SHTC3 (0x%02x)", env_sensor_addr_);
    }

    bool ReadEnvFromSht3x(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        uint8_t cmd[] = {0x24, 0x00};
        if (i2c_master_transmit(env_sensor_dev_, cmd, sizeof(cmd), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t buf[6] = {};
        if (i2c_master_receive(env_sensor_dev_, buf, sizeof(buf), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }

        uint16_t raw_t = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
        uint16_t raw_rh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];

        temp_c = -45.0f + 175.0f * (static_cast<float>(raw_t) / 65535.0f);
        humidity = 100.0f * (static_cast<float>(raw_rh) / 65535.0f);
        return true;
    }

    bool ReadEnvFromSht4x(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        uint8_t cmd = 0xFD;
        if (i2c_master_transmit(env_sensor_dev_, &cmd, 1, pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t buf[6] = {};
        if (i2c_master_receive(env_sensor_dev_, buf, sizeof(buf), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }

        uint16_t raw_t = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
        uint16_t raw_rh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
        temp_c = -45.0f + 175.0f * (static_cast<float>(raw_t) / 65535.0f);
        humidity = -6.0f + 125.0f * (static_cast<float>(raw_rh) / 65535.0f);
        if (humidity < 0.0f) humidity = 0.0f;
        if (humidity > 100.0f) humidity = 100.0f;
        return true;
    }

    bool ReadEnvFromShtc3(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        auto crc8 = [](const uint8_t* data, size_t len) -> uint8_t {
            uint8_t crc = 0xFF;
            for (size_t i = 0; i < len; ++i) {
                crc ^= data[i];
                for (int bit = 0; bit < 8; ++bit) {
                    crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
                }
            }
            return crc;
        };

        auto do_measure = [&](float& out_temp_c, float& out_humidity) -> bool {
            uint8_t wake_cmd[] = {0x35, 0x17};
            i2c_master_transmit(env_sensor_dev_, wake_cmd, sizeof(wake_cmd), pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(1));

            uint8_t measure_cmd[] = {0x7C, 0xA2};
            if (i2c_master_transmit(env_sensor_dev_, measure_cmd, sizeof(measure_cmd), pdMS_TO_TICKS(100)) != ESP_OK) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));

            uint8_t buf[6] = {};
            if (i2c_master_receive(env_sensor_dev_, buf, sizeof(buf), pdMS_TO_TICKS(100)) != ESP_OK) {
                return false;
            }

            if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
                return false;
            }

            uint16_t raw_t = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
            uint16_t raw_rh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];

            out_temp_c = -45.0f + 175.0f * (static_cast<float>(raw_t) / 65536.0f);
            out_humidity = 100.0f * (static_cast<float>(raw_rh) / 65536.0f);

            uint8_t sleep_cmd[] = {0xB0, 0x98};
            i2c_master_transmit(env_sensor_dev_, sleep_cmd, sizeof(sleep_cmd), pdMS_TO_TICKS(100));
            return true;
        };

        float t = 0.0f;
        float h = 0.0f;
        if (!do_measure(t, h)) {
            uint8_t reset_cmd[] = {0x80, 0x5D};
            i2c_master_transmit(env_sensor_dev_, reset_cmd, sizeof(reset_cmd), pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(2));
            if (!do_measure(t, h)) {
                return false;
            }
        }

        temp_c = t;
        humidity = h;
        return true;
    }

    bool ReadEnvFromHdc1080(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        uint8_t reg = 0x00;
        if (i2c_master_transmit(env_sensor_dev_, &reg, 1, pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t buf[4] = {};
        if (i2c_master_receive(env_sensor_dev_, buf, sizeof(buf), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }

        uint16_t raw_t = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
        uint16_t raw_rh = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];

        temp_c = (static_cast<float>(raw_t) / 65536.0f) * 165.0f - 40.0f;
        humidity = (static_cast<float>(raw_rh) / 65536.0f) * 100.0f;
        return true;
    }

    bool ReadEnvFromSi7021(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        uint8_t cmd_h = 0xF5;
        if (i2c_master_transmit(env_sensor_dev_, &cmd_h, 1, pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(30));

        uint8_t rh_buf[3] = {};
        if (i2c_master_receive(env_sensor_dev_, rh_buf, sizeof(rh_buf), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        uint16_t raw_rh = (static_cast<uint16_t>(rh_buf[0]) << 8) | rh_buf[1];
        raw_rh &= 0xFFFC;
        humidity = (125.0f * static_cast<float>(raw_rh) / 65536.0f) - 6.0f;
        if (humidity < 0.0f) humidity = 0.0f;
        if (humidity > 100.0f) humidity = 100.0f;

        uint8_t cmd_t = 0xF3;
        if (i2c_master_transmit(env_sensor_dev_, &cmd_t, 1, pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t t_buf[3] = {};
        if (i2c_master_receive(env_sensor_dev_, t_buf, sizeof(t_buf), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        uint16_t raw_t = (static_cast<uint16_t>(t_buf[0]) << 8) | t_buf[1];
        raw_t &= 0xFFFC;
        temp_c = (175.72f * static_cast<float>(raw_t) / 65536.0f) - 46.85f;
        return true;
    }

    bool ReadEnvFromAht2x(float& temp_c, float& humidity) {
        if (env_sensor_dev_ == nullptr) {
            return false;
        }

        uint8_t measure_cmd[] = {0xAC, 0x33, 0x00};
        if (i2c_master_transmit(env_sensor_dev_, measure_cmd, sizeof(measure_cmd), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(90));

        uint8_t data[6] = {};
        if (i2c_master_receive(env_sensor_dev_, data, sizeof(data), pdMS_TO_TICKS(100)) != ESP_OK) {
            return false;
        }

        uint32_t raw_h = (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) | data[3];
        raw_h >>= 4;
        uint32_t raw_t = (static_cast<uint32_t>(data[3] & 0x0F) << 16) | (static_cast<uint32_t>(data[4]) << 8) | data[5];

        humidity = (static_cast<float>(raw_h) / 1048576.0f) * 100.0f;
        temp_c = (static_cast<float>(raw_t) / 1048576.0f) * 200.0f - 50.0f;
        return true;
    }

    bool ReadEnv(float& temp_c, float& humidity) {
        switch (env_sensor_type_) {
            case EnvSensorType::Sht3x:
                return ReadEnvFromSht3x(temp_c, humidity);
            case EnvSensorType::Sht4x:
                return ReadEnvFromSht4x(temp_c, humidity);
            case EnvSensorType::Shtc3:
                return ReadEnvFromShtc3(temp_c, humidity);
            case EnvSensorType::Hdc1080:
                return ReadEnvFromHdc1080(temp_c, humidity);
            case EnvSensorType::Si7021:
                return ReadEnvFromSi7021(temp_c, humidity);
            case EnvSensorType::Aht2x:
                return ReadEnvFromAht2x(temp_c, humidity);
            case EnvSensorType::None:
            default:
                return false;
        }
    }

    bool RefreshEnvCache() {
        constexpr TickType_t kCacheTtl = pdMS_TO_TICKS(900);
        TickType_t now = xTaskGetTickCount();
        if (env_cache_tick_ != 0 && (now - env_cache_tick_) < kCacheTtl) {
            return env_cache_ok_;
        }

        env_cache_tick_ = now;
        float t = 0.0f;
        float h = 0.0f;
        env_cache_ok_ = ReadEnv(t, h);
        if (env_cache_ok_) {
            env_cache_temp_c_ = t;
            env_cache_humidity_ = h;
        }
        return env_cache_ok_;
    }

    void InitializeButtons() { 
        // 单击按键：
        // 1. 开机阶段进入配网
        // 2. 其他阶段切换对话状态
        // BOOT 按钮（GPIO0）- 唤醒小智
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // KEY 按钮 — 页面循环 Home↔Music（调试：先打印日志确认检测到按键）
        user_button_.OnClick([this]() {
            ESP_LOGI(TAG, "KEY button clicked!");
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Device state: %d", (int)app.GetDeviceState());
            auto* display = static_cast<CustomLcdDisplay*>(GetDisplay());
            if (display != nullptr) {
                display->CyclePage();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        // 双击按键：空闲状态下切换本地 AEC 模式。
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeTools() {
        // 注册给上层调用的“重新配网”工具。
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });
    }

    void InitializeLcdDisplay() {
        // 这里只负责创建显示对象，真正的 UI 控件创建在 Application::Initialize() 中进行。
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH,RLCD_HEIGHT,DISPLAY_OFFSET_X,DISPLAY_OFFSET_Y,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y,DISPLAY_SWAP_XY,spi_config);
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            // 首次调用时完成 ADC 单次采样与校准模块初始化。
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0; // 单位：mV
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage =  raw_voltage * 3;
            // ESP_LOGI(TAG, "电池电压: %dmV", voltage);
            return (uint16_t)voltage;
        }

        return 0;
    }

    uint8_t BatterygetPercent() {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }

        // 通过 10 次采样平均减少抖动，再用经验公式估算电量百分比。
        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        // ESP_LOGI(TAG, "电池电压: %dmV, 电量百分比: %d%%", voltage, percent);
        return (uint8_t)percent;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {    
        // 构造阶段完成板级外设准备，供 Application::Initialize() 后续直接使用。
        InitializeI2c();  
        InitializeEnvSensor();
        InitializeButtons();     
        InitializeTools();
        InitializeLcdDisplay();
   }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();

        return true;
    }

    virtual bool GetTemperature(float& esp32temp) override {
        if (!RefreshEnvCache()) {
            return false;
        }
        esp32temp = env_cache_temp_c_;
        return true;
    }

    virtual bool GetHumidity(float& humidity) override {
        if (!RefreshEnvCache()) {
            return false;
        }
        humidity = env_cache_humidity_;
        return true;
    }
};

DECLARE_BOARD(CustomBoard);
