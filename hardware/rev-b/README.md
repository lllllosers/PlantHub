# PlantHub V0.1 Rev B 硬件基线

当前为预设计。两张 EDA 图已导入并核对图面，正式 ERC 和整机通电验收待完成。M0-B 仅构建固件和执行主机测试。

## 设备与供电

| 部位 | 器件 |
| --- | --- |
| 主控 | ESP32-S3 N16R8 与 44 针螺丝端子扩展板 |
| 采集 | ADS1115 四通道 16 位 ADC、两支 V1.2 电容式土壤湿度探头、GY-SHT31 |
| 执行 | YYNMOS-4 四路 MOS 驱动板、NKP-DC-S10B 12V 蠕动泵、385 型 12V 隔膜泵 |
| 供电及辅材 | 12V 3A 电源适配器、独立 5V 手机充电头、两脚机械自锁开关、面包板与杜邦线 |

主控 USB 供电为 5V，传感器为 3.3V。水泵独立使用 12V 电源，两脚开关串接动力正极，动力线路绕开面包板。信号地与动力地分开以驱动板有效隔离为前提。光照、液位、漏水和流量传感器当前未安装。DS3231 预留，当前不安装、不接线。无有效日期时间的设备行为见[产品规范](../../docs/PRODUCT_SPEC.md#时间与历史)。

## 板级配置

[BoardProfile](../../src/platform/esp32/board/BoardProfile.h) 是代码中物理 GPIO、I2C 参数及 ADC 通道的唯一配置来源。ADS1115 的 ADDR 接地，探头 1、2 依次使用 `soilAdcChannels` 中的通道。`pumpGpios` 依次对应蠕动泵和隔膜泵。其余泵、ADC、缺水、漏水及 RTC 接口由 `reserved` 常量记录。SHT31 地址以实物扫描为准。MOS 有效电平待实测，M0-B 固件不配置水泵 GPIO。

[PlatformIO 配置](../../platformio.ini) 复用 pioarduino 的 `esp32-s3-devkitc1-n16r8`。构建候选为 16 MiB Quad Flash 和独立的 8 MiB Octal PSRAM，使用 QIO Flash 与 `qio_opi` Arduino 内存组合。实际模组丝印、Flash 和 PSRAM 容量及类型、USB 接口与烧录设置须在实物验收时核对。

## Flash 布局

[partitions.csv](../../partitions.csv) 定义实际布局。引导程序和分区表位于首个 APP 之前。NVS 独立分配 20 KiB，OTA 元数据沿用 Arduino 引导镜像约定，两个 4 MiB APP 分区为后续 OTA 预留。Arduino 预编译 SDK 启用了 Flash 崩溃转储，沿用 Flash 末尾 64 KiB 系统分区。剩余数据区用于 LittleFS 历史。

LittleFS 使用 Arduino 默认标签 `spiffs` 和兼容子类型 `data,spiffs`，PlatformIO 文件系统类型指定为 `littlefs`。未来挂载使用 `formatOnFail=false`，故障处理见[产品规范](../../docs/PRODUCT_SPEC.md#时间与历史)。M2 实测文件系统挂载和容量，并核对配置体积、NVS 使用量、配置更新余量及多盆扩展需求。

## 图纸索引

| 图纸 | 文件及用途 |
| --- | --- |
| PH-EL-001 网络标签式原理图 | [EDA 电气设计主源](schematic/PH-EL-001.json) · [PNG 预览](schematic/PH-EL-001.png) |
| PH-EL-002 全导线电气接线图 | [EDA 派生视图](schematic/PH-EL-002.json) · [SVG 预览](schematic/PH-EL-002.svg) |
| PH-WR-001 全引脚实物接线图 | [PNG](wiring/PH-WR-001.png) · [SVG](wiring/PH-WR-001.svg) |
| PH-WR-002 面包板孔位图 | [PNG](wiring/PH-WR-002.png)，孔位以实物通断验证为准 |

修订时保持派生视图与电气设计主源一致。图纸或实物存在冲突时停止接线，先核对实物。

## 验收边界

待完成 ERC、主控型号和容量核验、扩展板通断、探头输出电压、MOS 有效电平、驱动板有效隔离及续流保护、开关直流额定值、电源短路及过流保护。实物须验证 ESP32 未启动及上电、复位、掉电时的默认停泵行为，再进行真实采集、水泵运行及整机通电验收。硬件条件核实前禁止连接水泵通电，烧录及实物试验由项目负责人授权后进行。长期无人值守需单独验收硬件防护和稳定性。
