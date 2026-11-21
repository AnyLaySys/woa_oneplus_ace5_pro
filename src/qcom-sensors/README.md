# 传感器源码

每种传感器使用一个 `.inc` 文件，集中存放其 HID 声明、SEE 解码或 SensorsCx 元数据与上报逻辑。各模块随共用框架编译，复用原有驱动包与服务。

| 文件 | 传感器 |
|---|---|
| `light.inc` | 环境光 |
| `accelerometer.inc` | 三轴加速度 |
| `simple_orientation.inc` | 简单朝向 |
| `gyroscope.inc` | 三轴陀螺仪 |
| `orientation.inc` | 绝对姿态 |
| `gravity.inc` | 重力向量 |
| `linear_acceleration.inc` | 线性加速度 |
| `magnetometer.inc` | 三轴磁力计 |
| `relative_orientation.inc` | 相对姿态 |
| `geomagnetic_orientation.inc` | 地磁姿态 |
| `proximity.inc` | 接近检测 |
| `pedometer.inc` | 计步 |
| `activity.inc` | 活动分类 |
| `hall.inc` | 霍尔磁场、三段位置与静音控制 |

`hid.c` 管理 HID 队列、报告和电源；`native/sensor.cpp` 管理 SensorsCx 生命周期、属性和轮询。`see.inc` 复用消息解析、缓存和订阅，`rpc.inc` 处理 ADSP 通信，`service.cpp` 处理固件注册与文件请求。共享协议定义在 `abi.h`、`see.h` 和 `rpc-abi.h`。

模块按 `HID_SENSOR`、`SENSOR_SEE`、`SENSOR_NATIVE` 选择编译部分；`hall.inc` 另外由现有服务及按键驱动分别通过 `SENSOR_SERVICE`、`SENSOR_HARDWARE` 复用。模块包含顺序保留原报告顺序与依赖关系。

运行 `C:\DESIGN\WoA\build.cmd qcom-sensors` 同时构建并签名 HID、原生接口和服务，输出到 `out\drv\qcom-sensors`。修改共享 SEE／RPC 或霍尔硬件代码后，同时构建 `qcom-audio` 或 `qcom-keys`。

原生驱动同一份 INF 支持 `ROOT\QCOMSENSORNATIVE` 和 `ROOT\AKMHALL`：前者提供磁力计、计步、活动分类、接近检测；后者提供设备管理器可见的 AKM AK09970 霍尔传感器。
