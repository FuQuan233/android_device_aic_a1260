# A1260 bring-up 待办

## 后续执行顺序（2026-10-24，用户确认）

1. **基础接管已验证，保留未决项：PMIC 供电轨**。#42 接入 VUSB/VEMC/VIO18/VIO28，USB PHY 引用真实 VUSB，eMMC 引用真实 VEMC。实机 VEMC=3.3 V，已修正原占位 3.0 V；启动、4 MiB fsync/read 和显示开关通过。屏幕及触摸 I/O 实际接线尚无足够证据：正常内核的屏幕供电回调为空，保留显式 placeholder/dummy；共享 VIO18/VIO28 和 VEMC 暂保持常开，不凭猜测切断。
2. **重绑定竞态已修复并验证：USB 休眠恢复**。#42 日志表明 adbd 重启后新 FunctionFS 初始化反复被冻结：系统挂起成功 6 次、失败 0 次，一条 init 动作耗时约 60 秒。短暂保留 5 秒唤醒保护能恢复枚举。#45 将设备模式 gadget 启停的限时交接接入独立唤醒源；RESET/RESUME 转交正常连接锁，未连接时最多 5 秒自动到期。原生熄屏重启 adbd 已自动恢复 configured；USB 配置 none 时实测 5014 ms 到期、s2idle/RTC 成功 1 次失败 0 次，恢复 adb 后自动枚举。临时锁已释放、插电保持亮屏为 0；标准 boot/kernel 更新为 #45。主机主动 bus suspend/remote wake、PHY 深省电仍待后续整机 PM 工作，USB 大传输继续暂缓。
3. **进行中：完整时钟树**。#46 已把 MAINPLL/UNIVPLL 从固定频率改为 APMIXEDSYS 寄存器读回，并通过 DT 明确 TOPCKGEN 依赖；实机为 1501.5/1248 MHz，Android 启动、Mali 绘制回读、DISP/MFG 熄屏断电及亮屏零欠流通过。#47 补齐 UPLL 分频关系和 PWRAP 的 SPI/26M 门控引用，在首次 WACS2 读取前启用，保留至最终关机；实机 PMIC SPI mux=3、26 MHz，并未使用 USB 的 UPLL_D16（该共享只存在于其他 mux 选择）。启动、电池/温度/PMIC 寄存器读取、一次 s2idle 成功/零失败及 PMIC IRQ 唤醒、ADB 自动重枚举和亮屏零欠流通过。标准 boot/kernel 当前为 #51。#48 已让 28 个 MPLL/UPLL 分频门控读回硬件状态，接管 UPLL_D16/D20 的 SET/CLR；未使用的 D20 实测 bit18 清零，USB 使用的 D16 保持开启。Mali 绘制、启动后熄屏 DISP/MFG 断电、亮屏零欠流通过；USB none/RTC 检查记录两次 s2idle 成功、零失败和 PMIC IRQ 唤醒，ADB 自动恢复。临时锁、RTC alarm 已清理，插电保持亮屏仍为 0。接着 #50 接入 UART0/1 真实门控和固件 mux，stdout UART 保留 critical 时钟引用，未使用的 UPLL_D24 bit25 实测关闭；实际两路 UART 源均为 26 MHz，UART1 仍禁用。一次 alarmtimer 临近截止时间的 EBUSY 退出后，重试 s2idle/PMIC IRQ 唤醒和 ADB 自动恢复成功；唤醒后 4 MiB O_DIRECT 写入/fsync、四轮从关钟状态恢复的直接读回校验再次通过，显示帧计数前进且零欠流。#51 已接管全部 12 路 UPLL 分频门控：为仍开启的固件 MSDC1/SPI/CAM 保留实际父级引用（本机仅 MSDC1=26 MHz）；USB 保留 D2/D4/D8/D16，eMMC 的 D3/D6 按 runtime 引用开关，其余未用分支关闭。Android 启动、Mali 绘制回读、4 MiB O_DIRECT 写入/fsync 和四轮直接读回 SHA256 一致；其中一轮完整采到 MSDC0 及 D3/D6 均关闭后再成功读回，其他分时采样会受后台 I/O 干扰。一次 s2idle/RTC 唤醒成功、零失败，ADB 自动恢复，熄屏 DISP/MFG 断电、亮屏零欠流；临时锁、RTC 和测试数据已清理。其余 15 路 MPLL 分频仍只读，显示及 DDR 消费者需继续核对。DSI 两路门控工作正常但 BSP 未登记源，RDMA0 明确为 AO，不擅自补造父级或硬件门控；PLL 供电及调频仍保留固件设置。
4. **门控修正已验证，引脚/提速待做：eMMC 时钟和引脚**。时钟审查发现 MSDC0 gate 错登记为 TOPCKGEN 0x20 bit17；#49 已按 BSP CG_CTRL1 改为 0x24 bit17。4 MiB RAM 随机数据 O_DIRECT 写入/fsync 后，四轮 O_DIRECT 读回 SHA256 完全一致；每轮开始前均实测 runtime suspended、引用 0、真实 bit17=1，再成功恢复 I/O。临时文件和锁已清理。后续接管固件 pinctrl，再验证提速；当前仍为 legacy、实际约 17.3 MHz，VEMC 保持已验证的 3.3 V。该项在 #49 验证，当前标准 #51 已包含此修正。
5. **待做：整机待机回归**。在新 GPU/DISP/PMIC 驱动下验证挂起、RTC/按键唤醒和 USB 恢复。
6. **待做：深度睡眠与 GPU 变频**。SPM/PCM、DDR 自刷新、集群断电；GPU OPP/devfreq 与供电约束。

