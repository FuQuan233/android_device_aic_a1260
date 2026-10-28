# A1260 / MT6580 Wi-Fi

Linux 6.18 使用 MT6580 片内 AHB Gen2 驱动，WMT/STP 通过 BTIF 启动连接 MCU。
Android 使用源码 AIDL Wi-Fi 服务、本站点 nl80211 HAL、wificond 和现代 wpa_supplicant。
当前仅支持 2.4GHz STA；不声明 AP/P2P、WPA3/PMF、固件 PNO、RTT 或高级统计能力。

原厂镜像与本机 RF 校准不放进 Git。在 Android 源码根目录运行：

```sh
python3 device/aic/a1260/tools/prepare_stock_wifi.py \
    /path/to/stock/system vendor/aic/a1260-wifi \
    --nvram /path/to/this-device/WIFI
```

WIFI 记录接受原始 514 字节或其中前 512 字节；RF 数据保持原样。
空 MAC 字段不会被当作整份校准无效：有效 NVRAM MAC 优先，否则使用早期初始化提供的原厂 MAC、
有效 NIC 地址，最后才使用本次开机缓存的本地随机地址。原厂地址读取方式见下节。
当前没有动态修改 MAC 的固件确认实现，因此不广告随机化能力。
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

连接状态下更深的系统休眠、长时间吞吐仍待独立资格化。
当前 Wi-Fi 开启时 SPM 保留 CONN 所需资源；关闭后释放。SELinux 仍沿用设备整体 permissive 策略，
新增规则已通过 neverallow 检查，不代表其他硬件策略已完成。

最终 #192 冷启动再次自动连接已保存热点，框架识别为 802.11n/65Mbps，
MIUI探测及Apple自动校时再次成功；默认普通shell ADB、四核schedutil/604500–1300000、deep和stay-on0保留。
标准产品 boot.img/kernel/vendor.img 已同步本轮实现。

## BTIF 空闲停钟（#193）

按旧 BSP 的 TX 排空→关 BTIF CCF 时钟、恢复时钟→请求固件唤醒顺序实现，
没有用 SLEEP_EN 位代替真实门控。IRQ 同步后才停钟；未读 RX 或尚未结束的 STP 解析会保留时钟，
关闭、调试和发送路径统一保护，避免访问已停钟寄存器。只读 `btif_state` 提供缓存状态与统计，
读取本身不唤醒 BTIF；物理门控由 CCF 和 TOPCG_CTRL1 bit12 独立核对。

实机已连接时 prepare/enable=0/0、硬件门控 N，TOPCG_CTRL1 bit12 为 1；
保持停钟状态下网关/外网 ping 各5次零丢包，HTTPS200。关闭 Wi-Fi 时先恢复 BTIF 并完成真实固件唤醒，
随后 CONN/LDO 释放；再次开启关联/DHCP 后重新停钟。RX 软件溢出/硬件 overrun、停钟超时和 MPU 违例均为0。
初次联网探针早于开机 DHCP，当时无路由；保留失败原始记录，不计入通过样本。

另发现熄屏且断网时框架仍请求尚未支持的硬件 PNO，导致重试；亮屏后普通扫描和重连正常。
该 #193 阶段软件 PNO/熄屏重连尚未接入；后续结果见下节，未为绕过问题声明虚假固件能力。

实际拔线/熄屏/插回后网络重新验证成功，网关5包零丢包，但系统挂起未通过：success=0/fail=3，
其中两次设备挂起阶段被 `WLAN TX THREAD` 取消。当前不广告未验证的 WoWLAN，cfg80211 因此在挂起时
主动断开，断开 OID 又触发 TX 线程唤醒锁；旧 AHB platform suspend/resume 回调本身为空。
下一步需完成 cfg80211/WoWLAN、固件挂起/所有权和唤醒 IRQ 的真实配合；本轮不记为“保持连接深睡通过”。
恢复后 BTIF 仍停钟，溢出/超时/MPU违例和 taint 均为0。记录见 `out/a1260-runtime-pm/wifi-power/`。

## 保持关联的保留待机

