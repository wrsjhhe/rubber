# 渲染框架开发入口

功能与设计描述目标，进度文档记录代码事实。按下列顺序阅读和交接：

| 文档 | 用途 |
|---|---|
| [功能需求](docs/FEATURES.md) | 功能编号、范围、验收条件 |
| [技术方案](docs/TECHNICAL_DESIGN.md) | 数据语义、算法、同步、测试约束 |
| [架构设计](docs/ARCHITECTURE.md) | 组件、依赖、所有权和扩展边界 |
| [实现进度](docs/PROGRESS.md) | 当前证据、M1—M6 任务、缺项和审核状态 |
| [开发交接与审核](docs/CONTRIBUTING.md) | 实现、验证、更新进度及独立审核流程 |

M5（Observation、处理链入口、呈现隔离与互操作边界）已实现，现待审核：`include/sim/Observation.hpp` 用标定、采样时间与通道语义描述"这一帧看到了什么"，`Renderer::observe()` 与 `submit()` 同一次提交、只读返回；`include/sim/Processing.hpp` 的处理链只能新增派生输出，与输入通道同名或写未声明名字都整体拒绝；`src/presentation` 加上 `shaders/presentation.frag` 把调试通道从光照里彻底剥离，`lighting.frag` 只输出 Beauty，调试图写独立的 `debug` 图，因此同一帧的 Beauty 与调试视图互不串色。顺带补齐 `Channel::Resolved`（时序解析后的线性颜色，A 分量为该帧米制深度）。前序 M4（网格模块迁移、模块协议、点云）已通过审核（d3c0c1f）：`src/render/Module.hpp` 定义模块协议，`src/modules/mesh`、`src/modules/points` 并列注册，主调度只遍历接口；点云用实例化公告板写同一张深度与 G-buffer，与网格双向遮挡；姿态与点云身份基准只在时序解析真的执行、且提交成功后才落库。M3/M2 亦已验收。交接记录与缺项见 [实现进度](docs/PROGRESS.md)，审核流程见 [开发交接与审核](docs/CONTRIBUTING.md)。审核通过后下一项为 M6：安装头文件/库/shader、导出 target 与依赖。开发者按验收实现并更新至待审核；审核者针对确切版本独立核验，再决定是否已验收。

构建与运行见 [README](README.md)。旧实现背景见 [原架构说明](ARCHITECTURE.md)，框架目标以 docs/ARCHITECTURE.md 为准。不要在入口重复维护进度。
