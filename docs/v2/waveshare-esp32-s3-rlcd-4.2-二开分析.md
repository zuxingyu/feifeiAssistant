# waveshare-esp32-s3-rlcd-4.2 二次开发分析

## 1. 文档目的

本文档基于当前仓库代码现状，对 `waveshare-esp32-s3-rlcd-4.2` 设备的二次开发进行梳理，重点分析以下三块内容：

1. 配网模式
2. 激活设备
3. 主页与设备端页面

目标是帮助后续开发时快速找到正确的代码入口、明确现有能力与缺口，并给出较稳妥的改造顺序。

---

## 2. 项目整体结构

项目是基于 `xiaozhi-esp32` 的 ESP-IDF 固件工程，主干结构可以分为以下几层：

### 2.1 顶层目录

- `main/`
  - 主要业务代码，包含应用主流程、板级适配、显示、音频、协议、OTA 等
- `docs/`
  - 项目文档、需求、协议说明和原型
- `scripts/`
  - 构建脚本、资源生成脚本等

### 2.2 `main/` 下的核心模块

- `application.cc / application.h`
  - 应用总入口，负责初始化、状态机驱动、网络事件处理、激活、协议初始化
- `boards/common/`
  - 板级公共抽象，如 `Board`、`WifiBoard`、按钮、电池、背光等
- `boards/waveshare/esp32-s3-rlcd-4.2/`
  - 本次目标板的硬件适配代码
- `display/`
  - 显示抽象和 LVGL UI 实现
- `audio/`
  - 音频编解码、语音处理、唤醒相关
- `protocols/`
  - 与服务端通信协议，支持 MQTT / WebSocket
- `ota.cc / ota.h`
  - 固件检查、配置下发、激活、升级
- `assets/`
  - 多语言、音效、资源等

---

## 3. 启动主流程

当前项目的设备启动主线如下：

1. `Application::Initialize()`
   - 获取 `Board`
   - 初始化显示 `display->SetupUI()`
   - 初始化音频服务
   - 注册状态变化监听
   - 注册网络事件回调
   - 调用 `board.StartNetwork()`
2. `Board::StartNetwork()`
   - 对于 WiFi 板，进入 `WifiBoard::StartNetwork()`
3. `WifiBoard::StartNetwork()`
   - 初始化 `WifiManager`
   - 尝试连接历史 WiFi
   - 若没有历史 WiFi，则进入配网模式
4. 网络连通后
   - `Application::HandleNetworkConnectedEvent()`
   - 状态切换到 `kDeviceStateActivating`
   - 启动 `ActivationTask()`
5. 激活完成后
   - 初始化 MQTT/WebSocket 协议
   - 设备进入 `kDeviceStateIdle`
   - 显示待命界面

可以把它理解成：

`开机 -> 显示初始化 -> 联网/配网 -> 激活 -> 协议上线 -> 待命主页`

---

## 4. 板级适配结构

### 4.1 当前板型入口

目标板代码目录：

- `main/boards/waveshare/esp32-s3-rlcd-4.2/`

关键文件：

- `waveshare-s3-rlcd-4.2.cc`
- `custom_lcd_display.cc`
- `custom_lcd_display.h`
- `config.h`
- `config.json`

### 4.2 板级类关系

当前板型类定义在 `waveshare-s3-rlcd-4.2.cc` 中：

- `CustomBoard : public WifiBoard`

这说明该板是建立在 WiFi 公共板卡能力之上的，天然继承：

- WiFi 连接
- SoftAP 配网
- 网络状态回调
- WiFi 状态栏图标逻辑

### 4.3 当前板级已经实现的能力

`CustomBoard` 当前已经接入：

- I2C 初始化
- 音频 Codec 初始化
- RLCD 屏幕初始化
- 单个 `BOOT` 按钮事件
- 读取电池电量
- MCP 工具“重新配网”

### 4.4 当前板级的硬件限制与注意点

从 `config.h` 和现有板级代码看，当前已明确接入的信息有：

- 分辨率：`400 x 300`
- 屏幕类型：RLCD
- 颜色：当前渲染实际按黑白处理
- 按钮：硬件已确认有 3 个物理按键，但当前代码里只接了 `BOOT_BUTTON_GPIO`

这里有一个很重要的现状差异：