以上按顺序推进，每项区分源码接入与实机验证，不用关闭全局 PM 或常驻唤醒锁掩盖故障。

## 时钟、电源域和关机（2026-10-23）

- **#37 亮屏恢复未通过**：用户报告熄屏后无法点亮；ADB 仍能执行命令，Android 收到实体电源键唤醒并报告 ON，10 秒后因锁屏超时回到 OFF，未捕获内核崩溃。实际背光/显示恢复原因待定位，不能将 #37 标为通过。标准产品 boot/kernel 暂保留 #32；故障记录为 `out/a1260-runtime-pm/wake-failure-37{,-later}.txt`。
- 后续现场定位到 PWM 的影子寄存器更新未反映到输出：保持亮屏、设满亮度后，PWM_EN=1/HIGH_WIDTH=1024，但 GPIO66 输入仍为低；延长 COMMIT 脉冲无效。按 BSP 的直接更新模式写 0x20=3 并重写占空比/使能后，GPIO66 变高，用户确认背光恢复、画面正常。驱动现改用直接更新，保留 DISP 断电；尚不能确定缺失的是哪一个 shadow latch 触发条件。
- #39 已编译上机，自动熄屏/亮屏检查显示 DISP/MFG 均可断电、恢复后 RDMA 欠流和异常为 0；用户手动确认背光全灭、亮屏画面正常。标准 boot/kernel 已更新为 #39。排查期间临时 `stay_on_while_plugged_in=2` 已恢复原值 0。熄屏时重启 adbd 曾造成 USB 无法枚举；保持唤醒后重启 adbd 可以重连，USB/系统待机恢复需单独定位，不能把 PWM 修复当作 USB 问题也已解决。

