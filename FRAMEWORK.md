# 渲染框架开发入口

功能与设计描述目标，进度文档记录代码事实。按下列顺序阅读和交接：

| 文档 | 用途 |
|---|---|
| [功能需求](docs/FEATURES.md) | 功能编号、范围、验收条件 |
| [技术方案](docs/TECHNICAL_DESIGN.md) | 数据语义、算法、同步、测试约束 |
| [架构设计](docs/ARCHITECTURE.md) | 组件、依赖、所有权和扩展边界 |
| [实现进度](docs/PROGRESS.md) | 当前证据、M1—M6 任务、缺项和审核状态 |
| [开发交接与审核](docs/CONTRIBUTING.md) | 实现、验证、更新进度及独立审核流程 |

M6（安装分发，F10）已实现并按审核修复，现待复审：`cmake --install` 产出自包含前缀——`include/sim` 公共头文件、随包安装的 glm、静态库、预编译 SPIR-V 与 `lib/cmake/SimRenderer` 包文件；外部项目 `find_package(SimRenderer)` 后链接 `sim::renderer` 即可，包配置只 `find_dependency(Vulkan 1.3)`（着色器已预编译，消费者不需要 glslc），导出 target 以 PUBLIC 编译特性传播 C++20，消费者不必自设标准。二进制里的默认 shader 目录指向配置时的安装前缀而非构建目录，运行时按"显式目录 → `SIM_SHADER_DIR` → 可执行文件相邻布局 → 编译期安装目录"解析，构建树靠 exe 相对路径命中；`shader_check`（无 GPU）锁解析顺序与诊断，`package_check`（CTest）装全新前缀并让只认 `find_package` 的外部项目构建、离屏渲染——配置前缀只做只读检查（有合法安装则必须成功命中、没有则必须失败并点名），两个默认查找分支由测试目录内的隔离构建覆盖，脚本的写入与递归清理全部限制在自己的测试前缀内并经路径校验。前序 M5（Observation、处理链入口、呈现隔离与互操作边界）已通过审核（428d264）：输出描述只取自结果本身、处理链只能新增派生输出、调试图写独立图像且光照规划随调试通道纳入。M4/M3/M2 亦已验收。交接记录与缺项见 [实现进度](docs/PROGRESS.md)，审核流程见 [开发交接与审核](docs/CONTRIBUTING.md)。审核通过后下一项为 M7（F11 镜头畸变）。F11—F17 的规格与质量/性能门槛已写入 [功能需求](docs/FEATURES.md)，按 M7—M13 顺序立项：每项开工前把该节门槛落成测试清单，性能数字依赖 M7 内交付的 `gallery --bench`；F16b/F16c 等未补门槛的子项不得开工。开发者按验收实现并更新至待审核；审核者针对确切版本独立核验，再决定是否已验收。

构建与运行见 [README](README.md)。旧实现背景见 [原架构说明](ARCHITECTURE.md)，框架目标以 docs/ARCHITECTURE.md 为准。不要在入口重复维护进度。
