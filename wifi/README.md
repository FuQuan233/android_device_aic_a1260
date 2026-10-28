# A1260 / MT6580 Wi-Fi

Linux 6.18 使用 MT6580 片内 AHB Gen2 驱动，WMT/STP 通过 BTIF 启动连接 MCU。
Android 使用源码 AIDL Wi-Fi 服务、本站点 nl80211 HAL、wificond 和现代 wpa_supplicant。
当前仅支持 2.4GHz STA；不声明 AP/P2P、WPA3/PMF、PNO、RTT 或高级统计能力。

原厂镜像与本机 RF 校准不放进 Git。在 Android 源码根目录运行：

```sh
python3 device/aic/a1260/tools/prepare_stock_wifi.py \
    /path/to/stock/system vendor/aic/a1260-wifi \
    --nvram /path/to/this-device/WIFI
```

WIFI 记录接受原始 514 字节或其中前 512 字节；RF 数据保持原样。
空 MAC 字段不会被当作整份校准无效：内核优先使用有效的 NIC 地址，否则使用明确标记的本地随机地址。
当前没有动态修改 MAC 的固件确认实现，因此不广告随机化能力；持久化缺省本地地址仍可后续完善。
ROM 补丁和 WLAN 固件校验指定原厂文件 SHA256；控制程序 `mt6580-wmt-dispatcher`
从 `wifi/wmt/launcher.c` 构建，不运行原厂会长期持有 loopback 电源引用的 launcher。

开关由标准 DriverTool 写 `/dev/wmtWifi` 的 1/0，开启时才真实下载固件。
HAL 在 `/vendor` 挂载后重新加载带签名的 regulatory.db，再查询实际 wiphy 信道与 STA 能力。
配置和权限见 `wifi.mk`、`init.mt6580.wifi.rc` 和 `sepolicy/vendor/mt6580_wmt.te`。

## 已定位的硬件前提

- CONN 域、TOPRGU MCU reset、BTIF 门控及 MT6350 VCN18/28/33 由内核拥有。
- 初始化期间按原厂 EFUSE 窗口开启 VCN28；真实读取校准等级并设置两路 VCN33。
- EMI MPU region5 覆盖预留内存前 512KiB，仅 CONN 可访问，权限 0xa2d。
- region6 覆盖后 512KiB，AP/CONN 可访问，权限 0xa28。保留相邻 region4/7。
- 缺少上述窗口时，ROM 仍可能返回下载 ACK，但补丁/固件无法落入 DDR。
- BTIF 唤醒遵循原 BSP：WAKE 拉低 64–96 微秒后拉高，随后等待真实固件响应。

## 实机验证

2026-10-28 / 内核 #191：冷启动即由 Android 启动驱动，无手工 MMIO 设置；
原厂两段 ROM 补丁、真实 EFUSE 等级 2 / VCN33 3.5V、RF 校准和 WLAN Ready 均成功。
Android 与直接 nl80211 扫描均收到 10 个 BSS。设置界面完成 WPA2 连接与 DHCP，
DNS、外网 ICMP 和 HTTPS 200 已验证；稳定阶段网关/外网 ping 各 5 次无丢包，收发错误为 0。
空闲后关闭 Wi-Fi 确认 CONN 域与 Wi-Fi LDO 关闭，再开启成功，MPU 违例为 0。
MIUI 探测通过后，已保存热点的自动重连、重新 DHCP 和三次网关 ping（零丢包）也通过。

用户指定联网探测使用 `http(s)://connect.rom.miui.com/generate_204`；实机 HTTP/HTTPS 均返回204，
NetworkMonitor 已判定成功。NTP 使用经用户确认的 `time.apple.com`，实机正常获取时间。
`ntp.apple.com` 没有可用解析，未沿用该拼写。NetworkStack/framework 资源覆盖提供源码默认值，
启动时的一次性 `network-defaults.sh` 也为已有 bring-up 镜像设置尚未指定的默认值，保留后续用户修改。
当前镜像通过正常网络探测，不伪造已验证状态或绕过 TLS。

完整 BTIF CCF 停钟、连接状态下更深的系统休眠、长时间吞吐及 MAC 持久化仍待独立资格化。
当前 Wi-Fi 开启时 SPM 保留 CONN 所需资源；关闭后释放。SELinux 仍沿用设备整体 permissive 策略，
新增规则已通过 neverallow 检查，不代表其他硬件策略已完成。

最终 #192 冷启动再次自动连接已保存热点，框架识别为 802.11n/65Mbps，
MIUI探测及Apple自动校时再次成功；默认普通shell ADB、四核schedutil/604500–1300000、deep和stay-on0保留。
标准产品 boot.img/kernel/vendor.img 已同步本轮实现。