- Android 产品已撤掉 `clk_ignore_unused` / `pd_ignore_unused`；#29 启动完成，两轮熄屏/亮屏后 DSI 300 MHz、RDMA 欠流和异常均为 0。修复 MT6580 DSI 在时钟开启前访问寄存器、依赖 LK 遗留门控状态的问题。
- 新增 SMI 总线时钟与 DISP/ISP genpd 描述；ISP 实机为关闭，控制寄存器 0x3f12、SPM 双状态位均清零。DISP 已在 #35 接入 SMI/LARB0 保存恢复并撤掉 ALWAYS_ON：四轮熄屏亮屏中两个 SPM 寄存器的 DISP 位均在熄屏时清零，亮屏后恢复且 RDMA 欠流/异常均为 0；用户确认画面、触摸正常。GPU 的 MFG 已在 #31 接入 100 ms runtime autosuspend：真实绘制回读后再次断电，两个熄屏/亮屏周期正常，SPM 双状态寄存器的 MFG 位清零。完整 PLL 父子关系和全部外设门控仍待补齐。
- #30 GPU 断电试验导致黑屏、重启及 USB 枚举失败；#31 临时保留共享 MPLL_D5 后恢复。#32 已按固件 mux 接入 SMI、USB、MFG、MSDC0、PWM 父时钟与 D5→D10→D20→D40 引用链，D5 改用硬件 SET/CLR，并移除无条件 CLK_IS_CRITICAL。实机 SMI=MPLL/5，USB=UPLL/16，MFG=MPLL/3，MSDC0=UPLL/6；GPU 断电后 D5 由 SMI 保留一个正常引用。开机、GPU 绘制回读后再次断电、两轮熄屏亮屏通过，RDMA 欠流/异常均为 0，ADB 正常。已更新标准 boot/kernel 产物；记录在 `out/a1260-runtime-pm/clock-parents-validation.txt`。
- #33 已把 I2C0/1/2、APDMA 接回 AXI/MPLL_D12（125.125 MHz），AUXADC 接回 26 MHz；启动、输入设备注册、充电及温度读取通过。仍保留固件 PLL 频率和 mux，不是完整 PLL 驱动；显示引擎父时钟、其他分频门控与 PLL 控制仍待完善。新 GPU/DISP PM 回调下的完整系统待机及长时压力尚未验证。
- #35 熄屏期间 GPU 绘制回读通过，随后 DISP/MFG 同时断电，状态值为 0xbe45/0xbe41。断电状态下 DSI/RDMA 诊断读取会先获取电源引用，不再直接访问掉电寄存器。记录在 `out/a1260-runtime-pm/disp-domain-validation.txt`。
- #35 用户发现背光未灭：MT6580 不能只依赖 enabled+零占空比，已按原厂代码加入 HIGH_WIDTH=1、PWM_EN=0、COMMIT 的关灯方式，实机直接操作确认全灭。#37 增加两个 PWM 周期的提交等待，确保停钟前给输出更新留出时间。停钟后的 PWM 寄存器读值不可靠，不能仅凭它判断输出，最终需要实际熄屏确认；最低亮度仍为 4/255。
- MT6350 RTC 已接入最终关机回调，沿用 BSP 解锁、按用户状态处理 32 kHz 输出及 BBPU=0x4309 时序；PWRAP 改为原子轮询和自旋锁，避免最终断电阶段睡眠。电池供电关机/按键开机已实测：本地记录拔线后 3 秒请求 Android 正常关机，用户观察到关机并可按电源键开机；随后新一轮 #29 启动恢复 ADB、DSI 显示（欠流/异常为 0）、充电检测和温控，维护锁为空。插着充电器的关机行为及实际关机电流尚未验证。
- USB 大传输问题按用户要求暂缓：当前使用 USB/IP，需之后具备直接 USB 连接时再定位。

## CPU、自动灭屏和待机（2026-10-23）

