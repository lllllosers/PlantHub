# 开发指南

## 工具链

项目统一使用用户级 `pioarduino-core` 6.1.18，Windows CLI 位于 `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`，工具链数据位于 `%USERPROFILE%\.platformio`。用 `--version`、`system info` 和同一环境的 `python -m pip check` 核对 Core、Python、路径和依赖。`pioarduino-core` 与官方 `platformio` 共享 Python 模块和 CLI 入口，同一个 Python 环境只保留 `pioarduino-core`。工程配置使用相对路径，日常构建和项目库缓存统一保存在 `.pio/`。

固件平台、Native 平台和 Unity 的版本由 [platformio.ini](../platformio.ini) 固定。pioarduino 的版本清单固定 Arduino-ESP32 3.3.6、SDK 库包 `5.5.0+sha.f56bea3d1f` 和 Xtensa GCC `14.2.0+20251107`。库包标签用于标识分发包。S3 SDK 的 ESP-IDF 版本以 `esp_idf_version.h` 为准，当前为 5.5.2。[Arduino 发布说明](https://github.com/espressif/arduino-esp32/releases/tag/3.3.6)

Windows Native 测试使用 [WinLibs GCC](https://winlibs.com/) 或可用的 MinGW-w64 GCC。用 `where.exe gcc`、`where.exe g++`、`gcc --version` 和 `g++ --version` 核对测试终端实际使用的编译器。刚添加 `PATH` 时，重启 VS Code 或开发会话以继承新的环境。主机 GCC 与 Xtensa GCC 各自编译对应目标，项目源码统一使用 C++17。

官方 VS Code PlatformIO IDE 扩展可选，当前验收使用 CLI。日后由项目负责人选择启用时，先在本机设置 `platformio-ide.useBuiltinPIOCore` 为 `false`，并让 `platformio-ide.customPATH` 包含现有 Core 的 `Scripts` 目录，或让扩展继承已有的相同 `PATH`。在扩展终端核对 CLI 路径和版本后再构建。[扩展设置](https://docs.platformio.org/en/latest/integration/ide/vscode.html#settings)

## 工具安装权限

下载、安装和升级工具、扩展、Python 与虚拟环境、Core、平台、SDK、编译器及依赖由项目负责人手动操作。Codex 检查组件和兼容性、提供官方来源与具体命令、维护项目内工程配置，使用已具备的工具构建和测试。未经明确授权，禁止创建额外环境、调用安装工具或修改系统环境变量。

构建前核对 CLI 路径和 Core 数据目录、平台和工具包、实际编译器及 SDK 文件、测试依赖和 `PATH`。pioarduino 的归档、解包载荷及注册包分别位于用户级 `dist/`、`tools/` 和 `packages/`。预检须检查实际载荷，例如 `tools/toolchain-xtensa-esp-elf/bin/` 中的编译器，以及 `tools/tool-esptoolpy/` 中的工具版本，同时核对 `packages/` 的注册状态和用户级 Python 实际加载的 esptool。pioarduino 解析项目包时也可能启动安装流程。发现缺项或尚需注册时，由项目负责人在项目根目录的 PowerShell 手动补齐。

```powershell
$plantHubPio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
& $plantHubPio --version
& $plantHubPio pkg install -e planthub_rev_b -e native
```

优先复用同一工具链及项目库缓存。已有重复环境须先确认替代环境通过固件构建和 Native 测试，再清理已确认无用的项目级副本。保留用户级工具链与 `.pio/` 的有效缓存。

## 构建与验证

```powershell
$plantHubPio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
& $plantHubPio run -e planthub_rev_b
& $plantHubPio test -e native
```

- `planthub_rev_b` 编译 `src/` 中的固件源码。嵌入式测试筛选在 `test/embedded/` 下，随实物验证需求添加。M0-B 的 `main.cpp` 只完成 Arduino 入口及空闲等待。
- `native` 以测试命令为入口。M0-B 项目源码筛选为 `src/core/`，测试筛选为 `test/native/`，工具链冒烟测试使用 Unity 和标准 C++。M1 引入可移植的 Application 服务后，按实际用途将其纳入 Native 编译和测试范围。
- 两个环境都通过编译期断言核验 C++17。固件使用 `-std=gnu++17`，以支持 ESP-IDF 头文件的 `typeof` 和 FreeRTOS 的可变参数宏扩展。Native 使用 `-std=c++17` 验证可移植源码。固件构建须检查 APP 限额、Flash/RAM 和警告，使用框架分区工具核对 [分区文件](../partitions.csv) 及生成的表。主机测试须检查编译器、测试计数及实际编译的源码。
- 硬件候选配置及实物验收范围见 [Rev B 硬件基线](../hardware/rev-b/README.md)。烧录和实物试验按其中的授权约束执行。

## Git 与版本

`main` 为长期主分支，按阶段开设短期英文分支。M0-B 使用 `feat/m0-foundation`。阶段验收后，经项目负责人授权合并及清理分支。提交说明使用简体中文，阶段进度记入 [ROADMAP.md](ROADMAP.md)，版本变更记入 [CHANGELOG.md](../CHANGELOG.md)。硬件与软件各自管理版本，正式软件 Tag 使用语义化版本。普通阶段提交无需 Tag。禁止擅自改写历史、删除正式发布记录或清理用户未提交工作。

提交前运行 `git diff --check`、`git diff --cached --check`、`git status`、`git log -1 --oneline` 和 `git branch -a`。只暂存任务范围内的文件，并检查暂存清单、差异和最终提交内容。公开仓库严禁提交 Wi-Fi 密码、设备密码、Token、私钥或真实个人网络配置。

## 项目卫生

- 优先维护和整合现有正式文档。每份文档只承担独立、长期的职责，规则、参数和项目状态在对应来源维护，其他文档通过链接引用。实现或配置改变后同步更新。及时整合或删除已经过时、重复和被替代的文档，历史由 Git 追溯。新文档须有独立的长期用途，避免副本、备份及版本后缀。阶段开发报告、验收结果及修复记录直接在 Codex 对话交付。
- 日常构建统一使用默认 `.pio/`，普通阶段开发无需独立输出目录和交付包。用后及时清理本阶段产生的一次性脚本、日志、测试文件，只有持续复用的工具进入 `tools/`。Git 仅跟踪所需源码和正式资料，排除缓存、固件二进制、日志和运行数据。正式交付产物在正式发布时生成，历史产物原则上由 GitHub Releases 管理。保留用户原有资料及权属未确认的文件。
- 正式替代旧实现时删除失去用途的旧代码及兼容路径，及时移除未使用的依赖、导入、变量、函数和模块。按当前需求引入模块、抽象与依赖，保持合理分层。当前阶段引入的卫生问题在当前阶段解决。
- 测试围绕业务规则、安全和实际使用闭环，规则变化时同步维护。清除重复、针对废弃实现或已无验证价值的测试和诊断程序，保留真实回归覆盖。测试数量及覆盖率不能替代验证价值。

阶段提交前核对文档与实现是否一致、事实来源是否唯一、文档是否有效、代码和兼容路径是否仍在使用、抽象和依赖是否必要、测试是否对应当前规则、临时文件是否清理、构建产物位置是否正确、Git 清单及目录结构是否克制，以及是否越过阶段范围。阶段报告在对话中简述 Project Hygiene 检查结果。
