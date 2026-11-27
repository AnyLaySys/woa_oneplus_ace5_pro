# Windows for OnePlus Ace 5 Pro

`woa_oneplus_ace5_pro` 是 OnePlus Ace 5 Pro / PKR110（设备代号 `hummer`，Qualcomm SM8750）的 Windows on ARM 适配工程，包含设备驱动、运行固件及统一构建、部署入口。

Windows 驱动配合 `uefi_oneplus_ace5_pro` 的 UEFI/ACPI 使用；Linux 适配由 `linux_oneplus_ace5_pro` 维护。

## 适配状态

| 🟢原生屏幕 | 🟢多指触控 | 🟡指纹识别 |
| --- | --- | --- |
| 🟢物理按键 | 🟢GPU驱动 | 🟢无线网络 |
| 🟢蓝牙连接 | 🟢近场通信 | 🟢OTG连接 |
| 🟢音频输出 | 🟢音频输入 | 🟢振动反馈 |
| 🟢电量显示 | 🟢快速充电 | 🟢CPU调频 |
| 🟢GPU调频 | 🟢红外遥控 | 🟢三轴加速 |
| 🟢三轴陀螺 | 🟢三轴磁场 | 🟢重力向量 |
| 🟢线性加速 | 🟢屏幕朝向 | 🟢磁北航向 |
| 🟢相对姿态 | 🟢绝对姿态 | 🟢地磁姿态 |
| 🟢霍尔感应 | 🟢接近感应 | 🟢光照感应 |
| 🟢色温感应 | | |

## 驱动组件

| 组件 | 硬件与功能 |
| --- | --- |
| `syna-touch` | Synaptics S3910 多点触控 |
| `qcom-audio` | Qualcomm ADSP 音频、SIA9177 功放、WCD9395 编解码器、普通及 UFCS 充电、USB 2.0 Host/OTG |
| `qcom-wlan` | Qualcomm WCN7861 Wi-Fi |
| `qcom-bt` | Qualcomm WCN7861 蓝牙、配对、键鼠、A2DP、LE 与 GATT |
| `ti-batt` | Texas Instruments BQ28Z610 双电芯电量计、电池电量显示 |
| `qcom-cpupwr` | SM8750 CPU 调频、温控及休眠 |
| `qcom-keys` | 物理按键、PMIH010x 振动、AKM AK09970 三段拨键 |
| `qcom-sensors` | 环境光、色温、自动旋转、加速度、陀螺仪、磁场、姿态、计步、活动状态、接近检测及霍尔传感器 |
| `nxp-nfc` | NXP SN220T NFC 读卡接口 |
| `jiiov-fp` | Jiiov JV0301 WBDI 指纹设备接口 |
| `qcom-ir` | 红外发射及 Samsung M5 电源遥控 App |
| `qcom-cam` | Sony IMX906 主摄控制与 AVStream 视频接口 |
| `qcom-npu` | Qualcomm Hexagon NPU 的 CDSP、FastRPC、Windows MCDM 调度、ONNX Runtime 执行提供程序与 GGML 聊天后端 |
| `qcom-gpu` / `mesa` | Adreno 830 硬件桌面、D3D10/11、D3D12、OpenGL 与 Vulkan，GPU 调频及温控 |

