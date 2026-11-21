# Windows for OnePlus Ace 5 Pro

`woa_oneplus_ace5_pro` 是 OnePlus Ace 5 Pro / PKR110（设备代号 `hummer`，Qualcomm SM8750）的 **Windows on ARM 设备适配工程**，包含设备驱动、运行固件、配置，以及统一构建和部署入口。

**目前所有设备工作正常**，包括触控、音频、Wi-Fi、蓝牙、电池电量、充电、USB Host/OTG、GPU 桌面显示和 CPU 调频。

Windows 驱动配合本设备的 UEFI/ACPI 使用。Linux 适配由 `linux_oneplus_ace5_pro` 维护，UEFI 由 `uefi_oneplus_ace5_pro` 维护。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/syna-touch` | Synaptics S3910 触控驱动及上游许可证 |
| `src/qcom-audio` | Qualcomm ADSP 音频、SIA9177 功放及 PMIC 充电、USB 电源管理 |
| `src/qcom-wlan` | Qualcomm WCN7861 Wi-Fi 驱动 |
| `src/qcom-bt` | Qualcomm WCN7861 UART HCI 蓝牙驱动 |
| `src/ti-batt` | Texas Instruments BQ28Z610 电量计（hub I2C）剩余电量显示驱动 |
| `src/qcom-charge` | 随音频包部署的充电与 USB 角色管理服务 |
| `src/qcom-cpupwr` | SM8750 原生 CPU 调频驱动 |
| `src/qcom-keys` | Qualcomm 物理按键及 PMIH010x 振动控制 |
| `src/qcom-sensors/hall.inc` | AKM AK09970 霍尔磁场采样和三段拨键判定，复用按键及传感器包 |
| `src/nxp-nfc` | NXP SN220T NFC 读卡驱动，硬件通信待验证 |
| `src/qcom-ir` | 高通 SPI 红外发射及 Samsung M5 电源按钮，配套 boot 后实测 |
| `src/qcom-cam` | Qualcomm SM8750 / Sony IMX906 主摄控制器及 AVStream 视频接口，实机采集待验证 |
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
build.cmd syna-touch
build.cmd qcom-audio
build.cmd qcom-bt
build.cmd qcom-wlan
build.cmd ti-batt
build.cmd qcom-charge
build.cmd qcom-cpupwr
build.cmd qcom-keys
build.cmd nxp-nfc
build.cmd qcom-cam
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
| `syna-touch` | `out/drv/syna-touch` | syna_touch.inf / syna_touch.sys / syna_touch.cat |
| `qcom-audio` | `out/drv/qcom-audio` | sia9177.inf / sia9177.sys / sia9177.cat / charge.exe / credential.bin / ADSP（58 文件） |
| `qcom-bt` | `out/drv/qcom-bt` | qcom_bt.inf / qcom_bt.sys / qcom_bt.cat |
| `qcom-wlan` | `out/drv/qcom-wlan` | qcom_wlan.inf / qcom_wlan.sys / qcom_wlan.cat |
| `ti-batt` | `out/drv/ti-batt` | ti_batt.inf / ti_batt.sys / ti_batt.cat |
| `qcom-charge` | `out/qcom-charge/ARM64/Release/bin` | charge.exe / credential.bin |
| `qcom-cpupwr` | `out/drv/qcom-cpupwr` | qcom_cpupwr.inf / qcom_cpupwr.sys / qcom_cpupwr.cat |
| `qcom-keys` | `out/drv/qcom-keys` | qcom_keys.inf / qcom_keys.sys / qcom_keys.cat |
| `nxp-nfc` | `out/drv/nxp-nfc` | nxp_nfc.inf / nxp_nfc.dll / nxp_nfc.cat |
| `qcom-cam` | `out/drv/qcom-cam` | qcom_cam.inf/sys/cat、qcom_cam_video.inf/sys/cat、采集及枚举工具 |
| `qcom-gpu` | `out/drv/qcom-gpu` | qcom_gpu.inf/sys/cat、ARM64/x64 UMD、OpenGL、Turnip、运行库及 GPU 固件 |

编译中间文件位于 `out/<目标>/ARM64/Release`。驱动包中的 SYS 和 CAT、GPU 用户态驱动 DLL 及充电服务 EXE 均由构建脚本签名，安装时使用 `out/drv` 下的完整包。

`qcom-audio` 会自动构建 `qcom-charge`，将 `charge.exe`、`credential.bin` 和 58 个 ADSP 文件纳入音频包及 CAT。其余运行数据由 `install-fw` 复制。

`qcom-gpu` 构建 D3D10/11 前端、OpenGL、Turnip 和 WDDM 内核，同时生成 `out/drv/qcom-d3d12` 的 VKD3D-Proton 运行库。Mesa、着色器编译器和 VKD3D-Proton 的源码与编译缓存位于 `out/mesa`。

DWM 使用 D3D11 → Vulkan → Turnip；OpenGL 使用 Gallium Freedreno；D3D12 使用 VKD3D-Proton → Vulkan → Turnip。已移除未完成的原生 D3D12 前端和专用后端。

将 `out/drv/qcom-d3d12` 完整复制到设备，在管理员 PowerShell 中运行 `install-d3d12.ps1`，即可启用 ARM64/x64 共用的 D3D12 入口。安装器保留 Microsoft 运行库用于软件适配器和必需的系统入口，运行同一脚本并加 `-Uninstall` 可恢复系统入口。硬件 D3D12 使用已注册的 Turnip ICD，避免全局枚举进入 Dozen 后递归调用 D3D12。

D3D11 前端声明特性级 10_0；D3D12 的 12_0/12_1 能力由 VKD3D-Proton 与 Turnip 提供。接口与像素验证不等同于完整游戏兼容性验证。

### 名称与版本

源码组件目录采用“厂商－驱动类别”，使用 WLAN、BT、NFC、GPU、CPU、PWR、BATT 等常见缩写；touch、audio、sensors 保留明确的名称。芯片型号记录在硬件 ID、设备描述、固件分组和文档中。

驱动提供商和文件公司为 `AnyLaySys`；硬件制造商按对应设备填写 Synaptics、Qualcomm、NXP、Texas Instruments 等。

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
| `fw/gpu/gen80000_zap.mbn` | 随 GPU 驱动包安装到 `DriverStore/FileRepository/qcom_gpu.inf_arm64_<系统生成标识>`，由配套 UEFI 认证加载 | 1 固件 |

ADSP 随音频驱动包安装到 DriverStore。UEFI 在 Windows 启动前选择 `DriverVer` 最高且包含完整 MDT/DTB 的音频包，加载同一 ADSP 目录中的固件分段。目标 Windows 卷需在启动前可供 UEFI 读取。音频 replay 数据由 Windows 驱动读取。

其余固件和运行数据通过 `install-fw` 复制。首次配置时填写目标设备的原厂蓝牙地址；例中的 `W:` 替换为实际 ARM64 Windows 目标：

```powershell
.\build.cmd install-fw -WindowsDirectory "W:\Windows" -BluetoothAddress "XX:XX:XX:XX:XX:XX"
```

添加 `-WhatIf` 可预览复制计划；覆盖已有目标文件时添加 `-Force`。目标 Windows 目录须已存在，脚本会检查 ARM64 架构。

蓝牙地址转换为六字节小端 `address.bin`，保存在 `System32/drivers/wcn7861-bt`。未传入 `-BluetoothAddress` 时，使用目标上已有的有效地址文件。

## 在完整 Windows 中使用

1. 准备配套的 UEFI/ACPI，并配置目标 Windows 的测试签名和证书信任。
2. 将完整音频包通过 DISM 安装到目标 ARM64 Windows 镜像，使 ADSP 随包进入 DriverStore。CPU 调频驱动的离线安装步骤见 对应 CPU 驱动包。
3. 在首次启动驱动前，用 `install-fw` 部署上述运行数据和本机蓝牙地址。
4. 在目标手机的 ARM64 Windows 管理员终端中安装完整驱动包，例如：

```powershell
pnputil /add-driver ".\out\drv\syna-touch\syna_touch.inf" /install
pnputil /add-driver ".\out\drv\qcom-audio\sia9177.inf" /install
pnputil /add-driver ".\out\drv\qcom-bt\qcom_bt.inf" /install
pnputil /add-driver ".\out\drv\qcom-wlan\qcom_wlan.inf" /install
pnputil /add-driver ".\out\drv\ti-batt\ti_batt.inf" /install
pnputil /add-driver ".\out\drv\qcom-gpu\qcom_gpu.inf" /install
```

5. 重启设备。

## GPU 与 CPU

GPU 原生桌面路径为 DWM / D3D DDI → Vulkan 后端 → Turnip → WDDM → Qualcomm Adreno 830。DWM 硬件绘制、翻转和窗口拖动正常。设备名称为 `Qualcomm Adreno 830`，构建入口为 `build.cmd qcom-gpu`。下载、版本与工具链配置、编译和签名逻辑统一保存在 [build.cmd](build.cmd)，运行时生成的脚本和配置位于 `out/mesa`，最终驱动包位于 `out/drv/qcom-gpu`。`src/mesa` 仅保存驱动源码和 Mesa 源码补丁。

GPU 按负载选择原厂档位，每 20 ms 采样，正常最低 342 MHz；工作积压时加速提频，持续活动时短暂保持已提升的频率。温控读取有效 GPU TSENS 通道的最高温度：87°C 限至 734 MHz，99°C 限至 160 MHz，降到 81°C 退出紧急档，降到 78°C 解除温度限频。温度不可读时使用最低档，温控与额外频率上限优先于负载和固定频率请求。调频与提交队列分开同步。显示提交注册 MPO1/2/3 主平面回调，仅报告一个 RGB 主平面。

GPU 固定显存池为 64 MB。共享显存容量按 Windows 可见内存预算计算，并受剩余 GPU 地址空间约束，当前最大为 4 GB；物理页和页记录按需映射、分配与释放。

SM8750 CPU 两组核心各有原厂 16 档：六核组 384–3532.8 MHz，两核组 1017.6–4320 MHz。驱动按负载调频并支持 PSCI 保留上下文休眠。两个簇独立读取 CPU/cache TSENS：87°C 时分别限至 2400 / 2438.4 MHz，99°C 时降至最低档，81°C 退出紧急档，78°C 解除温度限频。升温和传感器失效立即收紧上限，冷却条件需持续 500 ms；Windows 请求仍约束频率上限。构建入口为 `build.cmd qcom-cpupwr`。

## 蓝牙与 USB

蓝牙使用独立的 GENI UART（`0x898000`、GSI 493），由 UEFI 的 `ACPI\HMRB7861` 节点提供寄存器和中断。Windows 原生蓝牙、配对、键鼠、A2DP 音乐、LE 扫描、GATT 广播和开关恢复正常。

USB 2.0 Host/OTG 支持自动插拔与充电器输入切换，主控和集线器使用 Windows 原生 USBXHCI/USBHUB3，UEFI 提供 `ACPI\PNP0D10` 节点。音频包中的 ADSP/PMIC 通道与 `ALSCharge` 服务共同管理 OTG 供电和角色切换，USB 外设正常工作。

## 电量与充电

`ti-batt` 读取原厂配置为 BQ28Z610 的双电芯电量计，并向 Windows 提供电池电量。`qcom-audio` 提供 PMIC GLINK、普通充电和 UFCS 充电通道，`qcom-charge` 的 `ALSCharge` 服务随音频包安装并自动启动，管理充电策略和 USB 角色。

UFCS 认证文件 `credential.bin` 随音频包进入 DriverStore，与 `charge.exe` 位于同一目录，由服务读取。

## 传感器

`qcom-sensors` 通过 Windows 标准 HID 传感器接口提供环境光、简单方向、三轴陀螺仪和三轴加速度计，复用 `qcom-audio` 的 ADSP 通道。陀螺仪单位为度/秒，加速度单位为 g（包含重力）；两者最小及默认报告间隔为 20 ms。自动旋转与三轴加速度共用一条加速度订阅：仅旋转时请求 9 Hz，三轴应用订阅时请求 50 Hz，三轴需求结束后恢复旋转频率。Windows Gyrometer 和 Accelerometer 接口已实机验证。ADSP 的融合旋转向量通过同一个 HID 姿态设备提供 Compass（磁北航向）、Inclinometer（倾角）和 OrientationSensor（四元数与旋转矩阵）三个标准接口，最小及默认报告间隔为 100 ms；三者共用一条订阅。

重力、线性加速度、相对姿态和地磁姿态也通过独立 HID 集合提供。磁力计、计步、活动状态和接近检测由同包的 UMDF SensorsCx 驱动提供，复用只读的 `QcomSensorData` 入口；入口只接受传感器查询。磁场单位为 µT；计步提供真实累计总步数，步态类型为 Unknown，累计时长不提供有效测量。活动状态接入 ADSP `activity_recognition`，支持 Unknown、Stationary、Walking、Running、InVehicle、Biking，置信度为 Low；固件未提供 Idle/Fidgeting。消息中的层级状态已按本机固件的发布器与消费端核对，步行事件已实测；RMD/AMD 保留为完整分类不可用时的静止/未知回退。跑步、骑行和乘车的实际识别准确性仍待实测。标准 API 已读到三轴磁场和活动状态；手持手机前后甩动后，ADSP 与 Windows 计步接口均读到累计计数 11，确认计步变化能够上报。实际步行准确性仍待验证。新增接口仅在应用订阅时轮询，订阅结束后 ADSP 需求在 5 秒内过期。HID 与原生接口统一使用 `QcomSensorData` 的单一读取 ABI，不包含原始事件采集或厂商服务探测接口。`qcom_sensors_service` 负责 ADSP 固件配置与注册初始化，是正常启动所需的服务。

原生驱动的承载控制器标记为不在设备管理器普通列表显示，原有四个标准接口继续供应用使用。接近检测通过 `ProximityDetection` 布尔字段提供，实机 Windows 事件已验证 0→1→0；不提供未经测量的毫米距离。SensorExplorer 部分页面可能不更新；交叉测试可使用 SensorInfo；接近距离未提供时不以 0 毫米代替。

实机没有注册 pressure 传感器，[OnePlus Ace 5 Pro 官方规格](https://www.oneplus.com/cn/ace-5-pro/specs)也未列出气压计；通用参考板的气压配置不能证明手机具备该硬件。目前不提供气压和气压高度。色温、环境色尚未接入标准接口，罗盘真北航向的磁偏角仍待校正。

修改 ADSP 通道后，依次运行 `build.cmd qcom-audio` 和 `build.cmd qcom-sensors`。传感器包同时生成、签名 HID 驱动和原生 SensorsCx 驱动，两份 INF 都需安装。原生接口使用 `ROOT\QCOMSENSORNATIVE`，独立霍尔项使用 `ROOT\AKMHALL`；两者复用同一份原生 INF，首次安装时需创建对应根设备实例。更新底层通道后重启设备。

AKM AK09970 霍尔传感器由 `qcom-keys` 通过专用 GPIO80/81 I²C 读取，ACPI 为按键设备声明对应的两个 GPIO 页；UEFI 保持原厂 `ldob1` 1.8 V 供电。`qcom-sensors` 额外提供名为 `AKM AK09970 Hall Sensor` 的 Windows CustomSensor：CustomValue0／1／2 为真实有符号 X／Y／Z 原始计数，CustomValue3 为三段位置（0 未确认、1 上、2 中、3 下）。不把未经校准的计数标作 µT。位置按原厂 X／Y 判定并经过两次采样确认；上档静音并关闭触控／按键振动，中档恢复声音但关闭振动，下档恢复声音和振动。静音由现有 `qcom_sensors_service` 调用 Core Audio 设置，不新增服务。实机已验证 AK09970 芯片识别与连续变化的真实三轴原始计数；三档声音／振动行为仍需逐档实测。各传感器源码平铺拆分，共有框架与文件对应关系见 [传感器源码说明](src/qcom-sensors/README.md)。

## 触屏反馈

触控及按键反馈由 `qcom-keys` 控制 PMIH010x 马达，每次播放 18 ms，使用高精度定时器，实际电压为 800 mV 和 1200 mV，振幅均为 `0xFF`。`syna-touch` 按接触 ID 分别记录每根手指：新增手指按下请求一次 800 mV 短振，在原有移动阈值内按住超过 495 ms 后请求一次 1200 mV 短振，抬起不振；移动超限或读取失败取消对应长按反馈。音量键单独按下上报一次音量按下／释放并请求一次 800 mV；按住 270 ms 后由按键驱动按 27 ms 节奏重复上报音量按下／释放，同时请求 800 mV 振动，系统音量连调与振动请求共用计时，播放和停机期间及停止后 27 ms 内的新触发忽略、不补振。电源键单独短按在抬起时请求一次 800 mV，单独按住 2.4 s 时请求一次 800 mV，之后抬起不再振。音量加／电源、音量减／电源组合成立时各请求一次 800 mV，并取消该次电源抬起反馈。`qcom-audio` 的扬声器音量和静音节点实际执行 24 位 PCM 增益处理，并通过已有内核控制设备提供当前音量上下限；音量键按住时到达相应极限播放两次 1200 mV，每次 18 ms，两次之间仍等待停止后 27 ms，随后停止重复反馈。极限模式在马达空闲后开始，开始前松开按键或离开极限会取消。触控、按键及音量协作不需要常驻用户态程序。版本沿用 `yy.M.d.revision`，日期和修订号由各驱动现有项目属性控制。

## 主摄

`qcom-cam` 控制高通 SM8750 相机平台，连接 Sony IMX906 主摄。当前包含主摄控制器和 AVStream 视频接口，可向 Windows 注册 IMX906 相机子设备；实机枚举和采集尚待重启完成驱动切换后验证。构建、配套 UEFI、诊断工具和运行限制见 [相机说明](src/qcom-cam/README.md)。

## 许可

触控目录保留上游 GPLv3 许可证；网络部分文件采用 BSD-3-Clause-Clear。各组件的许可证和固件来源记录随对应目录保留。
