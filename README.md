# PlantHub · 家庭智慧灌溉系统

PlantHub 面向家庭阳台，采用 ESP32-S3 在设备端运行。产品目标是通过手机、平板或电脑浏览器在家庭局域网内管理植物、查看环境和控制灌溉。

当前处于 **M0 收口阶段**。仓库提供 ESP32-S3 最小固件、Native 工具链测试和工程规范。首个正式软件版本目标为 `v0.1.0`，用于两盆植物的有人值守原型。

## 快速开始

准备 Git、pioarduino Core 6.1.18 和用于 Native 测试的 GCC。`PLATFORMIO_CORE_DIR` 指向本机正式工具链目录。在项目根目录使用 PowerShell 执行构建。依赖准备与版本见[开发指南](docs/DEVELOPMENT.md)。

```powershell
$plantHubPio = Join-Path $env:PLATFORMIO_CORE_DIR 'penv\Scripts\pio.exe'
& $plantHubPio run -e planthub_rev_b
& $plantHubPio test -e native
```

硬件版本为 V0.1 Rev B，ERC、泵控制安全和实物通电验收待完成。此阶段仅执行软件构建及主机测试，烧录和实物测试需项目负责人授权。见 [Rev B 硬件基线](hardware/rev-b/README.md)。

## 项目文档

- [架构、ADR 与 API](docs/ARCHITECTURE.md) · 模块依赖、存储职责及 REST 接口
- [产品与 UI 规范](docs/PRODUCT_SPEC.md) · 业务、数据口径、视觉基线及参考图
- [开发路线](docs/ROADMAP.md) · M0～M7 范围与阶段进度
- [开发约定](AGENTS.md) · 长期开发约束
- [变更记录](CHANGELOG.md) · 版本变更