- 需求文档里写的是三个物理按键 `A/B/C`
- 硬件实际也确认是三个物理按键
- 但当前 `waveshare-s3-rlcd-4.2.cc` 只实现了一个 `boot_button_`
- 没有看到第二、第三个按键的 GPIO 配置和事件逻辑

这意味着后续如果要做：

- A 键翻页
- B 键唤醒/弹出对话
- C 键返回主页

下一步需要补的是另外两个按键的 GPIO 定义与事件映射，把三颗实体按键完整接入 `A/B/C` 的页面交互逻辑。

---

## 5. 显示与 UI 架构

### 5.1 显示抽象层次

当前显示相关大致分为三层：

1. `Display`
   - 抽象基类
2. `LvglDisplay`
   - 提供状态栏、通知、网络/电池图标刷新等通用能力
3. `LcdDisplay`
   - 提供 LCD 场景下的具体 UI 布局
4. `CustomLcdDisplay`
   - 针对 RLCD 的刷屏和驱动适配

### 5.2 `Display` 的职责

`Display` 只定义统一接口，例如：

- `SetStatus()`
- `ShowNotification()`
- `SetEmotion()`
- `SetChatMessage()`
- `ClearChatMessages()`
- `SetTheme()`
- `UpdateStatusBar()`
- `SetupUI()`

这意味着后续做页面开发时，最好尽量基于已有显示对象扩展，而不是绕开这层接口直接散落写 UI。

### 5.3 `LvglDisplay` 的职责

`LvglDisplay::UpdateStatusBar()` 负责统一刷新：

- 静音图标
- 时间/状态
- 电池图标
- 网络图标

其中网络图标来自：

- `Board::GetNetworkStateIcon()`

对于 WiFi 板，最终由：

- `WifiBoard::GetNetworkStateIcon()`

返回 WiFi 信号图标或配网图标。

### 5.4 `LcdDisplay` 的当前页面形态

当前 `waveshare-esp32-s3-rlcd-4.2` 的显示并不是“多页面设备 UI”，而是一个通用的小智聊天界面。

当前主要 UI 组成是：

- 顶部状态栏
- 中间表情/图像区
- 底部对话条或聊天内容区

现有 `LcdDisplay::SetupUI()` 更接近“通用 AI 设备界面”而不是你需求中的：

- 首页
- 音乐页
- 课程表页
- 天气页

### 5.5 当前板子的字体问题

在 `main/CMakeLists.txt` 中，`CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_RLCD_4_2` 使用的是：

- `BUILTIN_TEXT_FONT = font_puhui_basic_30_4`
- `BUILTIN_ICON_FONT = font_awesome_30_4`

这套字号对 `400 x 300` 单色 RLCD 来说偏大，会直接带来：

- 标题和标签容易被截断
- 顶部状态栏占高过多
- 同一行布局空间不足
- 中文字段如“WiFi名称”“访问地址”更容易裁切

这也是后续做主页和配网页面时必须优先处理的基础问题之一。

---

## 6. 配网模式现状分析

### 6.1 当前已实现的配网链路

当前配网主入口在：

- `main/boards/common/wifi_board.cc`

关键流程如下：

1. `WifiBoard::StartNetwork()`
   - 初始化 `WifiManager`
   - 尝试连接历史 WiFi
2. `WifiBoard::TryWifiConnect()`
   - 如果已有 SSID，则进入 STA 连接
   - 如果没有 SSID，则延时后进入配网模式
3. `WifiBoard::StartWifiConfigMode()`
   - 设置状态为 `kDeviceStateWifiConfiguring`
   - 启动 `wifi_manager.StartConfigAp()`
   - 通过 `Application::Alert()` 显示配网提示

### 6.2 当前配网模式的实际表现

目前仓库里的“配网模式”本质上是：

- 设备开启 SoftAP 热点
- 屏幕显示一条提示消息
- 提示用户连接热点并访问浏览器地址

屏幕提示内容来自：

- `Lang::Strings::CONNECT_TO_HOTSPOT`
- `wifi_manager.GetApSsid()`
- `Lang::Strings::ACCESS_VIA_BROWSER`
- `wifi_manager.GetApWebUrl()`

然后通过：

- `Application::Alert(...)`

把内容展示到现有聊天/状态 UI 中。

