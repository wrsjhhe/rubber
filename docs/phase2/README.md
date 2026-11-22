# 第二阶段：视觉传感器流水线

状态：设计与开发计划，尚未实现或验收。建立日期：2026-11-21。基线为 M6 已验收代码 `67e11ec`，配套文档 `e9edd86` 及其后的独立复审记录。

本目录独立维护第二阶段，第一阶段 M1—M6 文档保留在上级目录，不改写历史验收。功能编号沿用 F11—F17，开发任务使用 S2-xx，审核问题使用 R-S2-xx-001。本目录的 PROGRESS 是第二阶段唯一进度来源。

| 文件 | 用途 |
|---|---|
| [FEATURES.md](FEATURES.md) | 本轮范围、输入输出和明确不做的能力 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 组件职责、所有权和扩展边界 |
| [TECHNICAL_DESIGN.md](TECHNICAL_DESIGN.md) | 数学模型、时间、颜色与数据契约 |
| [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) | 开发顺序、任务交付与依赖 |
| [VALIDATION.md](VALIDATION.md) | 数值、集成、寿命与性能验收 |
| [PROGRESS.md](PROGRESS.md) | 实际状态、证据和交接模板 |

本阶段目标：外部应用可以取得理想观测，并组合镜头畸变、有限曝光和可复现噪声，输出带完整元数据的派生传感器图像；原始深度与实例标签保持只读。提供离屏传感器示例，并为资产验证器提供机器可读的检查结果。

建议按 S2-00 → S2-01 → S2-02 → S2-03 → S2-04 → S2-05 顺序推进，每项独立提交、审核。第一批可以开始 S2-00，暂不进入雨雪或高斯开发。

现有 API 名称是代码事实；本目录提到的 SensorPipeline、CaptureSequence、CaptureResult 等是拟议职责名，不能当作已存在的接口。开发者须在对应任务中提交最终头文件方案和使用示例。

S2-00 若使用当前 Gallery 作为画面回归场景，采用 PCSS 默认参考图，并记录阴影模式及参数。M6 截图用于历史对照。参见 [阴影技术契约](../TECHNICAL_DESIGN.md#方向光阴影) 与 [默认画面基线](../../example/preview/courtyard-pcss.png)。

阅读历史：[第一阶段功能](../FEATURES.md)、[技术契约](../TECHNICAL_DESIGN.md)、[M6 复审](../REVIEW_M6.md)。本阶段对曝光输入、派生链和时间元数据的调整只约束新增路径，不追溯改变现有 Channel 和 observe() 的语义。