触控按下、长按及物理按键提供振动反馈，音量到达上下限时提供双振反馈。三段拨键上档静音并关闭振动，中档恢复声音并关闭振动，下档恢复声音和振动。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/<组件>` | 设备驱动源码 |
| `src/mesa` | GPU 用户态后端、Mesa 补丁及运行库安装脚本 |
| `src/qcom-ir/app` | 红外遥控 App |
| `src/qcom-npu/ollama` | Ollama 官方前端与 NPU 后端适配 |
| `fw` | 运行固件、配置及来源记录 |
| `AnyLaySys.cer` | 公开测试签名证书 |
| `build.cmd` | 构建、签名及固件部署入口 |
| `out/drv` | 签名驱动包及运行载荷 |
| `out/app` | 应用程序产物 |

编译中间文件位于 `out/<组件>/ARM64/Release`，GPU 源码和编译缓存位于 `out/mesa`。`out` 已由 `.gitignore` 排除，可重新生成。

## 构建

在 Windows 上准备以下环境：

- Visual Studio 18 Insiders：`C:\Program Files\Microsoft Visual Studio\18\Insiders`。
- WDK `10.0.26100.0`：包含 ARM64 工具链、SignTool 和 Inf2Cat。
- GPU 交叉编译：已配置工具链的 WSL `Ubuntu-26.10`，可用 `WOA_WSL_DISTRO` 指定其他发行版。
- 签名证书：默认使用当前用户 My 证书存储中的 `AnyLaySys` 证书及私钥，指纹为 `FF7908A360512CF12E08C0BD99625830C1EA4E6D`，可用 `WOA_SIGN_CERT` 覆盖。
- UFCS 认证材料：构建音频前准备 `fw/charge/credential.bin`，大小为 16 字节。

在仓库根目录的管理员 PowerShell 中运行：

```powershell
.\build.cmd all
.\build.cmd qcom-audio
.\build.cmd --help
```

`all` 编译全部驱动；单组件可指定 `qcom-sensors`、`qcom-gpu` 等目标，完整列表见 `--help`，配置统一为 **Release / ARM64**。红外遥控 App 单独运行 `.\build.cmd ir-remote`。

驱动包输出到 `out/drv/<组件>`，安装时复制完整目录。特殊产物如下：

| 目标 | 包内容或输出 |
| --- | --- |
| `qcom-audio` | 音频、充电及 USB 管理驱动，附带 `ADSP` 固件和 `credential.bin` |
| `qcom-sensors` | HID 驱动、原生 SensorsCx 驱动及 `QcomSensors` 服务，两份 INF 均需安装 |
| `qcom-cam` | 主摄控制器、AVStream 视频驱动，两份 INF 均需安装 |
| `qcom-npu` | FastRPC 与 MCDM 两份驱动、ONNX Runtime 执行提供程序及 `CDSP` 固件 |
| `qcom-gpu` | WDDM 驱动、ARM64/x64 用户态后端及固件；D3D12 运行库另存于 `out/drv/qcom-d3d12` |
| `ir-remote` | `out/app/ir-remote` |
| `npu` | `out/app/npu` |
| `ollama-npu` | `out/app/ollama-npu` |

充电和 USB 角色管理集成在音频驱动中，`qcom-charge` 构建目标为 `qcom-audio` 的别名。修改 ADSP 通道后，依次构建 `qcom-audio` 和 `qcom-sensors`。

## 固件部署与安装

目标设备使用配套 UEFI/ACPI，ARM64 Windows 启用测试签名并信任 `AnyLaySys.cer`。UEFI 启动前需能读取目标 Windows 卷。

1. 在首次启动前，将完整音频包安装到离线 Windows 镜像，使 ADSP 固件进入 DriverStore。CPU 调频驱动也在离线阶段部署。以下 `W:` 替换为目标 Windows 卷：

   ```powershell
   dism /Image:W:\ /Add-Driver /Driver:".\out\drv\qcom-audio\sia9177.inf"
   dism /Image:W:\ /Add-Driver /Driver:".\out\drv\qcom-cpupwr\qcom_cpupwr.inf"
   ```

2. 复制运行数据，首次配置时填写本机原厂蓝牙地址：

   ```powershell
   .\build.cmd install-fw -WindowsDirectory "W:\Windows" -BluetoothAddress "XX:XX:XX:XX:XX:XX"
   ```

   `-WhatIf` 预览复制计划，`-Force` 覆盖已有文件。后续更新可省略蓝牙地址，沿用目标上已有的有效 `address.bin`。

3. 将完整 `out/drv` 目录复制到设备，在目标手机的管理员 PowerShell 中安装：

   ```powershell
   pnputil /add-driver ".\out\drv\*.inf" /subdirs /install
   ```

   根设备首次安装时需创建对应实例：CPU 调频为 `ROOT\QCOM_CPUPWR`，传感器为 `ROOT\QCOMALS`、`ROOT\QCOMSENSORNATIVE` 和 `ROOT\AKMHALL`。传感器运行依赖音频驱动及随包安装的 `QcomSensors` 服务。

4. NPU 首次部署使用配套 UEFI 和 `linux_oneplus_ace5_pro/build.py -nr` 生成的 `boot.img`。将 `src/qcom-npu/deploy.ps1` 一并复制到设备，在管理员 PowerShell 中运行：

   ```powershell
   .\src\qcom-npu\deploy.ps1 -Package .\out\drv\qcom-npu
   ```

   `build.cmd qcom-npu` 编译 FastRPC 驱动、MCDM 计算驱动、原生 ONNX Runtime 执行提供程序和固定 b12359 版本的 GGML Hexagon 后端，并自动获取和校验 V79 DSP 运行库。`deploy.ps1` 安装两个驱动并创建或更新唯一的计算设备；固件、运行库、`qcom_npu_ep.dll` 和 `GGML/qcom_npu_ggml.dll` 全部由 DriverStore 管理。

   ARM64 ONNX Runtime 1.24+ 程序可通过 `RegisterExecutionProviderLibrary` 注册 `qcom_npu_ep.dll`，选择 `QcomHtpExecutionProvider`。`src/qcom-npu/ep/client.h` 提供自动定位 DriverStore、注册和选择设备的辅助函数：

   ```cpp
   Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "my-app");
   const OrtEpDevice* device = qcom_npu::Register(environment);
   Ort::SessionOptions options;
   qcom_npu::Attach(environment, options, device);
   Ort::Session session(environment, L"model.onnx", options);
   ```

   原始 ONNX 支持静态 FP32、最多四维的 Add/Sub/Mul/Div、Relu/Sigmoid/Tanh/Exp/Sqrt/Abs/Neg、Softmax、二维 MatMul、部分 Conv/MaxPool 和常量形状 Reshape。支持的节点由 Hexagon 执行，其他节点可由 ONNX Runtime 的 CPU 提供程序执行；`Attach` 的第四个参数设为 `true` 可禁止 CPU 回退。也保留一个静态 FP16 输入和输出的预编译 HTP `EPContext` 接口。

   驱动构建同时生成签名的 `qcom_npu_ep.msix`。部署脚本在当前交互用户下注册 Windows ML 执行提供程序扩展，支持使用新 Windows ML 提供程序目录的应用发现设备。DirectML 的 Direct3D 12 用户态驱动接口尚未实现；自带旧版、不同架构或私有运行时的软件仍需适配。

   `build.cmd ollama-npu` 构建并签名 Ollama 0.40.18 的 Windows ARM64 本地适配版，完整产物位于 `out/app/ollama-npu`，适配源位于 `src/qcom-npu/ollama`。构建自动获取并校验 Go 与交叉编译器，设备无需 SDK。该包包含相同版本官方源码编译的 ARM64 图形前端，保留官方界面、模型管理、命令行和 HTTP 接口。

   复制完整产物到设备，运行 `install.ps1` 安装到 `%LOCALAPPDATA%/Programs/Ollama`，加入用户 PATH 并由官方前端管理后台服务与登录启动。桌面“Ollama”打开图形对话界面。使用正常的 `ollama pull`、`ollama run` 下载和运行模型，无需在下载时指定 NPU；`ollama ps` 显示模型的 CPU/NPU 分配，`ollama stop` 释放模型。第三方客户端可使用 `http://127.0.0.1:11434` 或 OpenAI 兼容地址 `http://127.0.0.1:11434/v1`。

   Ollama 从已安装驱动自动定位 Hexagon 后端并刷新旧的自有路径。默认要求模型前向计算的全部算子由 V79 HTP/HMX/HVX 执行；不支持的算子会报错，禁止静默回退到 CPU。输入嵌入也放在 NPU，分词、采样和程序控制仍使用 CPU。默认使用 sustained_high_performance 功耗模式、上下文 2048、批次 128、两个驻留模型和每模型一个推理请求，保留显式设置。模型大小、量化格式和上下文缓存仍影响能否完整加载；默认严格检查由 `GGML_NPU_REQUIRE_FULL_OFFLOAD=1` 控制。官方安装包更新会替换本地适配后端，升级后需重新构建并安装本仓库版本。

   驱动为新进程注册机器级 `GGML_BACKEND_PATH`，指向 DriverStore 的 Hexagon 后端，保留用户已有的第三方路径。兼容动态后端 ABI 3 的 ARM64 GGML 程序可通过标准 `ggml_backend_load_all()` 枚举 `HTP0: Qualcomm Hexagon V79 NPU`；封装的私有后端、x64 程序或不同 ABI 仍需适配。

   MCDM 将实际推理提交给 Windows 调度，依据 DSP 完成通知累计执行时间。模型、KV 缓存和计算缓冲区登记为已有系统内存，完成驻留后再提交，避免另外分配后备副本，供 Windows 按进程和适配器统计真实共享占用。任务管理器的 2 MiB 专用段是驱动预留的调度内存，模型使用共享系统内存。配套 UEFI 暴露完整 SMMU 资源后，模型缓冲区使用物理地址 4 GiB 以上的非连续内存，预算取该范围 Windows 可见内存并预留 1 GiB；低地址内存留给固件通信。V79 的 32 位虚拟地址通过约 1.75 GiB 的映射窗口复用，可处理总占用超过 4 GiB 的模型；单个张量连同保护页仍须不超过 512 MiB。驱动最多接入八个独立推理进程，共用全局内存预算。MCDM 为各进程管理独立虚拟地址空间并按时间片共享执行引擎，RPC 回复、缓冲区和 DSP 保护域按进程隔离；未执行的任务在抢占后重新提交，退出一个进程会释放其资源。驱动在执行前恢复所需映射，并回收空闲缓冲区的映射以复用窗口。多个模型能否同时驻留仍取决于可用内存。同步 RPC 使用高精度休眠等待，页表采用批量写入；静态缓冲区默认按 256 MiB 分组以减少映射往返。设置用户环境变量 `GGML_HEXAGON_POWER_MODE=balanced` 可切换平衡模式，重启 Ollama 后生效。

