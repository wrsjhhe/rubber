# 技术方案

功能编号见 [需求](FEATURES.md)，实现状态见 [进度](PROGRESS.md)。以下区分实施约束与当前过渡方案。

## 数据与底层约束

C++20、Vulkan 1.3、动态渲染；当前需要至少五个颜色附件及带计算能力的图形队列。先保持单队列，优化需实测支撑。

世界采用米制、右手系、Y 向上、相机朝局部 −Z；图像左上为原点，硬件深度 [0,1]。内参 fx/fy/cx/cy 按像素表示。

| 通道 | 格式和语义 |
|---|---|
| Color | RGBA8 UNORM，字节已编码 sRGB |
| AlbedoMetal | RGBA8，线性底色 RGB/金属度 A |
| NormalRough | RGBA16F，世界法线 XYZ/粗糙度 W |
| Motion | RGBA16F，XY=旧 UV−当前 UV（无抖动），Z=旧相机深度，W=旧实例匹配 |
| MetricDepth | R32F，光轴正距离，非欧氏距离；背景为零 |
| InstanceId | R32_UINT，稳定非零标签，背景零；禁止线性插值。点云占用最高位（见下） |
| HardwareDepth | D32F，不能当米制深度 |
| Hdr | RGBA16F，光照结果，未经时序解析 |
| Resolved | RGBA16F，时序解析后的线性颜色（TAA 之后的辐亮度），未经曝光与色调映射；A 分量是该帧米制深度，不表示透明度 |
| Debug | RGBA8，调试呈现图，由独立的 presentation 生成；背景为零，内容由 `debugChannel` 决定 |

OutputInfo 给出尺寸、格式、行字节数。新增点云/高斯输出须显式声明深度和覆盖语义。`Hdr` 与 `Resolved` 的区别是硬约束：需要线性图像的下游（例如传感器曝光与噪声模型）必须取 `Resolved`，取 `Hdr` 会跳过 TAA 累积；两者的 alpha 语义也不同（`Hdr` 是覆盖率，`Resolved` 是米制深度）。通道语义表 `ChannelSemantic`（`include/sim/Observation.hpp`，实现在 `src/sensors/Observation.cpp`）把 channel、名字、格式、单位、定义、背景值、是否最近点采样、是否真值一并固定下来；真值通道只描述场景本身，不随曝光、抖动或调试呈现改变。

## 渲染与时序

目标路径为 Shadow → Geometry → Lighting → TemporalResolve → ColorTransform，调试呈现是与 ColorTransform 并列的旁路而不是它之后的阶段。当前已按此拆分：`geometry` 写五个 G-buffer 与硬件深度，`shadow` 写 CSM 深度图，`lighting` 写 HDR，`temporal` 读 HDR/速度/米制深度与旧历史、写本帧历史槽并输出线性颜色+深度，`color` 做曝光、色调映射与 sRGB 编码，`presentation` 读 G-buffer（以及级联视图需要的 HDR）写独立的调试图，`readback` 复制真值，历史失效时另有 `history.clear`。GGX 金属粗糙度模型；底色纹理解码 sRGB，法线/金属粗糙度按线性数据。四级 CSM 使用 texel 对齐和比较滤波，阴影剔除按光源视锥。

颜色允许抖动，几何真值按固定像素中心采样；当前必要时额外绘制稳定几何 pass。TAA 静止积累 16 个 Halton 样本，运动采用速度重投影和深度拒绝。静止冻结只适用于所有贡献均不随时间变化的场景，雨雪等必须声明动态失效。

历史有效性必须包含相机、分辨率、场景/材质/效果版本、时间跳转和 reset；同 ID 更换资产不能继承错误运动历史。

## 共享、同步与结果

资产在构造后不可变，修改内容生成新对象：顶点、索引与包围盒在构造时一次算定，全部为只读成员（测试用 `static_assert` 锁住，改回可变即编译失败）。快照按值保存材质，并持有几何与贴图的固定版本；缓存键用资产身份（几何、贴图，永不因地址复用而冲突）和材质内容，贴图再区分色彩空间。RenderScene 共享同快照 GPU 数据与上传依赖；每相机旧姿态和历史保留在 View。上传完成进入提交依赖，不能每个相机重新同步上传。

当前实现：RenderScene 位于 src/render，由 Renderer 按快照内容键复用并核对内容，视图只写上一帧变换与匹配标记。首次出现的资产在同一上传批内提交，设备时间线信号量给出完成值，帧提交等待该值。上传批在每次准备时回收已完成的批次（当前代际与旧代际都回收），未完成的保留。缓存代际只按引用回收，切换代际后旧快照仍能按自己持有的固定资产重新上传。场景准备仍在调用线程录制。

