# A1260 bring-up 待办

## 修好唤醒后删除临时唤醒锁（用户明确要求）

- 位置：`rootdir/init.mt6580.rc` 的 `on init` 动作。
- 要删除的命令：`write /sys/power/wake_lock a1260-bringup`，以及对应临时说明。
- 原因：实机在启动后约 378 秒进入 `s2idle`，冻结用户进程并挂起控制台，随后 USB/ADB 无法恢复。MT6580 的睡眠与唤醒路径尚未移植完成。
- 恢复条件：移植并验证唤醒源、中断、电源域与 USB/显示恢复；不持有此锁时，实机多次睡眠和唤醒后仍能显示画面、响应 ADB。
- 达到条件后删除上述命令，重新构建 vendor 并上机复测。`kernel/a1260.config` 中的 `CONFIG_PM_WAKELOCKS=y` 同时复核是否仍需保留；它提供接口，本身不持锁。
- 当前状态：临时措施已写入源码并随 boot/vendor 编译完成，等待部署和实机验证；不能将其当作睡眠功能修复。

## 图形

- 16 MiB CMA 已耗尽，导致 Launcher/SystemUI 缓冲分配失败；64 MiB 的修改已编译，尚待实机验证。
- 接入 Mali-400 的 Lima 内核驱动与 Mesa EGL/GLES，并验证实际 GPU 任务及 Android 渲染器；编译成功不代表已启用硬件加速。

## USB

- 小型 ADB 命令和文件传输可用，大文件 push 仍会使通道停止响应；拔插 USB 可恢复且屏幕动画持续，需继续定位 bulk/PIO 路径。
