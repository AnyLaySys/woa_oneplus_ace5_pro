# Windows for OnePlus Ace 5 Pro

`woa_oneplus_ace5_pro` 是 OnePlus Ace 5 Pro / PKR110（设备代号 `hummer`，Qualcomm SM8750）的 **Windows on ARM 设备适配工程**，包含触控、音频、Wi-Fi、蓝牙、电量计驱动源码及 USB Host 电源角色管理，运行所需固件/配置，以及统一构建和固件复制入口。

本仓库不是 Windows 操作系统源码，也不构建完整 Windows 安装镜像。Linux 适配由 `linux_oneplus_ace5_pro` 维护，UEFI 由 `uefi_oneplus_ace5_pro` 维护；Windows 驱动仍依赖与本设备匹配的 UEFI/ACPI。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/syna-s3910` | Synaptics S3910 触控驱动及上游许可证 |
| `src/qcom-audio` | Qualcomm ADSP 音频、SIA9177 功放适配及共享 PMIC 通道上的 USB Host 电源管理 |
| `src/qcom-wcn7861` | Qualcomm WCN7861 Wi-Fi 驱动 |
| `src/qcom-bluetooth` | Qualcomm WCN7861 UART HCI 蓝牙传输适配；原生蓝牙、配对、键鼠与A2DP已验证 |
| `src/qcom-battery` | Qualcomm 电量计（bq27xxx，hub I2C）剩余电量显示驱动 |
| `fw/adsp` | UEFI 启动阶段加载的 ADSP 主固件与 DTB 数据集 |
| `fw/sia9177` | 音频运行数据 replay.bin |
| `fw/wcn7861` | 网络初始化所需的七个固件/配置文件 |
| `fw/wcn7861-bt` | 蓝牙外围控制器、TMEL、主蓝牙的五个原始固件/NVM 文件 |
| `AnyLaySys.cer` | 项目共用的公开测试签名证书，不含私钥 |
| `build.cmd` | 驱动构建、签名及固件复制的统一入口 |
| `out/drv` | 最终签名的 INF / SYS / CAT 驱动包 |
| `out/syna-s3910`、`out/qcom-audio`、`out/qcom-wcn7861`、`out/qcom-battery` | 编译中间文件和 WDK 打包暂存 |

`fw` 保存的是运行输入，不是可随意删除的构建缓存；`out` 是可再生的产物目录，已由 `.gitignore` 排除。当前 `src` 不包含独立的测试程序、诊断工具、媒体或 IDE 个人配置。

## 构建环境

当前脚本使用 Windows 主机上的以下固定工具路径：

- Visual Studio 18 Insiders：`C:\Program Files\Microsoft Visual Studio\18\Insiders`。
- WDK：`10.0.26100.0`，包含对应的内核驱动工具集、ARM64 编译环境、SignTool 和 Inf2Cat。
- 签名证书：脚本通过 `CERT` 指定的指纹查找当前用户 My 证书存储中的证书，使用其私钥签名。

请在已配置上述环境的管理员终端中运行。`AnyLaySys.cer` 只有公开证书，**不能单独用来签名**，也不会被脚本自动导入。其他开发环境需要自行准备匹配的工具链和有权限使用的签名身份，不能只复制 cer 后就直接完成签名。

## 编译驱动

在仓库根目录运行：

```cmd
build.cmd syna-s3910
build.cmd qcom-audio
build.cmd qcom-bluetooth
build.cmd qcom-wcn7861
build.cmd qcom-battery
build.cmd --help
```

PowerShell 中对应使用 `.\build.cmd`，例如：

```powershell
.\build.cmd qcom-audio
```

每次只构建指定组件，固定使用 **Release / ARM64**。脚本在对应源码目录内查找名称匹配的唯一 vcxproj，因此不要求各组件具有相同的内部目录层级。

构建完成后，对正式包中的 SYS 签名，重新生成 CAT 并签名，输出位置为：

| 目标 | 正式驱动包 | 文件名 |
| --- | --- | --- |
| `syna-s3910` | `out/drv/syna-s3910` | syna_s3910.inf / syna_s3910.sys / syna_s3910.cat |
| `qcom-audio` | `out/drv/qcom-audio` | sia9177.inf / sia9177.sys / sia9177.cat |
| `qcom-bluetooth` | `out/drv/qcom-bluetooth` | qcom_bluetooth.inf / qcom_bluetooth.sys / qcom_bluetooth.cat |
| `qcom-wcn7861` | `out/drv/qcom-wcn7861` | wcn7861.inf / wcn7861.sys / wcn7861.cat |
| `qcom-battery` | `out/drv/qcom-battery` | qcom_battery.inf / qcom_battery.sys / qcom_battery.cat |

编译中间文件位于 `out/<目标>/ARM64/Release`。其中 bin 及其子目录可能保留 WDK 生成的**未签名暂存文件**，与正式签名包的大小和哈希不一定相同。安装时使用 `out/drv` 下配套的三件套，不要混用不同位置的 INF、SYS、CAT。

构建命令不复制固件，不编译辅助或排障程序，不连接手机，也不安装驱动或重编 UEFI。

### 名称与版本

硬件制造商分别为 Synaptics 或 Qualcomm，驱动提供商和文件公司为 AnyLaySys。`qcom-audio` 表示高通平台音频适配，不表示 SIA9177 功放芯片属于高通。

版本采用 `yy.M.d.revision`：

- 触控、音频、蓝牙和电量计默认按构建日期生成；现有 S3910VersionDate / S3910Revision、SiaVersionDate / SiaRevision、QcbVersionDate / QcbRevision、QbtVersionDate / QbtRevision 环境变量可覆盖相应日期和修订号。
- 网络当前固定为 `26.11.2.0`，版本头、INF 和工程中的 StampInf 日期/版本需要保持同步。

源码目录名称不会改变驱动文件名、服务名、ACPI 硬件 ID 或运行时读取路径。

## 复制固件和运行数据

固件在仓库中按组件扁平存放，由 `install-fw` 子命令映射到实际目录：

| 输入 | 目标位置 | 当前数量 |
| --- | --- | --- |
| `fw/adsp` | UEFI 可访问的 ALS 分区根目录下 `ADSP` | 58 文件 |
| `fw/sia9177/replay.bin` | 目标 Windows 目录下 `System32/drivers/sia9177/replay.bin` | 1 文件 |
| `fw/wcn7861` | 目标 Windows 目录下 `System32/drivers/wcn7861` | 7 文件 |

ADSP 固件在 Windows 启动前由 UEFI 加载；replay.bin 则由 Windows 音频驱动读取，两者不能互相替代。网络七文件中包含 `qdss_trace_config.bin`，当前初始化流程会读取它，不能因文件名带 trace 就省略。

至少明确指定 WindowsDirectory 或 AlsRoot 中的一个。下面的 W:、A: **只是示例盘符**，必须替换成实际目标；Windows 和 ALS 可以在同一分区。

先预览，不写入文件：

```powershell
.\build.cmd install-fw -WindowsDirectory "W:\Windows" -AlsRoot "A:\" -WhatIf
```

确认目标后复制：

```powershell
.\build.cmd install-fw -WindowsDirectory "W:\Windows" -AlsRoot "A:\"
```

只传 WindowsDirectory 时复制音频和网络八个文件；只传 AlsRoot 时复制 ADSP 数据集。目标根目录必须已经存在；WindowsDirectory 会检查目标内核的 ARM64 标识，以避免误写 x64 开发主机。

**已有目标文件时必须显式加 `-Force` 才会覆盖，即使内容可能相同。** 建议先用 -WhatIf 预览；确认是预期替换后再使用：

```powershell
.\build.cmd install-fw -WindowsDirectory "W:\Windows" -AlsRoot "A:\" -Force
```

此子命令只复制固件/配置，不安装驱动或证书，不修改注册表、BCD 或 Secure Boot。它不读取 manifest 或执行 SHA-256 校验，也没有 VerifyOnly 模式；只保留源文件存在性、目标路径/架构及覆盖保护。

`fw/manifest.json` 和 `fw/SHA256SUMS` 保留来源与哈希记录，供单独核查，不由复制子命令自动验证。

## 在完整 Windows 中使用

1. 使用同一设备所需的 UEFI/ACPI，并把 ADSP 数据集放在其可访问的 ALS 根目录 ADSP 下。
2. 在驱动首次启动前，将音频 replay 和网络固件复制到目标 Windows 的上述固定位置。
3. 将正式签名的驱动包传入目标设备，按目标系统的测试签名要求配置签名环境。公开 cer 文件不等于已经建立信任；本脚本不会自动完成这一步。
4. 在**目标手机的 ARM64 Windows** 中，以管理员身份安装完整驱动包，示例：

```powershell
pnputil /add-driver ".\out\drv\syna-s3910\syna_s3910.inf" /install
pnputil /add-driver ".\out\drv\qcom-audio\sia9177.inf" /install
pnputil /add-driver ".\out\drv\qcom-wcn7861\wcn7861.inf" /install
pnputil /add-driver ".\out\drv\qcom-battery\qcom_battery.inf" /install
```

以上不是给 x64 开发主机安装手机驱动的命令。目标 Windows 还需要具备驱动使用的 WDF、PortCls/KS、WiFiCx/NetAdapterCx 等原生系统组件，不应从 PE 直接复制不同版本的 Windows DLL/SYS 来补齐。

5. 重启后分别检查触控、扬声器、无线连接和电池电量。固件复制完成不等于驱动已经安装，也不等于完整 Windows 兼容性已经验证。

## 验证状态与许可

触控、音频、Wi-Fi 的 Release ARM64 构建以及此前23项固件复制隔离测试保留为已有验证记录。新增蓝牙目标已完成编译、测试签名及 INF/CAT 检查，H4 包边界解码通过 ASan/UBSan 测试；本次另行验证了新增固件映射、预览不写入、覆盖保护、蓝牙地址格式及字节序、Force 写入和 x64 目标拒绝。

这些是主机构建与隔离复制验证，**没有据此宣称在新装完整 Windows 上完成实机回归**。脚本使用测试签名，不是 Qualcomm / Synaptics 官方或 WHQL 签名。

触控目录保留上游 GPLv3 许可证；网络部分文件已有 BSD-3-Clause-Clear 标记。源码、固件和其他数据不应被视为自动适用同一许可证。固件的来源记录和哈希不代表再分发授权；相应许可需分别保留和确认。

## 蓝牙适配状态

蓝牙使用独立的 GENI UART（`0x898000`、GSI 493），不是 Wi-Fi 的 PCIe 通道。更新后的 UEFI 通过 `ACPI\HMRB7861` 交付 UART 寄存器与中断；旧 UEFI 没有此节点，不能直接绑定本驱动。需先编译 UEFI，再在 `linux_oneplus_ace5_pro` 运行 `python3 build.py -nr` 生成匹配的 boot。

当前已验证 115200/8 Mbps 内部回环、8 Mbps 控制器通信、完整固件初始化，以及 Windows 原生 BTHX 中断收发。26.11.6.4 正式包已确认实际加载，Bluetooth Radio、经典蓝牙/LE 枚举器及 RFCOMM 正常启动；OPPO K5 和一加 Ace 5 均已成功配对、枚举服务，并各自完成一次超过35秒的 RFCOMM/OBEX 连接保持测试，未发送文件。用户已确认键鼠与耳机 A2DP 音乐实测通过；本轮验证 LE 被动扫描、本机 GATT 广播和蓝牙开关恢复。实际大文件传输、远端 GATT 数据交互、整机睡眠恢复等尚未完成验证，不能据此宣称所有蓝牙功能已完成。

运行文件应位于目标 Windows 的 `System32\drivers\wcn7861-bt`。除仓库中的五个文件外，还需要本机原厂蓝牙地址。该地址属于每台设备的配置，不作为固件源文件上传；安装时可显式提供：

```cmd
build.cmd install-fw -WindowsDirectory "W:\Windows" -BluetoothAddress "XX:XX:XX:XX:XX:XX" -WhatIf
```

替换为目标设备的实际地址，确认预览后去掉 `-WhatIf`，已有文件按需加 `-Force`。脚本将地址转换为控制器所需的六字节小端 `address.bin`，不会修改源固件中的默认地址，也不会生成签名证书或安装驱动。没有提供地址时保留目标上已配置的地址；首次启用蓝牙前必须完成配置。

首版仅接入 HCI Command / Event / ACL；SCO 通话音频和 ISO / LE Audio 数据通路尚未实现。驱动不包含自定义文件日志或调试控制接口，排障程序不放入 `src`。


### 蓝牙后续适配状态（2026-11-06）

用户已确认蓝牙键鼠和耳机A2DP音乐实测通过。额外验证了LE被动扫描、本地GATT服务广播及Windows蓝牙Off/On；远端GATT数据交互尚未通过本次测试。控制器上报5.4和CIS/BIS能力，但Windows当前仅开放31字节广播，尚未实现HFP旁路音频与LE Audio的ACX/VSAP数据通路，不能宣称与原厂全部规格一致。

源码和正式包现为 26.11.6.4，保留主机 IBS 空闲休眠/发送前唤醒、重试和超时清理，并完成等价精简，C/H 源码净减少 859 字节，无生产日志或测试接口。14,097 组接收解析等价测试、IBS 10 万轮状态测试、包长边界和五条固件指令逐字节对比通过。已加载新版实机通过 8 轮空闲后查询、LE 扫描、原生蓝牙开关、开关后扫描和本机 GATT 广播；244 条 HCI 记录无命令错误、硬件错误或丢失，测试跟踪已停止。本轮“空闲后收发恢复＋原生蓝牙开关恢复＋LE 本机基础能力”子目标完成。该验证不直接捕获原始 IBS 控制字节，不代表完成实际电流、主机时钟门控、整机睡眠/远程唤醒或实机故障注入验收，也不宣称功耗/吞吐达到绝对上限。HFP 通话和 LE Audio 通路尚未实现。

## USB 适配状态

接口按设备实际规格仅支持 **USB 2.0 Host/OTG 与充电器输入切换**，不实现 Device/MTP/ADB 或 USB 3.x。主控与集线器使用 Windows 原生 USBXHCI/USBHUB3；匹配的 UEFI 需提供 `ACPI\PNP0D10` 节点。

自动插拔、OTG 5V 源端开关和供电方向切换复用音频驱动已有的 ADSP 边缘及 PMIC 通道，随 `build.cmd qcom-audio` 构建，不需要独立 USB 驱动包、常驻工具或手工角色切换命令。源端设为 500mA，并保留外部输入、端口状态、电池有效性、超时与关闭回读检查；不设置充电输入电流、充电电压或快充协议。电池净充电由充电适配另行处理。

已完成鼠标 Host 枚举/操作实测及自动策略的软件回归；软件回归不等同于所有外设和持续插拔的实机认证。正式源码不包含 USB 实验控制接口、追踪缓冲或测试程序。
