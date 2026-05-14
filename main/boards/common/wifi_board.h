#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <esp_http_server.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    httpd_handle_t management_server_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * 处理网络事件（由 WiFiManager 的回调转发而来）
     * @param event 网络事件类型
     * @param data 附加数据，例如 Connecting/Connected 阶段的 SSID
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * 发起 Wi-Fi 连接尝试
     */
    void TryWifiConnect();

    /**
     * 进入 Wi-Fi 配网模式
     */
    void StartWifiConfigMode();

    /**
     * 启动/停止正常联网状态下的本地管理页面。
     * 目前用于浏览器维护课程表，不切换到配网 AP。
     */
    void StartManagementServer();
    void StopManagementServer();

    /**
     * Wi-Fi 连接超时回调
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * 异步启动网络连接
     * 该函数会立即返回，网络状态通过 SetNetworkEventCallback() 注册的回调通知上层。
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * 进入 Wi-Fi 配网模式（线程安全，可在任意任务中调用）
     */
    void EnterWifiConfigMode();
    
    /**
     * 判断当前是否处于 Wi-Fi 配网模式
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
