# 框架架构设计

本文是目标边界，实际状态见 [PROGRESS](PROGRESS.md)。旧版实现说明保留在根目录 ARCHITECTURE.md，发生冲突时以本目标设计和进度记录为准。

借鉴 three.js/threepp 的几何、材质、实例分离，Filament 的场景/视图职责划分及 UE RDG 的资源依赖声明理念；不承诺兼容其 API 或移植其代码。库不依赖 GDS Core，由应用适配层产生快照。

## 目标组件

| 目录 | 职责 | 禁止耦合 |
|---|---|---|
| include/sim | 应用接口、结果、模块协议 | 原生 Vulkan 访问放独立互操作头 |
| src/scene | 节点、变换、ID、可见性、包围盒、类型批次快照 | GPU 分配与窗口 |
| src/assets | 导入、资产身份和缓存 | 每相机历史 |
| src/backend | Vulkan 设备、内存、上传、命令与同步 | 场景类型业务分支 |
| src/render | RenderScene、View、FrameContext 和请求编排 | 所有模块上传/绘制内联在主调度 |
| src/render/graph | 声明资源、依赖编译、转换、生命周期 | 材质与点云算法 |
| src/render/passes | 阴影、几何、光照、TAA、颜色变换、读回 | 隐式访问未声明资源 |
| src/modules/mesh、points | 类型资产、上传、可见性、pass 注册 | 要求主调度增加类型判断 |
| src/sensors | 校准、观测元数据与处理链 | 改写原始真值 |
| src/presentation | 调试输出、显示转换 | 改写其他消费者的颜色 |
| example | GLFW/ImGui 应用和资产 | 成为核心库必需依赖 |

无需提前创建空目录。Scene 的 mesh 专用字段通过兼容适配逐步迁移；点云不能被要求伪装成三角网格。截至 M5，`src/render/graph`、`src/render/passes`、`src/modules`、`src/sensors` 与 `src/presentation` 都已按表中职责存在：帧图只做声明、裁剪、依赖与屏障推导，不接触材质或点云语义；每个 pass 只录制自己的绘制，不隐式转换未声明的资源；`src/modules/mesh` 与 `src/modules/points` 是并列注册的模块，主调度只遍历 `RenderModule`/`ModuleView` 接口，没有类型分支；`src/sensors` 只产出标定、采样语义与处理链结果，观测对象对 `FrameResult` 只读、处理链只能新增派生输出，两者都无法改写真值；`src/presentation` 只写自己的调试图像，与 Beauty/HDR 是并列的消费者而不是覆盖者。三个方向都不允许反向依赖：`src/render` 不因调试呈现改变光照输出，`src/sensors` 不进入录制路径。

## 所有权

- Scene 由应用修改；SceneSnapshot 固定实例、灯光、时间和不可变资产引用，并按值保存材质。
- AssetCache 按资产身份缓存几何与贴图、按内容缓存材质；身份构造时分配且不复用。清理只移除缓存引用，未完成提交仍保活资产。
- RenderScene 持有共享 GPU 数据及上传依赖；不能放某台相机的 oldModel。
- View 持有分辨率、相机相关状态、历史注册表和失效版本。
- FrameContext 持有当次帧槽、命令和临时资源，安全完成后回收。
- FrameResult 持有当次输出、完成信号和元数据，不被下一帧覆盖。
- Observation 持有观测结果及校准/采样语义；构造时校验采样时刻与结果帧时间一致，之后只读。
- ProcessingChain 持有阶段与全链声明，`run()` 产出派生输出并同时保活源观测；它不持有任何可写入口通向真值通道。

设备必须活得比所有依赖它的资源更久，异常路径也遵守同步与销毁顺序。

## 数据流

应用 → 快照 → RenderScene/模块准备 → RenderRequest + View → FrameGraph → Vulkan 提交 → FrameResult → Observation → Sensor 处理链或 Presentation。

两条下游是并列的、互不覆盖的消费者：观测包一层只读元数据交给处理链，处理链只能新增派生输出；呈现从同一份 G-buffer 另出一张调试图。帧图里 Beauty 的链路（`lighting` → `temporal` → `color`）不读调试通道，调试链路（`presentation`）不写 `color` 的附件。

上层使用下层接口，backend 不反向依赖具体模块。模块通过上下文声明资源/pass，不访问 Renderer::Impl 私有字段。历史作为持久导入，结果作为导出。

## 迁移顺序

共享场景 → 帧图/pass → 网格模块化与点云验证 → 观测/呈现分离 → 安装消费。每阶段保持 Gallery 和现有测试工作。公开 API 变更需迁移示例；旧 render/read 保留最近帧语义，新接口提供保活结果。

当前 Renderer.hpp 仍含 Vulkan 类型，新增互操作头不代表公开接口已后端无关。目标目录也不代表功能已经实现。