### 6.3 当前配网模式没有实现的内容

根据当前代码搜索结果，仓库里没有发现面向这块板子现成的以下实现：

- 配网页面 HTTP 路由
- `setup-wifi.html` 对应的设备内置网页服务
- `setup-schedule.html` 对应保存逻辑
- `setup-weather.html` 对应保存逻辑
- `setup-done.html` 对应完成页逻辑
- `/api/save` 之类的配网接口

也就是说，`docs/v2` 中原型描述的“四步网页配网流程”，在当前代码里还没有落地。

### 6.4 结论

当前项目“有 SoftAP 配网入口”，但“没有完整网页配网流程”。

如果你要开发配网模式，实际上要做的是两层工作：

1. 设备屏首屏
   - 在 RLCD 上显示热点名和访问地址
2. 配网页服务
   - 在设备里新增 HTTP Server
   - 提供 4 个页面和若干 API
   - 保存 WiFi、课程表、天气等配置到本地存储

### 6.5 推荐改造入口

配网模式建议重点改这几个位置：

- `main/boards/common/wifi_board.cc`
  - 进入配网模式、退出配网模式、连接成功后的收口
- `main/display/display.h`
  - 如果要增加“设备配网页”接口，可从这里扩展
- `main/display/lcd_display.h/.cc`
  - 实现设备端配网引导页
- `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.*`
  - 处理 RLCD 特殊布局和黑白显示效果
- 新增模块，建议单独建目录
  - `main/web/` 或 `main/provisioning/`
  - 放 HTTP Server、路由、配置持久化逻辑

---

## 7. 激活设备现状分析

### 7.1 当前“激活设备”不是独立页面系统

现有项目里的“激活”主要发生在联网之后，由 `Application + Ota` 完成，不是单独的 UI 页面框架。

主流程如下：

1. 设备联网成功
2. `Application::HandleNetworkConnectedEvent()`
3. 状态切换到 `kDeviceStateActivating`
4. 启动 `ActivationTask()`
5. `ActivationTask()` 内依次执行：
   - `CheckAssetsVersion()`
   - `CheckNewVersion()`
   - `InitializeProtocol()`

### 7.2 当前激活的核心逻辑

激活主要在：

- `main/ota.cc`

其关键能力包括：

- 调用 OTA 配置接口
- 从服务端获取：
  - 固件版本信息
  - MQTT 配置
  - WebSocket 配置
  - activation 信息
  - server_time
- 解析 `activation.code`
- 解析 `activation.challenge`
- 调用 `/activate` 接口完成激活

### 7.3 当前激活界面的表现

现有 UI 表现并不是“绑定页”或“设备激活页面组件”，而是复用：

- `Application::Alert()`
- `Display::SetStatus()`
- `Display::SetChatMessage()`
- `Display::SetEmotion()`

如果服务端返回激活码：

- `Application::ShowActivationCode()`

会播放语音并在现有界面里提示激活信息。

### 7.4 与你的需求差异

你的需求中，激活更像是：

- 配网完成后进入主页
- 屏幕中部显示“小智号码/绑定码”
- 用户在小智控制台输入号码完成绑定
- 成功后才进入真正主页

而当前项目更接近：

- 联网后自动与 OTA/配置服务交互
- 若需要激活，展示激活提示
- 然后初始化协议

也就是说，当前仓库“有激活流程”，但“没有你想要的设备绑定展示页”。

### 7.5 推荐改造思路

建议把“激活设备”拆成两部分：

1. 网络/协议层仍然复用当前 `Ota` 机制
   - 不要推翻现有 `CheckVersion()/Activate()` 流程
2. UI 层新增“绑定中页面”
   - 在 `kDeviceStateActivating` 阶段显示更明确的儿童化页面
   - 中间展示绑定码、操作提示、联网状态
   - 绑定完成后切到主页

推荐改造入口：

- `main/application.cc`
  - `HandleNetworkConnectedEvent()`
  - `HandleActivationDoneEvent()`
  - `ShowActivationCode()`
  - `Alert()`
- `main/ota.cc`
  - 激活码、challenge 的接收逻辑
- `main/display/lcd_display.cc`
  - 增加激活态页面的显示逻辑

---

## 8. 主页与设备页面现状分析

### 8.1 当前没有你需求中的四个设备页面

