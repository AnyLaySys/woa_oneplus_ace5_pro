# 固件与运行配置

这里按组件扁平保存运行输入，不模拟 Windows 的多层目录，也不因源目录名改变驱动读取路径。当前目录无需统一加 qcom 前缀；尤其 SIA9177 不是高通芯片。源文件的名称、内容和原始许可保持不变。

| 仓库路径 | 复制目标 | 数量 |
| --- | --- | --- |
| `fw/adsp/*` | ALS 分区根目录的 `ADSP/*` | 58 |
| `fw/sia9177/replay.bin` | 目标 Windows 目录下的 `System32/drivers/sia9177/replay.bin` | 1 |
| `fw/wcn7861/*` | 目标 Windows 目录下的 `System32/drivers/wcn7861/*` | 7 |

ADSP 由 UEFI 在 Windows 启动前加载；replay 是音频图/参数/功放数据；网络七个文件均由当前初始化流程使用，包括不能省略的 `qdss_trace_config.bin`。触控没有额外外部数据文件需要复制。

这些文件与 2026-11-02 从手机 G: 保全的快照逐项哈希一致，来源、目标位置和 SHA-256 保存在 manifest.json、SHA256SUMS。校验完整不代表已在全新完整 Windows 上实测通过。文件仍适用于同一设备和匹配的 UEFI/ACPI；公开分发前应分别确认固件再分发许可，不能自动套用源码许可证。

## 校验

在 WoA 根目录运行：

```powershell
.\install-fw.ps1 -VerifyOnly
```

不指定任何目标时，也只校验，不写入任何位置。

## 复制到目标系统

明确指定目标 Windows 目录以及 UEFI 可访问的 ALS 分区根目录。W:、A: 只是示例，必须换成实际目标；两者可以是同一分区。脚本不会把仓库中的目录名直接套到 Windows 根目录，而是按上表映射。

先预览：

```powershell
.\install-fw.ps1 -WindowsDirectory 'W:\Windows' -AlsRoot 'A:\' -WhatIf
```

确认后复制：

```powershell
.\install-fw.ps1 -WindowsDirectory 'W:\Windows' -AlsRoot 'A:\'
```

也可以只传一个目标：WindowsDirectory 仅复制 replay 和网络七文件，AlsRoot 仅复制 ADSP。目标根目录必须已经存在。WindowsDirectory 会检查目标内核的 ARM64 PE 标识，避免把手机固件装到 x64 开发主机；不要绕过这个检查。

复制前会校验全部66个输入并预检所有目标。目标内容相同就跳过；已有不同文件则拒绝覆盖，只有明确指定 `-Force` 才允许替换。-WhatIf 不创建目录或文件。复制完成后逐文件核对 SHA-256。

本脚本只复制固件/运行数据，不安装驱动、不导入证书、不改注册表或启动配置，也不编译或运行日志/诊断程序。源文件保留在 fw，目标写入需要足够权限。

## 证书与驱动

公开测试证书位于 WoA 根目录 `AnyLaySys.cer`，供项目共用，**不含私钥**；它不是固件，本脚本不会安装它。当前签名仍使用原测试证书身份，文件重命名不等于换证书。

驱动三件套仍由 `build.cmd` 构建到 `out/drv/<名称>`，与固件复制分开。运行固件就位后，还需要在目标 ARM64 Windows 配置匹配的测试签名环境并正确安装 INF/SYS/CAT。不要从 PE 复制 Windows 自带组件，也不要将 WinPE 整体启动脚本套用于完整 Windows。