- `stay_on_while_plugged_in=0`，保留 60 秒自动灭屏、锁屏。USB 数据总线工作期间由 MUSB 驱动持唤醒锁，熄屏不应让连接电脑的 ADB 掉线；不能用“充电保持亮屏”设置替代这一行为。没有全局的充电常驻锁。
- CPU0–3 共享 cpufreq-dt/schedutil 策略，现已开放旧 BSP level-0 的 604.5、754、903.5、1001、1105、1209、1300 MHz 七档，频率、电压读回与两轮多核计算通过。前四档 1.15 V，1105/1209 MHz 分别为 1.19375/1.225 V。最高档 BSP 请求 1.30625 V，但实机目标 0x61 的 NI 电压仍钳在 0x60（1.3 V），因此按实测平台值请求 1.3 V。未开放更高速度分档或降压。
- CPU1–3 连续三轮下电、重新上线通过；SPM 双状态寄存器确认离线时对应电源位清零，每轮恢复后多核计算和时钟检查通过。CPU0 保持在线。
- 已接入 WFI 待机及 MT6350 RTC 定时唤醒；修正 PMIC 在子驱动登记唤醒 IRQ 之前就写入空掩码的顺序问题。RTC 按原厂 1968 年起始年份处理，读写时间和闹钟中断已实测。
- 已通过驱动挂起/恢复、三轮 RTC 唤醒以及 Android SystemSuspend 自动挂起；ADB 可自动恢复。触摸驱动提前关 IRQ 后，已消除休眠命令引起的输入唤醒重试，自动挂起一次成功、失败计数为 0。用户确认实际待机后电源键能唤醒，画面和触摸正常，ADB 自动恢复。
- 已从 `rootdir/init.mt6580.rc` 删除 `a1260-bringup` 临时唤醒锁及其说明。`CONFIG_PM_WAKELOCKS=y` 提供接口，本身不持锁；主机更新时使用的短期维护锁必须在工作结束或重启时释放。
- 最终 #21 普通内核（`CONFIG_PM_DEBUG=n`）两次自动挂起分别通过电源键及 RTC 唤醒，成功 2 次、失败 0 次；Android 分别计得睡眠 61.382 秒、挂起开销 0.753 秒。原先触摸 IRQ 顺序导致的重复 `-EBUSY` 已不再出现。
- 当前实现是 s2idle/WFI；SPM/PCM 深度睡眠、集群断电和 DDR 自刷新尚未移植，不能宣称最低待机功耗已经完成。GPU 仍使用已有固定频率配置。

## 图形

- 当前产品已接入原厂 Mali r8p0：GPL Utgard 驱动和原厂 EGL/GLES 已完成真实绘制、DMA-BUF/EGLImage 导入及 native fence 回读；修复真实 devinfo 输入后，SurfaceFlinger、SystemUI、Launcher 和设置可显示，SurfaceFlinger 报告 ARM/Mali-400 MP。
- 原厂 `libmtk_drvb` 不能缺少同一设备的真实 devinfo 数据。缺失时返回 `0x1004`，原厂错误处理会向上层栈写入 `0xddeeaadd`；已用实机备份数据适配其输入文件，保留原有硬件检查。
- 已修复 native fence 已完成时提前退出、漏建隐式依赖 waiter 导致的 `mali_timeline_dma_fence_callback` 空指针；abort 也统一通过 pending 计数和 worker 完成。
- 内置驱动 #6 冷启动桌面阶段曾出现 vblank/MMC 超时及 CPU0/2 的 RCU 停顿，精确根因尚未捕获。后续审查修正了 timeline IRQ 自旋锁内创建 sync_file 的可睡眠分配，以及原生 fence 信号回调期间缺少临时引用、最终释放重入同一把锁的问题。不能据此断言此前停顿已彻底解决，仍需长期和内存压力验证。
- #7 临时锁诊断内核已运行超过十二分钟，设置和桌面可用，未捕获新的锁等待。`kernel/locking/spinlock_debug.c` 的临时修改已全部移除；#8 普通内核实机确认 `CONFIG_DEBUG_SPINLOCK=n`、`CONFIG_MALI400=y`，四核在线并进入 boot_completed，绘制回读无 GL 错误，设置/桌面切换通过。
- #8 普通版由用户确认实屏画面正常、触摸准确。连续三次结束设置进程再冷启动均通过，当前运行期间未见新的 GPU MMU 错误或内核 Oops/RCU stall；长期稳定性仍待验证。