5. 重启设备。

固件由驱动包安装或由 `install-fw` 复制，位置如下：

| 输入 | 安装位置 |
| --- | --- |
| `fw/adsp`、`fw/charge/credential.bin` | 音频包的 DriverStore 目录，ADSP 由 UEFI 预加载 |
| `fw/cdsp` | NPU 包的 DriverStore 目录，由 UEFI 认证加载 |
| `fw/gpu/gen80000_zap.mbn`、`fw/gpu/vpu35_4v.mbn` | GPU 包的 DriverStore 目录 |
| `fw/gpu` 的 GMU/SQE/AQE 固件 | Windows 目录下 `System32/drivers/qcom-gpu` |
| `fw/sia9177`、`fw/wcd9395` | Windows 目录下 `System32/drivers` 的同名目录 |
| `fw/wcn7861`、`fw/wcn7861-bt` | Windows 目录下 `System32/drivers` 的同名目录，蓝牙地址存为 `address.bin` |

D3D12 使用 VKD3D-Proton 与 Turnip。将完整 `out/drv/qcom-d3d12` 复制到设备，在管理员 PowerShell 中运行该目录的 `install-d3d12.ps1`；加 `-Uninstall` 可恢复系统入口。

## 许可

触控目录保留上游 GPLv3 许可证，网络部分文件采用 BSD-3-Clause-Clear。各组件的许可证及固件来源记录保留在对应目录中。