内核的 cfg80211 PM 桥接仅提供标准 WoWLAN `any`：保持正常收发，以真实 AHB HIF IRQ 唤醒 AP。
supplicant overlay 设置 `wowlan_triggers=any`，避免 #193 在挂起时被 cfg80211 主动断开。
挂起开启 IRQ wake，恢复（包括其他设备取消挂起的回滚）及释放 IRQ 时配平引用。
固件 sleepy 事件和原有所有权握手继续生效，不强制伪造 firmware-own；只读 netdev `pm_state` 记录相关状态。
不接未验证的过滤/GTK/ARP offload，也不调用有 IPv6 类型问题的旧系统挂起助手。
现有 CONN/DDR 保护保留，因此该模式不等同于联网时关闭 DDR/infra/MAINPLL。

#194 实机拔线/熄屏/插回通过：7次实际系统挂起，cfg80211挂起/恢复7/7，IRQ99 `AHB_SLAVE_HIF`
被内核明确记录为7次唤醒来源；不是只统计回调。入睡前firmware-own=1、sleepy=1、flags=0、
WHLPCR=0x1，未强制改所有权。SPM为state3/cpu0_mode0/fallback1/r0 blocker0x4，确认CONN保护按预期保留DDR。
拔线瞬间1次mt6350-usb待处理唤醒取消，WLAN TX THREAD取消不再出现。
各阶段IPv4一致，未因挂起产生carrier-off；恢复后VALIDATED、网关5包零丢包、HTTPS200，收发错误0。
IRQ wake已配平解除，BTIF仍停钟、MPU违例和taint为0。ANY会因正常无线活动唤醒，
选择性过滤、更深的联网断电及长时待机功耗尚待后续资格化。
随后关开Wi-Fi，CONN/LDO释放、保存网络重连及新wiphy的ANY配置恢复，网关3包零丢包、错误0。
诊断须按netdev的phy80211链接查当前wiphy，不能在开关后继续硬编码phy0。
标准boot/kernel/vendor已同步#194，测试脚本清理并恢复普通shell ADB。

## 熄屏断网后的软件扫描回退

`A1260WifiOverlay` 是 vendor 静态 RRO，仅覆盖 Wi-Fi 模块明确允许设备定制的
`config_wifiSwPnoEnabled`。一次性 `network-defaults.sh` 在
`wifi/software_pno_enabled` 尚未配置时设为true，保留显式的后续自定义值。
这两个开关缺一不可；资源覆盖不修改 Wi-Fi APEX，也不把普通扫描声明为固件 PNO。

硬件 PNO 不可用时，系统 `WifiScanningService` 使用其现有 `SwPnoScanState`，
通过真实单次扫描寻找保存网络。沿用系统原有移动/静止、快速、慢速退避参数，
没有添加轮询守护进程、永久唤醒锁或高频重扫周期；连接建立或请求结束时由系统取消扫描闹钟。
内核继续使用已验证的 #194，WoWLAN ANY 和 CONN/DDR 保留策略保持一致。

初轮实机验证（2026-10-28）：普通重启后 vendor RRO 自动启用，资源查询与 DeviceConfig 均为true；
未修改 Wi-Fi APEX 或默认 ADB 权限。设备全程 `mWakefulness=Asleep`，正常关开 Wi-Fi 后框架日志
明确进入 `Starting Software PNO`，按原参数安排下一次20000ms扫描，随即通过真实扫描找到保存网络，
自动关联/DHCP成功；网关5包零丢包、HTTPS200、收发错误/MPU/taint0。
该轮USB保持连接，只证明熄屏重连；定时重扫跨真实待机另行记录。

