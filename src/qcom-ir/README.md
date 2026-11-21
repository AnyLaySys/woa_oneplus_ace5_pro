# 红外遥控

`ir.c` 提供高通 SPI 红外发射驱动，`power.c` 提供只有一个“Samsung M5 电源”按钮的 Windows 程序。每次点击发送一帧电源切换码，不重复发送。

| 项目 | 配置 |
|---|---|
| ACPI 设备 | `ACPI\HMRI0001` |
| 设备管理器 | 系统设备：Qualcomm Infrared Transmitter |
| 控制器 | QUP1 SE5，`0x00A94000/0x4000` |
| 发射引脚 | GPIO53，`qup1_se5_l1`，`0x0F135000/0x1000` |
| 中断 | GSI 390 |
| 供电 | `ldob9`，3.056 V |
| 时钟 | TCXO 19.2 MHz，SE 分频 168 |
| 载波 | 38.095 kHz，1/3 占空比 |
| 电源码 | Samsung 32 位 `E0E040BF` |
| 单帧 | 1,544 字节，108.08 ms，包含结束间隔 |

使用 `build.cmd qcom-ir` 同时编译驱动及按钮程序，并生成签名安装包 `out\drv\qcom-ir`。UEFI 已新增专用 ACPI 资源和红外供电；修改后由 UEFI RELEASE 构建及 `linux_oneplus_ace5_pro/build.py -nr` 生成配套 boot。

驱动使用硬件 FIFO、中断和 DPC 完成发送。请求最长 250 ms，出错后执行有界中止并清理 DPC；空闲一秒后进入低功耗状态，只释放 SE5 时钟票，保留其他 QUP 使用者。开机及空闲时将发射引脚固定为低电平。无常驻服务、轮询定时器、调试输出或注释。

`\\.\QcomIr` 支持同步 `WriteFile`，输入为固定采样率 114,285.714 bit/s、按字节从最高位开始的 MOSI 波形，长度为 1～2,048 字节。三个位生成一个载波周期；三星编码由按钮程序生成，驱动不内置家电型号。

原厂硬件配置依据 OnePlusOSS 的 `hummer` 设备树和 `oplus_consumer_ir` 驱动；三星码与时序依据 IRDB 和 IRremoteESP8266。Samsung M5 的实际开关响应仍需刷入配套 boot 后实测；传输完成只表示发射器发送完毕。