结果拥有导出资源和完成信号。GPU 完成不等于消费者释放，帧槽不得提前复用。当前每 View 三份结果，满时 Busy；不得等待调用线程自己持有的结果而死锁。当前导出采用复制，同 View 串行复用，提交/缓存接口需调用方串行使用。

VulkanOutput fence 仅代表生产完成。GPU 消费者须保活 FrameResult，跨队列消费先等待，不修改布局/内容，也不能销毁库拥有的句柄；异步消费结束后才释放结果。

## 帧图与扩展

资源声明与 Vulkan 分配分离。描述纹理 mip/layer/aspect、缓冲字节范围和访问类型；跟踪 RAW/WAR/WAW，诊断未初始化读取、循环依赖、冲突声明。持久资源导入声明初始状态，导出声明最终状态和所有权。

从请求输出、必要历史写回和副作用反向裁剪；不能误裁 GPU 输出。编译执行顺序、屏障及首末使用点。首版单队列、保守分配，别名复用和多队列后续优化。仅包装固定调用不能算完成帧图。

当前实现：`src/render/graph/FrameGraph.hpp` 的声明层用 `Layout`/`Access`/`ImageUsage`/`BufferUsage` 描述意图，pass 体内不出现 `VkImageLayout`/`VkAccessFlags`；`Subresource{mip,mipCount,layer,layerCount}` 与 `BufferRange{offset,size}` 让依赖按子资源求交。跟踪粒度是逐区间的：图像按 (mip, layer) 展开成精确 cell 集（多 layer 的子范围不做首末压平），缓冲按字节段切分并只覆盖声明过的区间，每段各自记录写入者、读取者与是否已定义，因此"写了一部分就读全部"会在编译期被拒绝。一次写入会收集所有相交子资源的旧写入者与读取者，建立完整 WAW/WAR 边。声明期与执行期的缓冲状态是两张表：声明期的段可以为了压缩依赖而合并相邻同状态区间，`plan()` 则在导入声明上独立建一张覆盖 `[0,size)` 的区间表，按所有活跃访问的字节边界预切分后重放，因此一次局部写入不会改写未访问字节的已定义状态。`readWrite` 是"读改写同一资源"的独立声明：它既消费先前内容（要求已定义、保留对上一个写入者的依赖、参与 RAW 边），又不构成同一 pass 的反馈回路。附件读改写需要同时具备读与写访问——以 LOAD 进入的附件与混合操作都会读取旧值，屏障的目标访问因此是 `COLOR_ATTACHMENT_READ|COLOR_ATTACHMENT_WRITE`（深度读改写本已隐含读）；只声明写会让依赖图有边而屏障没把先前写入变成可见。`compile()` 依次做校验、反向裁剪（`exportImage/exportBuffer` 与 `sideEffect` 为根）、拓扑排序、屏障规划；`execute()` 才翻译成 `vkCmdPipelineBarrier` 并回写物理布局。导入声明初始布局与是否已初始化，导出声明终态布局并作为裁剪根，`restoreImage` 只回写布局、不作根，供跨帧复用的历史槽使用；导出与恢复只转换本帧被访问过的子资源，整图消费者需自行声明整图范围。`Renderer.cpp` 每帧重建声明，再按 `transients()` 分配或复用瞬时资源。历史状态的推进判据是“时序解析真的执行了”，不是“本次需要颜色”：`executed(PassHandle)` 返回编译后的裁剪结果，`resolved = color && executed(temporal)` 为真时视图才推进槽奇偶、`lastVP`、`stillSamples` 与 `stationary`，模块也只在同一条件下提交颜色历史基线。仅导出 Hdr 的请求没有 `color` 的导出根，未启用 TAA 时也没有 `sideEffect` 根，时序解析会被裁掉，此时颜色历史仍是上一份内容，任何提前推进都会让下一帧误判静止。有效 pass 清单经 `View::activePasses()` 对外可见，`tests/pass_check.cpp` 按输出通道逐项断言，`tests/graph_check.cpp` 在无设备条件下覆盖裁剪、诊断与区间跟踪。

模块负责资产准备、类型批次、可见性、资源/pass 注册、输出语义和历史失效。点云有自己的数据和绘制实现，公共调度只调用协议。

## 点云语义