后续物理拔线/热点关闭再开启/插回通过：其他保存网络的自动连接临时关闭，避免切回另一热点；
完成后按原值恢复。热点丢失后软件PNO先后安排20000ms、40000ms重扫，AlarmManager确实记录2次
软件PNO唤醒；在USB仍断开且wakefulness=Asleep时已连接回同一测试网络。
连接后尚待执行的300000ms重扫被系统取消，记录中的相关闹钟只剩取消历史与统计。
实际完成6次保留待机，另2次被临近的alarmtimer以-EBUSY取消；cfg80211挂起/恢复8/8、IRQ wake解除、错误0。
插回后网络VALIDATED、网关5包零丢包、HTTPS200，收发错误/MPU/taint0；脚本COMPLETE/EXIT_STATUS0。
这补齐了熄屏断网及跨待机的定时重扫，不等于已完成联网DDR/infra/MAINPLL关闭或长期功耗标定。

## 原厂序列号与 MAC（#197）

早期备份的 proinfo 与实机 SHA256 一致；通过只读提取同一备份的 nvdata，
确认 `proinfo+0x6e` 的六字节对应 `APCFG/APRDEB/WIFI+4`，
`+0x68` 对应 `BT_Addr+0`。序列号按原厂 LK 的 `+0x74`、最多19字节读取，
遇 NUL 停止，仅接受 ASCII 字母数字，不把填充或二进制 MAC 拼进字符串。
蓝牙地址只作布局确认，本轮未接蓝牙功能。

`post-fs-data` 等待 proinfo 和 Wi-Fi 平台节点，再由现有 USB 初始化服务只读解析。
原厂 Wi-Fi MAC 在第一次开无线前写入内核的一次性 `factory_mac_address`，
序列号提供给 `ro.boot.serialno`、`ro.serialno`、`sys.usb.serialnumber` 和 USB iSerial。
不再从 eMMC CID 派生身份，也不回写 proinfo/NVRAM/RF 数据；SELinux 只新增相应读取/配置权限，
整机 permissive 的既有限制仍在，未宣称完整 enforcing 资格化。

实测两次完整无线关开（CONN 关闭再开启）和随后正常重启，原厂 MAC、序列号保持不变，
均自动关联/DHCP；四个联网快照各网关5包无丢包、HTTPS200、收发错误/taint为0。
无效 MAC 写入返回 EINVAL，运行后换地址返回 EBUSY，重复同值允许。
原厂分区与已安装 RF 校准 SHA256 未变。主机 ADB 原始字节与 USB iSerial 均为原厂16个可打印字符；
此前用户遇到的不可见字符未在修复前独立复现，不推断它必然由 EP0 包边界引起。

## Android 当前 MAC 查询

AIDL supplicant 的 `getMacAddress()` 走文本 `MACADDR` 驱动命令；
通用 stub 未提供此命令，导致内核已使用原厂地址而 Android `WifiInfo` 仍是 `MAC: <none>`。
BoardConfig 选择设备内的 `lib_driver_cmd_mt6580`，通过标准 `SIOCGIFHWADDR`
读取当前接口并返回 supplicant 要求的文本格式。没有向固件发送未经确认的私有命令，
也没有把 proinfo 值直接冒充当前接口值。未实现的命令与 P2P 操作明确返回不支持。

本机早期 bring-up 还在 Wi-Fi config store 中缓存了旧随机工厂地址。
停用框架后只删除该缓存 XML 元素，其他字节保留，再由标准 HAL 的 `ETHTOOL_GPERMADDR`
路径重新读取并保存真实地址；未重置网络、密码或用户偏好，未添加周期性数据改写脚本。
备份仅保存在本地私有输出目录，不进入仓库。

新 supplicant 与内核 #197 实测：`wpa_cli driver MACADDR`、Android `WifiInfo`、
工厂缓存和实际 netdev 地址一致；未知命令返回 FAIL。熄屏全程关开无线后自动关联/DHCP，
原有保存网络列表与软件 PNO 两个开关保持不变；初次启动及关开各网关5包零丢包、
HTTPS200、netdev 错误/内核 taint 为0。此项不代表已实现选择性过滤或完整联网深睡。
随后正常重启无需手动开启无线即自动连接，Android 当前地址与持久化工厂地址仍一致，
保存网络列表未变；普通 shell ADB、四核 schedutil/604500..1300000、默认 deep/stay-on0 保留。
产品 vendor.img 已同步新 supplicant，内核与 boot.img 仍为已验证的 #197。