根据当前仓库代码搜索结果，没有找到以下现成模块：

- `page_home.cc`
- `page_music.cc`
- `page_schedule.cc`
- `page_weather.cc`
- `STATE_HOME`
- `STATE_MUSIC`
- `STATE_SCHEDULE`
- `STATE_WEATHER`

说明当前项目还没有“页面式”的设备 UI 架构。

### 8.2 当前主页实际是什么

当前所谓“主页”更准确地说是“小智主待机界面”，包含：

- 顶部状态栏
- 中间表情或图片
- 底部对话内容

待机时主要显示：

- 状态文本
- 表情
- 小智聊天消息

它适合通用语音助手，但不适合直接承载你要的：

- 时间、日期
- 三日天气
- 今日/明日课程
- 绑定码
- 多页面切换

### 8.3 当前按钮交互不足以支撑多页面

当前 `waveshare-s3-rlcd-4.2.cc` 中只看到：

- 单击 `boot_button_`
  - 启动阶段进入配网
  - 其他阶段调用 `app.ToggleChatState()`
- 双击 `boot_button_`
  - 切换 AEC

没有看到：

- A 键翻页
- B 键唤醒
- C 键返回主页

因此主页开发不能只改 UI，还需要同步补“页面状态”和“按键事件分发”。

### 8.4 当前状态栏能力

现有状态栏已经有一部分能力可复用：

- WiFi 图标
- 电池图标
- 静音图标
- 时间/状态文本

来源主要是：

- `LvglDisplay::UpdateStatusBar()`

这部分适合保留，继续扩展。

### 8.5 当前缺失的数据能力

现有代码对主页需求里的数据支持不完整：

- 温度
  - 框架有 `Board::GetTemperature(float&)`
  - 但本板 `waveshare-esp32-s3-rlcd-4.2` 没有重写实现
- 湿度
  - 当前代码中没有看到通用湿度抽象或接口
- 课程表
  - 当前仓库没有现成课程表数据模块
- 天气
  - 当前仓库没有针对你这套原型的天气页面和天气数据缓存模块
- 近三日天气/四日趋势图
  - 当前没有现成绘制逻辑

因此主页开发不是“微调当前页面”，而是“新增页面框架和业务数据层”。

---

## 9. 当前代码与需求的差距总结

### 9.1 已有能力

当前仓库已经具备：

- ESP-IDF 工程基础
- Waveshare RLCD 4.2 的板级驱动
- 音频输入输出
- WiFi 联网
- SoftAP 配网入口
- OTA 配置与激活
- 通用 LCD/LVGL 显示框架
- 顶部状态栏基础能力

### 9.2 明显缺口

和你的需求相比，当前主要缺口有：

1. 缺少完整的四步网页配网流程
2. 缺少设备端专用配网引导页
3. 缺少绑定码/激活页
4. 缺少真正意义上的主页
5. 缺少音乐/课程表/天气三类页面
6. 缺少页面状态管理和翻页逻辑
7. 缺少 3 个物理按键在代码中的完整接入
8. 缺少课程表数据模型
9. 缺少天气数据模型和接口层
10. 当前默认字体偏大，不适合儿童向 400x300 黑白界面

---

## 10. 建议的二开方案

建议不要直接在现有 `LcdDisplay` 里继续堆逻辑，而是采用“保留底层，新增页面层”的方式。

### 10.1 推荐分层

建议新增以下模块：

- `main/ui/`
  - 页面管理器
  - 页面状态枚举
  - 公共状态栏
  - 公共对话条
- `main/ui/pages/`
  - `page_setup_start.*`
  - `page_activation.*`
  - `page_home.*`
  - `page_music.*`
  - `page_schedule.*`
  - `page_weather.*`
- `main/services/`
  - `schedule_service.*`
  - `weather_service.*`
  - `device_bind_service.*`
- `main/web/`
  - `config_http_server.*`
  - `config_routes.*`
  - `config_storage.*`

### 10.2 推荐开发顺序

#### 第一阶段：先把配网首屏做稳

目标：

- 开机无 WiFi 时，屏幕正确显示热点名和访问地址
- 顶部状态栏不被覆盖
- 中文标签不截断

优先改：

- `wifi_board.cc`
- `lcd_display.h/.cc`
- `custom_lcd_display.*`

