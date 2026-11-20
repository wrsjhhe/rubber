# 渲染框架开发入口

功能与设计描述目标，进度文档记录代码事实。按下列顺序阅读和交接：

| 文档 | 用途 |
|---|---|
| [功能需求](docs/FEATURES.md) | 功能编号、范围、验收条件 |
| [技术方案](docs/TECHNICAL_DESIGN.md) | 数据语义、算法、同步、测试约束 |
| [架构设计](docs/ARCHITECTURE.md) | 组件、依赖、所有权和扩展边界 |
| [实现进度](docs/PROGRESS.md) | 当前证据、M1—M6 任务、缺项和审核状态 |
| [开发交接与审核](docs/CONTRIBUTING.md) | 实现、验证、更新进度及独立审核流程 |

M2（共享 RenderScene、不可变资产版本与上传依赖）已实现，并按首轮审核意见修复 R-M2-001 至 R-M2-004，现待复审，交接记录见 [实现进度](docs/PROGRESS.md)、问题处理见 [审核回应](docs/REVIEW_M2_RESPONSE.md)。复审通过后下一项为 M3：帧图及六类独立 pass，资源声明与分配分离。开发者按验收实现并更新至待审核；审核者针对确切版本独立核验，再决定是否已验收。

构建与运行见 [README](README.md)。旧实现背景见 [原架构说明](ARCHITECTURE.md)，框架目标以 docs/ARCHITECTURE.md 为准。不要在入口重复维护进度。
