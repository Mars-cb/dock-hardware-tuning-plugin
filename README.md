# dock-hardware-tuning-plugin

适用于 UOS 20 的 DDE Dock 5.9 插件，使用 Qt 5.11 和 Dock 插件接口 2.0。点击任务栏上的芯片图标打开面板。左侧为硬件调控，右侧为显示器。下拉框显示当前值，选择另一项后立即应用。悬停在控件或下拉选项上可查看说明。亮度滑块拖动时随数值变化提交。

| 项目 | 面板可调节的设置 | 监控 |
| --- | --- | --- |
| CPU | 调频策略、全局睿频（驱动提供时） | 总占用、封装温度、各调频策略中的最高读数 |
| AMD GPU | 性能档位、自动风扇或固定 PWM 70% / 85% / 100% | 核心和显存控制器负载、频率、显存容量及使用量、温度、功耗、风扇 RPM |
| 内存 | | 已用、总量、使用比例 |
| PCIe | ASPM 策略 | 当前策略 |
| 磁盘 | SATA 和 NVMe 的 I/O 调度器 | 本机系统及数据卷容量、每盘吞吐量、驱动提供时的温度 |
| 外接显示器 | 每屏硬件输入源、亮度、对比度 | 当前 DDC/CI 读数 |

监控仅在面板显示时每十秒采样一次。CPU 和 GPU 温度在临界值以下 10 °C 内为黄色，达到临界值为红色。内存、显存在 85% / 95% 着色；磁盘容量在 90% / 95% 着色。未暴露的控制项目会自动隐藏。CPU 占用和磁盘速度在第二次采样后显示。显存容量和显存控制器负载是不同指标。GPU 调控目前只支持 amdgpu，GPU 风扇的百分比表示 PWM 占空比。风扇固定模式应在观察温度时使用，测完可切回自动。主板风扇、硬盘休眠和 GPU 电压不在调控范围内。

本版只修改运行时设置。重启或 DDE 重设电源模式可能覆盖设置。面板中的「刷新」会重新读取状态。在出现过高性能档死机的机器上，可以保留 PCIe ASPM 的默认值，单独改变 CPU 调频。

## 显示器

安装 `dde-dock-input-source` 0.1.5 或更新的兼容版本和 `ddcutil` 后，「显示器」页会用参考项目的独立 helper 扫描显示器和切换输入源。亮度和对比度使用 DDC/CI VCP 0x10 / 0x12，需要显示器开启 DDC/CI 且当前用户有对应 I²C 设备权限。参考项目的 udev 规则可以授予活动桌面用户 AMD DP AUX 的访问权限。

面板只列出显示器报告的输入源。切到另一台设备后，可用显示器物理按键切回。亮度用 DTK 滑块选择，对比度按约 10% 步长选择。首次打开面板时检测显示器，之后沿用缓存，收到 DRM 插拔事件时重新检测。也可点击「刷新」。亮度和对比度写入后尝试读回确认。切换后插件会读回输入源。读不到时显示「输入源未确认」，并在面板打开期间定期重读。

参考实现：[Mars-cb/dde-dock-input-source](https://github.com/Mars-cb/dde-dock-input-source/tree/10f5f4735415f675976e6e435e8b6b30bd503ae5)。本项目复用已安装的 helper 协议，未拷贝该仓库源码。Debian 打包结构沿用其原生源码包和 debhelper/CMake 方式。

## 构建及打包

构建依赖为 CMake、C++14 编译器、qtbase5-dev 5.11+、libqt5svg5-dev、libdtkwidget-dev 5.6+、pkg-config、libudev-dev、debhelper 和 fakeroot。本机优先使用系统的 dde-dock-dev 头文件。缺少该包时，使用项目内附的 UOS 20 Dock 5.9.5 ABI 头文件。

```bash
./ci/build-deb.sh
sudo apt install ./dist/dock-hardware-tuning-plugin_0.2.9_amd64.deb
```

安装后注销并重新登录 DDE，再从任务栏打开「硬件调控」。若图标没有显示，检查控制中心的任务栏插件列表。卸载使用 `sudo apt remove dock-hardware-tuning-plugin`。

插件由普通用户运行。传感器读取和显示器调节按系统提供的设备权限工作。内核参数写入由系统 D-Bus 服务执行。安装使用管理员权限。日常调控仅对 sudo 组中正使用本机活动图形会话的用户开放。服务只接受预定义项目和驱动支持的值。GPU PWM 允许驱动在 3 个百分点内取整，面板显示读回值。

GitHub Actions 在每次 push 后使用固定 Debian 10 容器和校验过的 DTK 5.6 源码构建 amd64 版本，并在该次运行的 Artifacts 中保存 deb 和 SHA-256。合并后的批量推送在推送末端的提交上运行一次。工作流也可手动触发。

## 本机预览

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 3
DDE_HARDWARE_TUNING_PREVIEW_HELPER="$PWD/controller.py" \
  build/dock-hardware-tuning-plugin-preview "$PWD/build/libdock-hardware-tuning-plugin.so"
```

预览使用真实读数，控件只读。附加 PNG 路径可在离屏模式保存预览图。

GPU 节点定义见 [Linux AMDGPU 文档](https://kernel.org/doc/html/latest/gpu/amdgpu/thermal.html)，CPU 调频规则见 [Linux 5.10 CPUFreq 文档](https://kernel.org/doc/html/v5.10/admin-guide/pm/cpufreq.html)。
