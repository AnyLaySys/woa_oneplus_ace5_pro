# Windows for OnePlus Ace 5 Pro

`woa_oneplus_ace5_pro` 是 OnePlus Ace 5 Pro / PKR110（设备代号 `hummer`，Qualcomm SM8750）的 **Windows on ARM 设备适配工程**，包含设备驱动、运行固件、配置，以及统一构建和部署入口。

**目前所有设备工作正常**，包括触控、音频、Wi-Fi、蓝牙、电池电量、充电、USB Host/OTG、GPU 桌面显示和 CPU 调频。

Windows 驱动配合本设备的 UEFI/ACPI 使用。Linux 适配由 `linux_oneplus_ace5_pro` 维护，UEFI 由 `uefi_oneplus_ace5_pro` 维护。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/syna-s3910` | Synaptics S3910 触控驱动及上游许可证 |
| `src/qcom-audio` | Qualcomm ADSP 音频、SIA9177 功放及 PMIC 充电、USB 电源管理 |
| `src/qcom-wcn7861` | Qualcomm WCN7861 Wi-Fi 驱动 |
| `src/qcom-bluetooth` | Qualcomm WCN7861 UART HCI 蓝牙驱动 |
| `src/qcom-battery` | Qualcomm 电量计（bq27xxx，hub I2C）剩余电量显示驱动 |
| `src/qcom-charge` | 随音频包部署的充电与 USB 角色管理服务 |
| `src/qcom-cpupower` | SM8750 原生 CPU 调频驱动 |
| `src/qcom-keys` | Qualcomm 物理按键及 PMIH010x 振动控制 |
| `src/qcom-gpu` | Adreno 830 Windows 硬件执行驱动 |
| `src/mesa/umd` | D3D DDI → Vulkan / Turnip 用户态后端 |
| `fw/gpu` | Adreno 830 GMU、SQE、AQE 固件及来源、许可 |
| `fw/adsp` | 随音频驱动包安装、由 UEFI 预加载的 ADSP 主固件与 DTB 数据集 |
| `fw/sia9177` | 音频运行数据 replay.bin |
| `fw/wcd9395` | 音频编解码器运行数据 replay.bin |
| `fw/wcn7861` | 网络初始化所需的七个固件/配置文件 |
| `fw/wcn7861-bt` | 蓝牙外围控制器、TMEL、主蓝牙的五个原始固件/NVM 文件 |
| `AnyLaySys.cer` | 项目共用的公开测试签名证书 |
| `build.cmd` | 驱动构建、签名及固件复制的统一入口 |
| `out/drv` | 最终签名的驱动包及其运行载荷 |
| `out/<目标>` | 编译中间文件和 WDK 打包暂存 |
| `out/mesa` | Mesa、vkd3d 源码及 GPU 用户态编译缓存 |

`fw` 保存设备运行所需的固件和配置；`out` 保存可重新生成的构建产物，已由 `.gitignore` 排除。

## 构建环境

构建使用以下环境：

- Visual Studio 18 Insiders：`C:\Program Files\Microsoft Visual Studio\18\Insiders`。
- WDK：`10.0.26100.0`，包含对应的内核驱动工具集、ARM64 编译环境、SignTool 和 Inf2Cat。
- GPU 用户态交叉编译：WSL `Ubuntu-26.10`；`WOA_WSL_DISTRO` 可指定已配置工具链的其他发行版。
- 签名证书：默认使用 `AnyLaySys` 代码签名证书，指纹为 `FF7908A360512CF12E08C0BD99625830C1EA4E6D`；`WOA_SIGN_CERT` 可覆盖默认指纹。证书及对应私钥位于当前用户的 My 证书存储中。

在已配置上述环境的管理员终端中运行构建。目标 Windows 启用测试签名并信任当前的 `AnyLaySys.cer`。

构建音频或充电服务前，在本地准备 `src/qcom-charge/credential.bin`，内容为 16 字节 UFCS 认证材料。

## 编译驱动

在仓库根目录运行：

```cmd
build.cmd all
build.cmd syna-s3910
build.cmd qcom-audio
build.cmd qcom-bluetooth
build.cmd qcom-wcn7861
build.cmd qcom-battery
build.cmd qcom-charge
build.cmd qcom-cpupower
build.cmd qcom-keys
build.cmd qcom-gpu
build.cmd --help
```

PowerShell 中对应使用 `.\build.cmd`，例如：

```powershell
.\build.cmd qcom-audio
```

`build.cmd all` 依次编译所有驱动，音频构建同时编译充电服务；任一目标失败时停止。也可指定单个组件构建，统一使用 **Release / ARM64** 配置。

构建产物如下：

| 目标 | 输出目录 | 文件 |
| --- | --- | --- |
| `syna-s3910` | `out/drv/syna-s3910` | syna_s3910.inf / syna_s3910.sys / syna_s3910.cat |
| `qcom-audio` | `out/drv/qcom-audio` | sia9177.inf / sia9177.sys / sia9177.cat / charge.exe / credential.bin / ADSP（58 文件） |
| `qcom-bluetooth` | `out/drv/qcom-bluetooth` | qcom_bluetooth.inf / qcom_bluetooth.sys / qcom_bluetooth.cat |
| `qcom-wcn7861` | `out/drv/qcom-wcn7861` | wcn7861.inf / wcn7861.sys / wcn7861.cat |
| `qcom-battery` | `out/drv/qcom-battery` | qcom_battery.inf / qcom_battery.sys / qcom_battery.cat |
| `qcom-charge` | `out/qcom-charge/ARM64/Release/bin` | charge.exe / credential.bin |
| `qcom-cpupower` | `out/drv/qcom-cpupower` | qcom_cpupower.inf / qcom_cpupower.sys / qcom_cpupower.cat |
| `qcom-keys` | `out/drv/qcom-keys` | qcom_keys.inf / qcom_keys.sys / qcom_keys.cat |
| `qcom-gpu` | `out/drv/qcom-gpu` | a830_native.inf / a830_native.sys / a830_native.cat / a830_umd.dll / vulkan_freedreno.dll / msvcp140.dll / vcruntime140.dll / gen80000_zap.mbn |

编译中间文件位于 `out/<目标>/ARM64/Release`。驱动包中的 SYS 和 CAT、GPU 用户态驱动 DLL 及充电服务 EXE 均由构建脚本签名，安装时使用 `out/drv` 下的完整包。

`qcom-audio` 会自动构建 `qcom-charge`，将 `charge.exe`、`credential.bin` 和 58 个 ADSP 文件纳入音频包及 CAT。其余运行数据由 `install-fw` 复制。

`qcom-gpu` 会自动构建直接 DDI 后端、Turnip 和 WDDM 内核。Mesa、vkd3d 源码及编译缓存位于 `out/mesa`，Meson 子依赖位于 `out/mesa/source/subprojects`。

同一次构建还会生成 `out/drv/qcom-d3d12`。把整个目录复制到设备后，在管理员 PowerShell 中运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-d3d12.ps1
```

