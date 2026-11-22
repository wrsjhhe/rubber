# 第二阶段架构

## 现有基础与缺口

M6 已有只读 Observation、保活 FrameResult、CPU ProcessingChain 和模块化帧图。但现有 SensorStage 只读取原观测，不能读取其他阶段的派生输出；SampleTime.duration 仅是元数据；单个 SceneSnapshot 不含可重建完整运动的轨迹。不能把三个缺口写成已经支持。

## 组件与依赖

| 组件（拟议） | 职责 | 不负责 |
|---|---|---|
| SensorPipeline | 阶段声明、依赖排序、类型检查、执行与输出归属 | 场景运动、Vulkan 私有资源 |
| CaptureSequence | 给定时间返回不可变场景快照与相机状态；由应用实现 | 渲染器推测速度或修改 GDS 状态 |
| CaptureScheduler | 展开曝光采样、调用 Renderer、限制在途数量、汇总结果 | 镜头方程、噪声模型 |
| CaptureResult | 参考 Observation、采样计划、派生数据、完成状态与来源 | 冒充单帧 FrameResult |
| LensStage / NoiseStage / DisplayStage | 自己声明的只读输入与新输出 | 改写真值或隐式重绘场景 |
| ExposureAccumulator | 接收线性时间样本、累计 FP32、归一化 | 时间调度、图像配准、跨曝光 TAA |

建议实现位置：公共接口仍在 include/sim；调度在 src/sensors/capture；链与阶段在 src/sensors/processing；算法在 src/sensors/models；示例在 example/sensors 与 example/asset_validator；测试按领域放 tests。实现时才创建目录。

依赖方向：应用 → CaptureScheduler → Renderer → 帧图/模块；CaptureScheduler → SensorPipeline → 数学模型。模型不依赖窗口或 Renderer::Impl。CPU 首版调度可以等待 GPU 完成，但不能把隐式等待塞进 Renderer::submit()。

## 处理链契约

输入引用区分原始 Channel 与派生输出键；每阶段声明所需输入、输出和支持的格式。先构图，检测缺失引用、循环、重名、维度/格式/单位不兼容和内存预算，再执行。顺序相同的无依赖节点使用稳定顺序。消费者只能读取已完成的声明输入，写自己的已分配输出。

新增只读数据视图，不返回可修改 DerivedOutput；描述与存储布局由执行器持有。失败不返回部分成功结果，不把阶段副作用写入源观测。外部文件写入属于应用层，不能在通用阶段执行。

保留旧 ProcessingChain 的源观测处理方式，通过适配器接入；不直接改变已有 SensorStage 的纯虚函数使现有消费者失效。新增接口的最终命名、适配代价及安装消费者编译测试属于 S2-01。

## 所有权与有界执行

输入观测和派生结果在消费者持有期间保持有效；CPU 数据由结果拥有，不能返回阶段栈地址。临时输出可在最后一个消费者完成后回收，公开结果不可被下次执行覆盖。

CaptureSequence 在一个捕获请求内须返回同一配置版本的数据；调度器在提交前复制相机并保活快照。它不保存用户临时引用，不修改原始 Scene。首版调用线程内同步执行，不承诺多线程调用 Renderer 安全。

首版采用有界逐样本渲染、读回、累计并释放：不一次持有 N 个 FrameResult 占满帧槽。配置 maxSamples、maxPixels、maxWorkingBytes，执行前检查算术溢出和预算；Busy 时停止提交并给出可重试状态，不能等待调用者手中永不释放的结果。中途 GPU 已提交的资源仍须按既有完成信号回收。

曝光使用专属 View，与交互显示 View 隔离；每个样本禁用跨时刻 TAA 历史复用。不改变现有显示相机的历史基线或帧槽奇偶。上传暂存回收必须通过 S2-00 压力检查，不把 N 次快照上传的泄漏推迟到性能优化。

## GPU 扩展位置

CPU 首版只保证输出语义可表达，不承诺现有 CPU 描述已经足以实现 GPU。后续 GPU 执行器应显式声明图像/缓冲读写和结果导出，通过帧图编译同步；不能在 CPU SensorStage 里直接访问 Vulkan 句柄。单队列足以起步，多队列和显存别名仍为独立优化。
