# A1260 bring-up 待办

## 修好唤醒后删除临时唤醒锁（用户明确要求）

- 位置：`rootdir/init.mt6580.rc` 的 `on init` 动作。
- 要删除的命令：`write /sys/power/wake_lock a1260-bringup`，以及对应临时说明。
- 原因：实机在启动后约 378 秒进入 `s2idle`，冻结用户进程并挂起控制台，随后 USB/ADB 无法恢复。MT6580 的睡眠与唤醒路径尚未移植完成。
- 恢复条件：移植并验证唤醒源、中断、电源域与 USB/显示恢复；不持有此锁时，实机多次睡眠和唤醒后仍能显示画面、响应 ADB。
- 达到条件后删除上述命令，重新构建 vendor 并上机复测。`kernel/a1260.config` 中的 `CONFIG_PM_WAKELOCKS=y` 同时复核是否仍需保留；它提供接口，本身不持锁。
- 当前状态：已编译并部署；实机 `/sys/power/wake_lock` 显示 `a1260-bringup`，运行超过 660 秒仍可响应 ADB。不能将其当作睡眠功能修复。

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
- 截图正常，FT6336U 保持注册，RDMA 欠流/异常为 0。CPU 热拔插、DVFS 与休眠尚未实现；临时唤醒锁继续保留。

## USB

- 小型 ADB 命令和文件传输可用，大文件 push 仍会使通道停止响应；拔插 USB 可恢复且屏幕动画持续，需继续定位 bulk/PIO 路径。

## 其他硬件

- 蓝牙进程因无法启用实时调度而 abort，需核对 RT group 调度配置；尚未验证蓝牙硬件。
- FT6336U 触摸已实机验证：点击、滑动、双指正常，落点准确。
- 物理音频、Wi-Fi 尚待移植。

## 显示观察

- 触摸接入后的启动过程中，用户报告过短暂错位，随后未修改参数就恢复。截图正常、RDMA 欠流和异常计数均为 0；原因未定位，若再次出现需结合当时的画面和寄存器定位。