安装脚本备份系统 `d3d12.dll`，安装 ARM64/x64 共用入口和各自的 VKD3D-Proton、DXGI 运行库。重启应用后可直接使用 VKD3D-Proton → Turnip，无须在应用 EXE 目录复制 DLL；不使用 Zink。VKD3D-Proton 和 DXGI 直接加载显示驱动注册的对应架构 Turnip ICD，避免 Vulkan 全局枚举进入 Dozen 后递归调用 D3D12。原生 D3D11 保持现有 DDI 路径，显式软件适配器仍使用系统 WARP。实机已通过 FL12_0/FL12_1 设备创建、GPU 计算、三角形显示、稀疏缓冲及纹理回读，以及像素／四重采样 ROV 排序测试；TiledResourcesTier 为 2，ROVsSupported 为 1。这些测试验证图形接口，不代表所有游戏均已通过实测。需要恢复系统入口时运行同一脚本并加 `-Uninstall`，随后重启应用。

### 名称与版本

驱动提供商和文件公司为 `AnyLaySys`；硬件制造商按对应设备填写 Synaptics、Qualcomm 等。

版本采用 `yy.M.d.revision`：

- 触控、音频、蓝牙和电量计默认按构建日期生成；`S3910VersionDate` / `S3910Revision`、`SiaVersionDate` / `SiaRevision`、`QcbVersionDate` / `QcbRevision`、`QbtVersionDate` / `QbtRevision` 可覆盖相应日期和修订号。
- 网络版本为 `26.11.7.62`，版本头、INF 和工程中的 StampInf 日期、版本保持同步。