点云是独立类型，不伪装成三角网格：`BufferPointCloud` 与几何一样在构造时算定点数据、包围盒与最大半径（包围盒按最大半径外扩，用于剔除），`SceneSnapshot::points` 与 `meshes` 并列，节点共用父子变换与稳定 ID。

- 绘制：每点是一个正对相机的圆盘（6 顶点实例化公告板）。位置在准备阶段一次变换到世界空间，同一份数据被多个节点引用时按节点展开；半径以世界单位给出，着色器按 `radius * fx / 视深`、`radius * fy / 视深` 分别换算两个轴的像素半径，最小 0.5 像素，因此不同分辨率与距离下保持物理尺寸。焦距来自**纯投影矩阵**（`Globals::lens = {|P00|*width/2, |P11|*height/2}`），不能取 `vp` 的元素：`vp` 含视图旋转，俯仰或翻滚会改变该元素，半径随之漂移，90 度翻滚还会退化到最小像素；两轴分开换算也让 `fx != fy` 的标定相机得到椭圆足迹。
- 强度与半径：强度乘在线性颜色上作为辐亮度，按覆盖度预乘 alpha 混合到已光照的 HDR；半径只决定覆盖面积，两者互不影响。
- 遮挡：点云 pass 以保留模式进入渲染，对硬件深度做 `LESS_OR_EQUAL` 测试并回写，因此点与网格在同一张深度图上互相遮挡，顺序只由深度决定，不依赖绘制次序。
- 深度与 ID：点写 `MetricDepth` 用与网格相同的公式（`1/gl_FragCoord.w`，光轴正距离）；`InstanceId` 的最高位置 1、低 31 位是点云节点的稳定 ID，因此消费方能在同一通道上区分网格与点云且标签不冲突；`Motion` 写零表示"无历史匹配"，运动分支不会复用旧像素，`AlbedoMetal`/`NormalRough` 与网格同义（法线朝向相机、粗糙度 1）。
- 历史失效：静止冻结（`info.w >= 0`）按像素平均历史，不看运动匹配，因此 `Motion` 写零不足以表达"点云变了"。每个 `PointsView` 保存上次真正写入颜色历史时的点云身份与点数（共享缓冲版本号，零表示无点云），一旦移动、替换、增删、改强度或半径即返回 `unchanged() == false`，相机与网格都没变也不会进入静止分支。这份基线是两段式的：`declare()` 只记下本帧待提交的身份，`commitHistory(true)` 才把它落库，因此几何真值交付、以及被裁掉时序解析的仅 Hdr 请求都不会把基线带到未写入的内容上。网格姿态基准（`lastModels`）走同一套提交规则——原先它按 `res.color` 推进，在时序解析被裁的帧上会让下一颜色帧把新姿态误判为"未变"而冻结旧画面。`View::stationary()` 暴露上一帧的静止判定，只在真正写入历史的帧上为真。
- 已知边界：不参与阴影投射与 CPU 剔除，没有逐点运动向量；大规模点云按节点整体驻留一份存储缓冲，受 `maxStorageBufferRange` 限制。存储缓冲以 `StorageRead` 登记进帧图（本帧读取有显式声明与屏障），但上传发生在图外，完成时序仍由时间线信号量在提交时声明；上传批的暂存副本随点云驻留，大点云尚未主动回收。

模块协议（`src/render/Module.hpp`）：`RenderModule` 每 Renderer 一份，负责跨相机共享的准备与缓存，`prepare()` 把共享数据交给 `FrameLifetime` 保活并登记上传时间线值；`ModuleView` 每相机一份，只保存与该相机有关的状态，`declare()` 声明本模块 pass 并记录待提交的历史状态，`declareLate()` 在时序解析与颜色变换之后补声明几何真值写回，`commitHistory(bool resolved)` 在时序解析确认执行并成功提交后才落库历史基线（默认空实现，模块按需覆写）。`FrameResources` 按语义交出帧级共享资源（G-buffer、深度、HDR、显示图、历史槽、常量缓冲），模块不接触视图私有字段；`FrameStats` 由模块写入、调度侧只汇总。设备因此要求 `shaderDemoteToHelperInvocation`（点云用 `discard` 裁圆盘）、`independentBlend`（同一管线 6 个附件只有 HDR 混合）与 `maxColorAttachments >= 6`。

## 观测与标定

观测不是"另存一份像素"，而是把已提交的结果和它所属的那次采样绑在一起：`Renderer::observe()` 与 `submit()` 走同一条提交路径，额外返回 `SensorCalibration`、`SampleTime` 与 `std::vector<OutputInfo>`，构造 `Observation` 时校验 `sampleTime.time == FrameResult::time()`，失败即抛 "Sample time does not match the frame"。这样"结果属于哪一刻"由类型保证，而不是靠调用方记得对齐。