- 16 MiB CMA 曾耗尽并导致 Launcher/SystemUI 缓冲分配失败；64 MiB 的新池已在实机启用。
- 之前 Lima 路径已验证 Mali-400 MP2、三角形回读和桌面；该路径作为历史实现保留，当前产品选择原厂 Utgard，未同时加载两个 GPU 驱动。
- 之前单核配置冷启动出现 SystemUI 启动超时 ANR；四核物理定时器版已到桌面，当前启动事件未见 SystemUI ANR，后续观察冷启动性能。

## 多核启动（已实机验证）

- 已移植 MT6580 非安全启动向量、SPM CPU1–3 上电时序以及 GPT6/ARM 每核定时器，移除两处 `maxcpus=1`。
- 首次实机 #17 在约 27 ms 启动四核；随后虚拟计数器计时异常，0.211 秒时 RCU 看到十几亿个 jiffies，未进入 Android。
- #18 改用物理定时器/计数器后进入 Android，CPU0–3 全部在线，每核本地定时器中断正常，ADB 和桌面可用，未再见 RCU 时间跳跃或内核 Oops/panic。
- 四线程分别固定到四核：计算结果一致，400000 次共享原子计数无丢失；CPU 时间合计 2.457 秒、墙钟 0.618 秒，并行度 3.98。跨核迁移 400 次未出现单调时钟倒退。
- 截图正常，FT6336U 保持注册，RDMA 欠流/异常为 0。CPU 热拔插、低频档变频及待机已在后续实现，见本文开头；四核首次启动验证不能替代这些后续检查。

## 温控（2026-10-23）

- MT6580 AP AUXADC thermal 已接入主线 thermal 框架。三路 MCU 传感器取最高温；使用只读 eFuse 0x194..0x19f 的真实校准，实机数据与原机备份一致，不在 DTS 写死校准字。
- 修正第三路传感器 mux（2，而非 MT2701 的 ABB=16）、TS_CON0[7:6] 缓冲上电与 AUXADC channel 11 自动采样配置，保留旧 BSP 的增益、偏置及斜率整数换算。
- 75°C 开始被动降频，5°C 回差；105°C 请求临界关机。阈值是当前板级保守策略，尚未做高温极限或临界关机实测。已临时降低被动阈值验证实际降频及恢复，测试后恢复 75°C。
- 已实现温控 AHB 采样暂停、等待空闲和休眠关闭模拟缓冲，唤醒后重新配置采样。#26 拔线后 USB 锁正常释放、RTC 唤醒、温控采样及 ADB 重连恢复；但触摸空帧缺陷造成 5 次 event2 待机重试，加上拔线瞬间一次充电事件重试，实际仅睡眠 0.133 秒，不能算干净的长时待机通过。后续版本针对 FT6336U 的 TD_STATUS=0 空帧释放全部触点，并过滤越界触点 ID，随后 #27 在拔线事件稳定 2 秒后释放维护锁：自动待机成功 1 次、失败 0 次、无重试退避，实际睡眠 18.820 秒，RTC 唤醒后三路温度采样与 ADB 重连正常；临时维护锁已释放。

## USB

- 已补回 MT6580 USB 活动连接的唤醒锁。总线 RESET/RESUME 持锁，SUSPEND/DISCONNECT 和 gadget 停止、解绑释放；不强制亮屏，也不对所有充电器常驻持锁。当前 PHY 仍强制 device VBUS/session；主机主动 USB suspend 的完整链路唤醒保留另项验证，不能宣称已经修完所有 USB PM 路径。
- #24 已完成熄屏保持 ADB 检查：Android 为 Asleep，显示阻塞锁释放、背光 PWM 占空比 0，MUSB 唤醒源保持活动，system suspend 成功计数不增加，ADB 仍能执行命令。

- 小型 ADB 命令和文件传输可用，大文件 push 仍会使通道停止响应；拔插 USB 可恢复且屏幕动画持续，需继续定位 bulk/PIO 路径。