## 复制固件和运行数据

固件按组件存放，部署位置如下：

| 输入 | 目标位置 | 当前数量 |
| --- | --- | --- |
| `fw/adsp` | 音频包内 `ADSP`；安装后为 `%SystemRoot%/System32/DriverStore/FileRepository/sia9177.inf_arm64_<系统生成标识>/ADSP` | 58 文件 |
| `fw/sia9177/replay.bin` | 目标 Windows 目录下 `System32/drivers/sia9177/replay.bin` | 1 文件 |
| `fw/wcd9395/replay.bin` | 目标 Windows 目录下 `System32/drivers/wcd9395/replay.bin` | 1 文件 |
| `fw/wcn7861` | 目标 Windows 目录下 `System32/drivers/wcn7861` | 7 文件 |
| `fw/wcn7861-bt` | 目标 Windows 目录下 `System32/drivers/wcn7861-bt` | 5 固件/NVM 文件，另配 address.bin |
| `fw/gpu` | 目标 Windows 目录下 `System32/drivers/qcom-gpu` | 3 固件 |
| `fw/gpu/gen80000_zap.mbn` | 随 GPU 驱动包安装到 `DriverStore/FileRepository/a830_native.inf_arm64_<系统生成标识>`，由配套 UEFI 认证加载 | 1 固件 |

ADSP 随音频驱动包安装到 DriverStore。UEFI 在 Windows 启动前选择 `DriverVer` 最高且包含完整 MDT/DTB 的音频包，加载同一 ADSP 目录中的固件分段。目标 Windows 卷需在启动前可供 UEFI 读取。音频 replay 数据由 Windows 驱动读取。

其余固件和运行数据通过 `install-fw` 复制。首次配置时填写目标设备的原厂蓝牙地址；例中的 `W:` 替换为实际 ARM64 Windows 目标：

```powershell
.\build.cmd install-fw -WindowsDirectory "W:\Windows" -BluetoothAddress "XX:XX:XX:XX:XX:XX"
```

添加 `-WhatIf` 可预览复制计划；覆盖已有目标文件时添加 `-Force`。目标 Windows 目录须已存在，脚本会检查 ARM64 架构。

蓝牙地址转换为六字节小端 `address.bin`，保存在 `System32/drivers/wcn7861-bt`。未传入 `-BluetoothAddress` 时，使用目标上已有的有效地址文件。

## 在完整 Windows 中使用

1. 准备配套的 UEFI/ACPI，并配置目标 Windows 的测试签名和证书信任。
2. 将完整音频包通过 DISM 安装到目标 ARM64 Windows 镜像，使 ADSP 随包进入 DriverStore。CPU 调频驱动的离线安装步骤见 [CPU 说明](src/qcom-cpupower/README.md)。
3. 在首次启动驱动前，用 `install-fw` 部署上述运行数据和本机蓝牙地址。
4. 在目标手机的 ARM64 Windows 管理员终端中安装完整驱动包，例如：

```powershell
pnputil /add-driver ".\out\drv\syna-s3910\syna_s3910.inf" /install
pnputil /add-driver ".\out\drv\qcom-audio\sia9177.inf" /install
pnputil /add-driver ".\out\drv\qcom-bluetooth\qcom_bluetooth.inf" /install
pnputil /add-driver ".\out\drv\qcom-wcn7861\wcn7861.inf" /install
pnputil /add-driver ".\out\drv\qcom-battery\qcom_battery.inf" /install
pnputil /add-driver ".\out\drv\qcom-gpu\a830_native.inf" /install
```

5. 重启设备。

## GPU 与 CPU

GPU 原生桌面路径为 DWM / D3D DDI → Vulkan 后端 → Turnip → WDDM → Qualcomm Adreno 830。DWM 硬件绘制、翻转和窗口拖动正常。设备名称为 `Qualcomm Adreno 830`，构建入口为 `build.cmd qcom-gpu`。下载、版本与工具链配置、编译和签名逻辑统一保存在 [build.cmd](build.cmd)，运行时生成的脚本和配置位于 `out/mesa`，最终驱动包位于 `out/drv/qcom-gpu`。`src/mesa` 仅保存驱动源码和 Mesa 源码补丁。