- 标定与出图同源：`SensorCalibration::of(camera, width, height)` 在有内参时取 `camera.fx/fy/cx/cy`，否则由垂直视场角按方形像素换算；`projection()` 复用与 `PerspectiveCamera::projection` 完全相同的构造路径——`glm::perspectiveRH_ZO` 给出 P00/P11 后乘以 y 轴翻转，再把内参重写入 P00/P11/P20/P21（`2fx/w`、`-2fy/h`、`1-2cx/w`、`1-2cy/h`）。`Globals::lens` 里的焦距也取自同一组参数。三者同源是"标定可用于反投影"的前提；写成两份就会随改动漂移。
- 采样时间是元数据，不是积分器：`SampleTime{time, duration}` 的 `time` 必须等于快照时间，`duration` 是曝光时长（0 表示瞬时），`end() = time + duration` 供下游描述曝光区间。当前渲染器按瞬时采样出图，曝光积分属 F12，因此 `duration` 只影响观测元数据，不影响像素。
- 观测只读且不可覆盖真值：`read(Channel)` 与 `FrameResult::read` 同语义，未请求的通道显式报错而不补零；`semantic(Channel)` 给出该通道的单位、定义与背景值，供下游判断自己拿到的是什么。

## 呈现隔离与处理链

调试通道原来是 `lighting.frag` 里的分支，写回同一张颜色图。这样做有两个后果：一是调试帧的 Beauty 就被覆盖了，同一帧不可能既看到颜色又看到诊断；二是 HDR 与 TAA 历史会跟着调试内容走。M5 把这条分支整体删除：

- `lighting.frag` 只输出 Beauty，`color.frag` 无条件做曝光+ACES+sRGB 编码，因此 `Hdr`/`Resolved`/`Color` 三条输出与是否选了调试通道无关。
- `presentation.frag` 读 G-buffer（`normalTex`、`metricTex`、`idTex`）与 HDR（级联分区要看光照结果才能画分区色），按 `frame.settings.w` 选通道，写自己的 `debug` 图；背景为 0，最后统一做 sRGB 编码。
- `recordPresentation()` 只提交一个全屏三角形，`View::debugView()` 与 `Channel::Debug` 暴露的是同一张图。窗口要显示诊断就采这份图，而不是把调试写进 Beauty。
- 帧图侧有两个必须写对的细节。其一，**声明必须覆盖描述符可访问的资源**：`debugSet` 在创建时就绑定了 HDR，即使当前调试通道不读它，着色器也可以读到，因此 `presentation` pass 总是声明 `read(hdrImage)`——否则"只请求 Debug"的帧会把光照裁掉、HDR 停在 UNDEFINED，验证层报 `VUID-vkCmdDraw-None-09600`。其二，`presentation` 不是导出根（`Color` 才是），在旧 `render()` 路径下没有导出根时会被裁掉，窗口采样的调试图从未被写入；所以选了调试通道就把该 pass 声明为 `sideEffect()` 根，保证这一帧必定产出调试图。这两条都不是"多写一行保险"，而是描述符绑定与裁剪根语义的直接推论。
- 处理链是"只能新增"的协议：`SensorStage::outputs()` 声明本阶段新增的派生输出，`SensorStage::process(DerivedSink&)` 只通过 `sink.input()` 读观测、`sink.write(name)` 取自己声明的输出，`write()` 对未声明的名字直接抛 "Stage wrote an undeclared output"。`ProcessingChain::run` 在执行任何阶段之前做全链校验：空名、零尺寸、`desc.name == Observation::semantic(channel).name`（会遮蔽输入通道）、全链重名，任一命中就整体拒绝，不留半成品输出。因此"处理器覆盖真值"在结构上不可能发生，不需要靠约定或代码评审守。`ProcessedObservation` 同时持有源观测和派生输出，调用方读派生数据时源结果仍然有效。
- 未实现的阶段（畸变、曝光积分、运动模糊、噪声）按同一协议接入，不改变上述约束；它们目前都是缺项，不列为已支持。

## 验证与性能

每阶段运行现有回归和相关 Vulkan validation。图像质量用数值参考和人工检查；性能记录设备/驱动、分辨率、场景、相机数、预热/样本数、CPU/GPU 时间、显存和读回设置。测试契约和失败路径，不复制实现细节作为断言。
