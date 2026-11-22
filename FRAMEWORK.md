# 渲染框架开发入口

功能与设计描述目标，进度文档记录代码事实。按下列顺序阅读和交接：

| 文档 | 用途 |
|---|---|
| [功能需求](docs/FEATURES.md) | 功能编号、范围、验收条件 |
| [技术方案](docs/TECHNICAL_DESIGN.md) | 数据语义、算法、同步、测试约束 |
| [架构设计](docs/ARCHITECTURE.md) | 组件、依赖、所有权和扩展边界 |
| [实现进度](docs/PROGRESS.md) | 当前证据、M1—M6 任务、缺项和审核状态 |
| [开发交接与审核](docs/CONTRIBUTING.md) | 实现、验证、更新进度及独立审核流程 |

M1—M6 已验收。安装包导出 `sim::renderer` 并传播 C++20 要求。离屏消费者通过 `find_package(SimRenderer)` 使用，着色器随包安装。详细验收见 [M6 审核](docs/REVIEW_M6.md)。

方向光阴影已扩展为默认 CSM + PCSS + 屏幕空间接触阴影，Ray Query 可选增强接触遮挡。参考 [阴影技术方案](docs/TECHNICAL_DESIGN.md#方向光阴影)、[审核记录](docs/REVIEW_SHADOW.md) 和 [实现进度](docs/PROGRESS.md)。

第二阶段的实施顺序见 [视觉传感器流水线](docs/phase2/README.md)。F11—F17 的功能编号及既有规格继续在 [功能需求](docs/FEATURES.md) 维护。开发者提交各任务的测试证据后，由审核者对确切版本独立核验。

构建与运行见 [README](README.md)。旧实现背景见 [原架构说明](ARCHITECTURE.md)，框架目标以 docs/ARCHITECTURE.md 为准。不要在入口重复维护进度。