GPU 调频按实际忙碌比例选择原厂档位，空闲立即降到 160 MHz；活动时每 20 ms 采样，空闲时每 100 ms 检查，普通降频需要连续两次低负载采样。温控读取 GPU TSENS 有效通道的最高温度，达到 87°C 时将上限限制到 660 MHz，降至 78°C 后恢复正常调频；达到 99°C 时限制到最低的 160 MHz，降至 81°C 后退回限频状态，再降至 78°C 后恢复正常调频；温控上限也约束固定频率请求，温度不可读时保持最低频率。调频与提交队列分开同步，持续 GPU 工作不会阻塞温控。可用 `src/qcom-gpu/tools/state.ps1` 查看当前频率、温控上限、负载及温度。显示提交注册 MPO1/2/3 的主平面回调，能力只报告一个 RGB 主平面，不启用额外叠加平面。持续游戏时的温度表现需在设备上验证。

GPU 固定显存池为 64 MB。共享显存容量按 Windows 可见内存预算计算，并受剩余 GPU 地址空间约束，当前最大为 4 GB；物理页和页记录按需映射、分配与释放。

SM8750 CPU 两组核心均开放原厂 16 档：六核组 384–3532.8 MHz，两核组 1017.6–4320 MHz。驱动按实际负载升降频，空闲立即降至最低档并支持 PSCI 保留上下文休眠；构建入口为 `build.cmd qcom-cpupower`。详见 [CPU 说明](src/qcom-cpupower/README.md)。

## 蓝牙与 USB

蓝牙使用独立的 GENI UART（`0x898000`、GSI 493），由 UEFI 的 `ACPI\HMRB7861` 节点提供寄存器和中断。Windows 原生蓝牙、配对、键鼠、A2DP 音乐、LE 扫描、GATT 广播和开关恢复正常。

USB 2.0 Host/OTG 支持自动插拔与充电器输入切换，主控和集线器使用 Windows 原生 USBXHCI/USBHUB3，UEFI 提供 `ACPI\PNP0D10` 节点。音频包中的 ADSP/PMIC 通道与 `ALSCharge` 服务共同管理 OTG 供电和角色切换，USB 外设正常工作。

## 电量与充电

`qcom-battery` 读取 BQ 电量计，并向 Windows 提供电池电量。`qcom-audio` 提供 PMIC GLINK、普通充电和 UFCS 充电通道，`qcom-charge` 的 `ALSCharge` 服务随音频包安装并自动启动，管理充电策略和 USB 角色。

UFCS 认证文件 `credential.bin` 随音频包进入 DriverStore，与 `charge.exe` 位于同一目录，由服务读取。

## 触屏反馈

触控及按键反馈由 `qcom-keys` 控制 PMIH010x 马达，每次播放 18 ms，使用高精度定时器，实际电压为 800 mV 和 1200 mV，振幅均为 `0xFF`。`syna-s3910` 按接触 ID 分别记录每根手指：新增手指按下请求一次 800 mV 短振，在原有移动阈值内按住超过 495 ms 后请求一次 1200 mV 短振，抬起不振；移动超限或读取失败取消对应长按反馈。音量键单独按下上报一次音量按下／释放并请求一次 800 mV；按住 270 ms 后由按键驱动按 27 ms 节奏重复上报音量按下／释放，同时请求 800 mV 振动，系统音量连调与振动请求共用计时，播放和停机期间及停止后 27 ms 内的新触发忽略、不补振。电源键单独短按在抬起时请求一次 800 mV，单独按住 2.4 s 时请求一次 800 mV，之后抬起不再振。音量加／电源、音量减／电源组合成立时各请求一次 800 mV，并取消该次电源抬起反馈。`qcom-audio` 的扬声器音量和静音节点实际执行 24 位 PCM 增益处理，并通过已有内核控制设备提供当前音量上下限；音量键按住时到达相应极限播放两次 1200 mV，每次 18 ms，两次之间仍等待停止后 27 ms，随后停止重复反馈。极限模式在马达空闲后开始，开始前松开按键或离开极限会取消。触控、按键及音量协作不需要常驻用户态程序。版本沿用 `yy.M.d.revision`，日期和修订号由各驱动现有项目属性控制。

## 许可

触控目录保留上游 GPLv3 许可证；网络部分文件采用 BSD-3-Clause-Clear。各组件的许可证和固件来源记录随对应目录保留。
