# Windows for OnePlus Ace 5 Pro

`woa_oneplus_ace5_pro` 是 OnePlus Ace 5 Pro / PKR110（设备代号 `hummer`，Qualcomm SM8750）的 **Windows on ARM 设备适配工程**，包含触控、音频、无线驱动源码，运行所需固件/配置，以及统一构建和固件复制入口。

本仓库不是 Windows 操作系统源码，也不构建完整 Windows 安装镜像。Linux 适配由 `linux_oneplus_ace5_pro` 维护，UEFI 由 `uefi_oneplus_ace5_pro` 维护；Windows 驱动仍依赖与本设备匹配的 UEFI/ACPI。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/syna-s3910` | Synaptics S3910 触控驱动及上游许可证 |
| `src/qcom-audio` | Qualcomm ADSP 音频与 SIA9177 功放适配 |
| `src/qcom-wcn7861` | Qualcomm WCN7861 Wi-Fi 驱动 |
| `fw/adsp` | UEFI 启动阶段加载的 ADSP 主固件与 DTB 数据集 |
| `fw/sia9177` | 音频运行数据 replay.bin |
| `fw/wcn7861` | 网络初始化所需的七个固件/配置文件 |
| `AnyLaySys.cer` | 项目共用的公开测试签名证书，不含私钥 |
| `build.cmd` | 驱动构建、签名及固件复制的统一入口 |
| `out/drv` | 最终签名的 INF / SYS / CAT 驱动包 |
| `out/syna-s3910`、`out/qcom-audio`、`out/qcom-wcn7861` | 编译中间文件和 WDK 打包暂存 |

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
build.cmd qcom-wcn7861
build.cmd --help
```

PowerShell 中对应使用 `.\build.cmd`，例如：

```powershell
.\build.cmd qcom-audio
```

每次只构建指定组件，固定使用 **Release / ARM64**。脚本在对应源码目录内查找名称匹配的唯一 vcxproj，因此不要求三个组件具有相同的内部目录层级。

构建完成后，对正式包中的 SYS 签名，重新生成 CAT 并签名，输出位置为：

| 目标 | 正式驱动包 | 文件名 |
| --- | --- | --- |
| `syna-s3910` | `out/drv/syna-s3910` | syna_s3910.inf / syna_s3910.sys / syna_s3910.cat |
| `qcom-audio` | `out/drv/qcom-audio` | sia9177.inf / sia9177.sys / sia9177.cat |
| `qcom-wcn7861` | `out/drv/qcom-wcn7861` | wcn7861.inf / wcn7861.sys / wcn7861.cat |

编译中间文件位于 `out/<目标>/ARM64/Release`。其中 bin 及其子目录可能保留 WDK 生成的**未签名暂存文件**，与正式签名包的大小和哈希不一定相同。安装时使用 `out/drv` 下配套的三件套，不要混用不同位置的 INF、SYS、CAT。

构建命令不复制固件，不编译辅助或排障程序，不连接手机，也不安装驱动或重编 UEFI。

### 名称与版本

硬件制造商分别为 Synaptics 或 Qualcomm，驱动提供商和文件公司为 AnyLaySys。`qcom-audio` 表示高通平台音频适配，不表示 SIA9177 功放芯片属于高通。

版本采用 `yy.M.d.revision`：

- 触控和音频默认按构建日期生成；现有 S3910VersionDate / S3910Revision、SiaVersionDate / SiaRevision 环境变量可覆盖相应日期和修订号。
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
```

以上不是给 x64 开发主机安装手机驱动的命令。目标 Windows 还需要具备驱动使用的 WDF、PortCls/KS、WiFiCx/NetAdapterCx 等原生系统组件，不应从 PE 直接复制不同版本的 Windows DLL/SYS 来补齐。

5. 重启后分别检查触控、扬声器和无线连接。固件复制完成不等于驱动已经安装，也不等于完整 Windows 兼容性已经验证。

## 验证状态与许可

当前源码布局下，三个 Release ARM64 目标已实际完成编译、测试签名及 INF/CAT 完整性检查；合并入口的固件复制和参数处理已通过23项隔离测试。测试包含目录映射、预览不写入、覆盖保护、缺失输入和 x64 目标拒绝。

这些是主机构建与隔离复制验证，**没有据此宣称在新装完整 Windows 上完成实机回归**。脚本使用测试签名，不是 Qualcomm / Synaptics 官方或 WHQL 签名。

触控目录保留上游 GPLv3 许可证；网络部分文件已有 BSD-3-Clause-Clear 标记。源码、固件和其他数据不应被视为自动适用同一许可证。固件的来源记录和哈希不代表再分发授权；相应许可需分别保留和确认。