## 其他硬件

- MT6350 PMIC 中断、电源键和 AUXADC 电池检测已接入；BQ24296 由 I²C0/0x6b 上的主线驱动读取真实充电状态。Android 已显示电池存在、约 4.12 V / 27.3°C，并能识别 USB 拔出放电、插入充电及充满。
- 电源键已抓到成对 KEY_POWER DOWN/UP。GPIO66/DISP_PWM 已接到 DRM panel 和 drm_hwcomposer，用户确认背光可完全熄灭、恢复和调节；熄屏时实际 PWM 占空比为 0，亮屏恢复。原先最低档把 Composer 的 0.0（最低亮度）直接写成关灯，已改为设备专用 4..255 有效亮度映射；用户确认 4/255 可见，重启后实测最低档 PWM 为 616/39347 ns、熄屏为 0，框架和滑条范围不变。
- 拔插实测已覆盖 Discharging → Charging → Full，修正 BC1.2 覆盖后，USB 重插仍维持 500000 µA 输入限流。
- 已定位旧电量跳变：拔电 3715 mV/13%，约 10 秒后插电 3817 mV/50%，原因是端电压直接查 OCV 表。#41 改用 BSP OAM 第二观察器思路：启动 ADC9 电压初始化、温度/电压内阻表、OCV 与端电压差估算电流并按时间积分；插拔保持 SOC，温度连续插值，BATSNS 五次平均。估算电流不冒充 CURRENT_NOW 或硬件库仑计。实机已启动并进入 Android，启动电压 3.717 V 对应约 14%，充电端电压到 3.81 V 时仍按积分变化；实测约 38 秒拔线：3.853 V/16% → 最低 3.720 V/16%，插回仍为 16%，随后通过积分升为 17%；状态恢复 Charging、输入限流 500 mA，无采样错误。记录为 `out/a1260-runtime-pm/battery-gauge-41-summary.txt`；标准 boot/kernel 已更新 #41。
- 电量绝对精度仍需实装电芯的容量、内阻、OCV 和完整充放电校准；目前仍用 1397 mAh 参考参数。长时间睡眠没有真实电流观测，恢复时不以醒着的负载外推睡眠耗电；跨重启 SOC 保留和老化模型尚未移植。满电需 Full 且电压不低于 4.1 V 持续 60 秒才锚定，拔电低于 3.4 V 持续 30 秒锚定空电。排查期间临时插电保持亮屏已恢复 0，a1260-battery-check 临时锁已释放。
- 背光亮度曲线尚未按屏体实测标定；用户反馈低段变化小、高段偏亮。当前保留 4..255 映射，最高亮度不因扩大数值范围而增加。
- 后续已验证 s2idle 并删除临时唤醒锁；电源键能控制画面不代表 SPM/DDR 深度睡眠已完成。

- 已修正 RT group 的默认策略，新的蓝牙崩溃原因是缺少 HCI HAL；蓝牙硬件及 HAL 尚未移植。
- AW9523（I²C1/0x5b）和 AW9106（I²C2/0x5b）已通过 gpio-keys 接入 18 个实体键，并保留原厂 Android 键码布局；用户确认响应及位置正确，捕获到两个芯片的成对按下/松开。I²C2 时钟门控修正为 bit16，重启可自动注册。原厂红外服务/功能键应用动作及 AW9106 LED 功能尚未移植。
- FT6336U 点击、滑动及落点已由用户确认；用户最新说明双指一直未试，双指操作保留未验证状态。#27 零触点帧不再误报左上角按下，已抓到正常触摸按下/抬起及 tracking-ID 结束。
- 物理音频、Wi-Fi 尚待移植。

## 显示观察

- 触摸接入后的启动过程中，用户报告过短暂错位，随后未修改参数就恢复。截图正常、RDMA 欠流和异常计数均为 0；原因未定位，若再次出现需结合当时的画面和寄存器定位。
