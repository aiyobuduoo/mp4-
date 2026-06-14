# ESP32-P4 Multimedia Desktop

基于 ESP-IDF 和 LVGL 的 ESP32-P4 多媒体桌面项目，集成音乐播放、时间天气、
历史上的今天、网页备忘录、WiFi 配网以及 USB 扩展屏功能。

## 主要功能

- LVGL 触摸界面，支持启动页、时间页和音乐播放页
- 从 SD 卡扫描并播放 MP3，显示专辑封面、歌词、播放进度和歌曲信息
- ES8311 音频编解码器与音量控制
- 连接 WiFi 前停留在启动页面，支持热点网页配网
- 从阿里云 NTP 服务器同步时间
- 开机获取天气，之后每 8 小时更新一次
- 每日获取一次“历史上的今天”
- 通过设备网页管理备忘录，并将内容保存在 NVS
- USB 扩展屏模式，支持 JPEG 硬件解码、PPA 旋转和 HID 触摸

## 硬件与软件

- 目标芯片：ESP32-P4
- ESP-IDF：`>=5.4,<5.6`
- 显示：ST7701，原生分辨率 `480x800`
- 触摸：GT911
- 音频：ES8311
- 存储：SDMMC SD 卡
- 图形库：LVGL 8

硬件引脚、屏幕方向、触摸参数、SDMMC 和音频参数集中定义在
[`main/config.h`](main/config.h)。移植到其他开发板前，请先核对这里的配置。

## 项目结构

```text
main/
├── historyfl/          历史上的今天
├── memo/               NVS 备忘录与网页管理服务
├── music/              MP3 播放、封面、歌词与进度控制
├── time/               NTP 同步与时间界面更新
├── ui/                 LVGL 生成的界面和资源
├── usb_extend_screen/  USB 扩展屏与 HID 触摸
├── weatherfl/          天气接口与定时更新
├── lcd.c               LCD、背光与触摸初始化
├── sd.c                SD 卡挂载
└── wifi.c              WiFi 连接与网页配网
```

## SD 卡目录

SD 卡挂载点为 `/sdcard`，当前功能使用以下目录：

```text
/sdcard/music/   MP3 音乐文件
/sdcard/geci/    LRC 歌词文件
/sdcard/MJPEG/   MJPEG 视频文件
```

## WiFi 配网

源码默认不保存 WiFi 名称和密码。设备没有可用凭据或连接失败时，会启动配网热点：

- 热点名称：`SW-ROA-Setup`
- 配网页面：`http://192.168.4.1/`

连接成功后，凭据会保存在设备 NVS 中。请勿将真实 WiFi 密码、API 密钥或证书提交到
Git 仓库。

设备联网后，串口日志会打印设备 IP 和备忘录页面地址。备忘录服务默认端口为
`8080`，例如：

```text
http://设备IP:8080/
```

## 构建与烧录

先安装并激活兼容版本的 ESP-IDF，然后在项目根目录执行：

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

本项目使用自定义分区表 [`partitions.csv`](partitions.csv)，并在仓库中保留了当前
开发板使用的 `sdkconfig`。烧录前请确认芯片版本、Flash 配置和硬件引脚与目标设备一致。

## USB 扩展屏

时间页面向下滑动可打开进入扩展屏的确认框；扩展屏工作时再次向下滑动可退出并恢复
LVGL 界面。

Windows 端需要为 `USB\VID_303A&PID_2986` 安装或绑定 Espressif IDD 驱动。
详细说明见 [`main/usb_extend_screen/README.md`](main/usb_extend_screen/README.md)。

## 相关接口

- 天气：[UAPI 天气接口](https://uapis.cn/docs/api-reference/get-misc-weather)
- 历史上的今天：[UAPI 程序员历史上的今天](https://uapis.cn/docs/api-reference/get-history-programmer-today)
- ESP-IDF：[Espressif Programming Guide](https://docs.espressif.com/projects/esp-idf/)

## 注意事项

- `build/` 和 `managed_components/` 为本地生成目录，不提交到仓库。
- `components/` 中包含项目当前依赖的本地组件，克隆后可直接使用。
- 修改 SquareLine/LVGL 导出的 UI 文件后，请同步维护 `main/ui/filelist.txt`。
- USB 扩展屏会暂停 LVGL 渲染，退出扩展屏后再恢复主界面。