#### 第二阶段：补网页配网服务

目标：

- 手机/电脑连接热点后，可打开设备网页
- 完成 WiFi、课程表、天气配置保存

建议新增：

- `main/web/`

#### 第三阶段：补激活页

目标：

- 联网成功后在屏幕中部明确显示绑定码
- 绑定成功前后有明确视觉状态

优先改：

- `application.cc`
- `ota.cc`
- 页面层

#### 第四阶段：搭建设备页面框架

目标：

- 首页
- 音乐页
- 课程表页
- 天气页

建议先建立：

- 页面枚举
- 页面管理器
- 公共状态栏
- 公共底部对话条

#### 第五阶段：补按钮导航和数据刷新

目标：

- A/B/C 事件稳定
- 页面切换稳定
- 每分钟刷新策略落地

---

## 11. 建议优先修改的文件清单

### 11.1 一定会动到的现有文件

- `main/application.cc`
- `main/application.h`
- `main/boards/common/wifi_board.cc`
- `main/boards/common/board.h`
- `main/display/display.h`
- `main/display/lcd_display.h`
- `main/display/lcd_display.cc`
- `main/display/lvgl_display/lvgl_display.cc`
- `main/boards/waveshare/esp32-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc`
- `main/boards/waveshare/esp32-s3-rlcd-4.2/custom_lcd_display.cc`
- `main/boards/waveshare/esp32-s3-rlcd-4.2/config.h`
- `main/CMakeLists.txt`

### 11.2 建议新增的文件

- `main/ui/page_manager.h`
- `main/ui/page_manager.cc`
- `main/ui/pages/page_setup_start.h`
- `main/ui/pages/page_setup_start.cc`
- `main/ui/pages/page_activation.h`
- `main/ui/pages/page_activation.cc`
- `main/ui/pages/page_home.h`
- `main/ui/pages/page_home.cc`
- `main/ui/pages/page_music.h`
- `main/ui/pages/page_music.cc`
- `main/ui/pages/page_schedule.h`
- `main/ui/pages/page_schedule.cc`
- `main/ui/pages/page_weather.h`
- `main/ui/pages/page_weather.cc`
- `main/web/config_http_server.h`
- `main/web/config_http_server.cc`
- `main/services/schedule_service.h`
- `main/services/schedule_service.cc`
- `main/services/weather_service.h`
- `main/services/weather_service.cc`

---

## 12. 开发风险与注意事项

### 12.1 字体与排版风险

当前默认大字号会严重影响：

- 状态栏
- 配网页
- 中文字段
- 多栏布局

建议尽早决定是：

- 调整全局字体
还是
- 页面局部单独设置字体

### 12.2 RLCD 刷新性能风险

本板是 RLCD 黑白刷新，频繁整屏重绘可能带来：

- 刷新慢
- 闪烁感
- CPU 占用偏高

建议：

- 顶部状态栏和主体内容尽量局部刷新
- 页面切换时再做较大范围重绘

### 12.3 按键方案风险

硬件已确认有 3 个物理按键，但当前代码只接了 1 个。

如果不把另外两个按键的 GPIO 和事件补进板级代码，页面流转逻辑就无法真正落地。

### 12.4 数据层缺口风险

主页、课程表、天气详情都依赖本地数据模型，但当前仓库并没有这些模块，需要新增而不是硬改显示层。

---

## 13. 最终结论

基于当前仓库现状，可以明确得出以下判断：

1. 这个项目已经具备稳定的底座能力
   - 板级驱动、联网、激活、音频、基础显示都可复用
2. 你要开发的内容并不是“简单改界面”
   - 而是需要新增一层设备页面系统和网页配网系统
3. `waveshare-esp32-s3-rlcd-4.2` 当前最适合作为二开的切入点是：
   - 保留 `WifiBoard + Application + Ota + LcdDisplay` 主干
   - 在其上扩展“配网页、激活页、主页、多页面管理器”
4. 当前最先要解决的基础问题是：
   - 页面结构
   - 按键映射
   - 字体排版
   - 配网页服务

如果下一步要正式进入开发，建议按下面顺序推进：

1. 先完成设备端配网首屏
2. 再补四步网页配网
3. 再做激活页
4. 最后做主页、音乐、课程表、天气四页与按键流转
