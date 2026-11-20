# 实现进度

## 最新状态（2026-11-20 / M4，待审核）

**M4 已实现，待审核。** 网格与点云拆成两个并列模块（`src/modules/mesh`、`src/modules/points`），协议在 `src/render/Module.hpp`：`RenderModule` 负责跨相机共享的准备、缓存与上传依赖，`ModuleView` 只保存与相机有关的状态并声明自己的 pass，`FrameLifetime` 保活本帧共享数据。主调度只遍历接口，没有网格/点云类型分支。点云以位置、线性颜色、强度与半径（世界单位）加入场景，准备阶段一次变换到世界空间并合成一份共享存储缓冲；绘制用实例化公告板（6 顶点/点），半径按 `focal / 分辨率 / 视深` 换算成像素，因此同一份数据在不同分辨率与距离下保持物理尺寸，强度直接缩放辐亮度。点云 pass 对硬件深度做 `LESS_OR_EQUAL` 测试并回写，同时写 HDR、五个 G-buffer、米制深度与实例标签，于是点与网格双向遮挡，且颜色与几何真值属于同一物体。

本轮验证：Release 构建通过；七项 CTest 全通过（新增 `points_check`）；六项 `--validation` 各 0 条校验错误；`gallery --capture m4-capture.png --validation` 退出 0、0 条，与 `example/preview/courtyard.png` 差异仍为 37472/4096000 字节（0.915%）、`max_delta=1`；`temporal_check` 静止 raw=28.623 / TAA=6.28194、运动轮廓 347.217/293.813 逐位不变。

本轮同时修掉两个只在点云路径上暴露的缺陷：`beginPass()` 的颜色附件描述数组原来按 5 个静态分配，而颜色链路的点云 pass 需要六个（五个 G-buffer 加 HDR），越界写栈并触发 `0xC0000409`；顶点着色器又用 `frame.vp[1][1]` 换算像素半径，而 Vulkan 的 y 向下裁剪空间让该分量为负（实测 −2.246），半径被钳到最小 0.5 像素、点云退化成单像素。前者改为按 `maxColorAttachments` 分配并在超限时抛错，后者取绝对值；`points_check` 的半径覆盖与分辨率比例断言覆盖第二条。

## 最新独立验收（2026-11-20 / 715233d）

**M3 已验收，可进入 M4。** 声明期与执行期缓冲区间表已隔离，R-M3-001 补充关闭，M3 三项审核问题全部关闭。独立 Release、六项 CTest、五项 --validation 和 Gallery smoke 通过；TAA 数值不变。详见 [最终复审](REVIEW_M3.md)。本节取代下方历史待审核/需修改状态。性能优化、区间合并优化、多队列与显存别名不在本次验收范围。

## 最新状态（2026-11-20 / M3 复审补充修复 715233d）

**R-M3-001 补充项已修复，待复审。** 根因是执行期重放复用了声明期为压缩依赖而合并的缓冲段：`plan()` 把 `defined`/`stage`/`access`/`lastWrite` 写到整段上，一次局部写入就把未访问字节标成已定义。修复把两者彻底分开——声明期 `segments` 只服务裁剪与排序；执行期在 `plan()` 内新建一张覆盖 `[0,size)` 的区间表，初始状态完全由导入声明推导，并按所有活跃访问的 `pieces` 边界预切分，于是每次访问恰好等于若干整段，状态更新精确到被访问的字节；导出收尾也改读这张表。`graph_check` 新增两条用例（局部写 + 读另一半 + 整体覆写必须报未初始化；完整写后分段以不同访问读、再整体覆写时每段屏障源状态各自正确），并用 083f8fe 的真实实现验证两条都会失败。Release 构建、六项 CTest、五项 `--validation`、Gallery 捕获差异（0.915%，`max_delta=1`）、`temporal_check` 数值均与首轮修复逐位一致。详见 [M3 审核回应](REVIEW_M3_RESPONSE.md)。

## 最新独立复审（2026-11-20 / 083f8fe）

M3 仍需修改：R-M3-002/003 修复通过代码复核；R-M3-001 的声明期区间合并仍会在 plan 重放时把局部访问状态扩散到整段，漏报未初始化读取。详见 [复审报告](REVIEW_M3.md)。独立 Release、六项 CTest、五项 --validation 和 Gallery smoke 通过；现有用例未覆盖该组合。本节为 083f8fe 的审核结论，补充项已在上一节修复。

## 最新状态（2026-11-20 / 083f8fe）

**M3 三项阻塞问题已修复，待复审。** 帧图的通用契约从"每个资源一个状态"改为逐个声明区间跟踪：缓冲按字节段跟踪写入者、读取者与是否已定义；图像子资源按 layer × mip 精确展开，不再把多 layer 的 mip 范围压平；写入依赖收集全部相交子资源的写入者与读取者，WAW/WAR 边不再丢失。新增 10 个 `graph_check` 用例，并用缺陷注入逐项确认它们在旧实现或注回缺陷后失败。Release 构建、六项 CTest、五项 `--validation`、Gallery 捕获与参考图差异（0.915%，`max_delta=1`）、`temporal_check` 数值均与上一版一致。详见 [M3 审核回应](REVIEW_M3_RESPONSE.md)。下方独立审核记录的"需修改"结论对应修复前的 996a779。

## 最新独立审核（2026-11-20 / 996a779）

**M3 需修改，尚未验收。** 独立 Release 构建、六项 CTest、五项 --validation 和 Gallery smoke 通过；但通用帧图存在缓冲区间/初始化追踪缺失、多 layer mip 范围展开错误、多子资源 WAW/WAR 边丢失三项阻塞问题。详见 [M3 审核报告](REVIEW_M3.md)。本结论优先于下方开发者待审核记录；M2 既有验收结论不变。

## 最新状态（2026-11-20 / 996a779）

**M3 已实现，待审核。** 帧图（`src/render/graph/`）把资源声明与 Vulkan 分配/布局彻底分开：pass 只声明"读/写哪个资源的哪个子资源，用哪类访问"，编译期完成反向裁剪、拓扑排序、RAW/WAR/WAW 依赖推导和屏障生成，执行期才翻译成 `vkCmdPipelineBarrier`。渲染拆成历史清除、阴影、几何、光照、时序解析、颜色变换、读回七类可独立裁剪的 pass，`shaders/resolve.frag` 拆为 `temporal.frag` + `color.frag`。

本轮验证：Release 构建通过；六项 CTest 全通过（新增 graph_check、pass_check）；五项 `--validation` 各 0 条校验错误（含 gallery）；`gallery --capture` 与 `example/preview/courtyard.png` 差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`，仅 ±1 LSB 舍入；`temporal_check` 静止误差 raw=28.623 / TAA=6.28194 逐位不变，运动轮廓 347.217 / 293.813（上一轮 293.259，差异来自 temporal 与 color 拆成两个 pass 的浮点顺序）。缺项见下方 M3 交接记录，不计为已验收。

## 最新验收结论（2026-11-20 / 278371f）

**M2 已验收，可进入 M3。** R-M2-001 包围盒可写入口已封闭，四项审核问题全部关闭。独立 Release 构建、四项 CTest、四项 --validation、Gallery smoke 通过；Gallery 捕获与仓库参考图 SHA256 一致。详见 [最终复审](REVIEW_M2.md)。下方旧的“需修改/待审核”记录仅保留为历史，本节为当前状态。上传限流、同代际驱逐、性能和跨设备测试不在本次验收结论内。

## 最新独立复审（2026-11-20 / 8c35d79）

Release、四项 CTest、asset_check --validation、Gallery smoke 独立通过。R-M2-002/003/004 修复通过复核；R-M2-001 的材质/贴图隔离已修复，但公开可变的几何包围盒仍能改变旧快照的剔除结果，M2 暂保持需修改。详见 [复审记录](REVIEW_M2.md)。本记录优先于下方历史审核状态。

复审追加项已修复：包围盒改为与顶点同时算定的 const 成员，几何的五个渲染输入全部只读，并以 `static_assert` 锁住该约定（缺陷注入可使其编译失败）。状态**待二次复审**，见下方"复审追加修复"交接记录。

## 审核与修复（2026-11-20）

审核版本 dev / bdfab25，结论：**需修改，M2 尚未验收**。独立 Release 构建、四项 CTest、四项 --validation 和 Gallery smoke 均通过，但发现版本隔离、TAA 失效、当前代际上传回收和缓存地址复用四项阻塞问题。详见 [M2 审核报告](REVIEW_M2.md)。

修复版本：dev 分支"M2 修复"提交，状态**待复审**。四项问题的处理与对应测试见 [审核回应](REVIEW_M2_RESPONSE.md)；根因是资产"版本"由可变的 `revision` 与裸地址表达，修复后几何与贴图在构造后不可变、材质按值进入快照、缓存按不可复用的资产身份索引。下方开发者交接记录保留为历史。

基线核对日期：2026-11-20。M2 在 dev 分支完成实现并修复审核问题，本轮执行了 Release 构建、四项 CTest、验证层运行和画廊捕获；结论仍为待复审，不等同独立审核。

状态：待开发 → 开发中 → 待审核 → 已验收。部分实现必须列缺项，不用百分比代替证据；“已验收”需独立审核结论。

## 代码事实

| 功能 | 状态与位置 | 缺项/限制 |
|---|---|---|
| F01/F02 | 已有实现，待独立审核；Scene.hpp、src/scene、src/assets/Gltf.cpp | 静态不透明导入子集；资产版本与共享场景见下方“F02 缓存拆分”行 |
| F03/F04 | 已有实现，待审核；Renderer.cpp、Pipeline.cpp、shaders、render/passes、temporal_check | 帧图驱动的七类 pass；动态效果失效协议未实现；光照仍单次直射+点光，无阴影滤波选择 |
| F05/F06 | 已有实现，待审核；Output.hpp、Renderer.hpp、VulkanInterop.hpp、request_check | 三份结果上限，导出复制，同 View 串行；旧 API 仍为最近帧 |
| F05 几何路径 | 已有实现，待审核；submit/renderFrame | 跳过光照/TAA，新 View 不建颜色历史；仍写五个 G-buffer，用过颜色的 View 保留颜色资源 |
| F02 缓存拆分 | 已验收（278371f）；render/RenderScene.hpp、render/RenderScene.cpp、assets/AssetCache.hpp、backend/Device.hpp、tests/asset_check.cpp | 场景准备仍在调用线程录制；实例缓冲按快照版本重新分配，未跨版本复用；上传批未限流 |
| F07 帧图 | 已有实现，待审核；render/graph/FrameGraph.hpp、FrameGraph.cpp、tests/graph_check.cpp | 单队列顺序执行；保守屏障（每 pass 保守 stage/access，不做合并优化）；瞬时资源按描述缓存复用，未做显存别名 |
| F08 模块与点云 | 已有实现，待审核；render/Module.hpp、modules/mesh、modules/points、shaders/points.vert、shaders/points.frag、render/passes/PointsPass.cpp、tests/points_check.cpp | 点云不在 CPU 侧剔除，也不参与阴影投射；无逐点历史，运动向量写零（时序解析不复用旧像素）；点云仍需按节点逐点展开到一份共享缓冲，总量受 `maxStorageBufferRange` 限制；gallery 示例场景尚未加入点云 |
| F09 | 部分实现 | 已有内参、时间、真值输出和互操作头；无独立 Observation/sensors/presentation |
| F10 | 待开发 | 无 install/export/find_package 验证 |
| F11—F17 | 未实现 | 后续规划，不列为已支持 |

GPU 遮挡剔除已移除，保留 CPU 视锥剔除。

## 可交接任务

| 任务 | 依赖/状态 | 范围 | 验收证据 |
|---|---|---|---|
| M1 | 已有实现，待审核 | F05/F06 请求、结果、槽位、互操作 | 双相机、旧结果不变、Busy/恢复、销毁 View/Renderer 后读取、非法请求及 GPU 寿命 |
| M2 | M1；已验收（278371f） | RenderScene、共享静态实例数据、不可变资产版本、上传依赖、延迟回收 | asset_check：同快照两相机准备与上传各一次、旧快照在原对象被修改后与清缓存后仍重现原画面、身份不复用、静止收敛后换贴图的 Color 更新、上传批与暂存回落、旧代际最终回收、渲染输入只读的编译期断言 |
| M3 | M2；已验收（715233d） | 帧图及六类独立 pass，资源声明与分配分离 | graph_check：裁剪、未初始化读取（图像与缓冲逐区间，含"局部写+整体覆写"合并路径）、同 pass 读写冲突、循环依赖、子资源精确展开、缓冲分段导出与生产者、跨子资源/跨区间 RAW/WAR/WAW、分段读后覆写的每段屏障源状态、usage 诊断、导出终态；pass_check：按输出通道断言活跃 pass 清单（含首帧历史清除与状态变化后的重建）；六项 CTest 与五项 --validation |
| M4 | M3；已有实现，待审核 | 网格模块迁移、模块协议、点云 | points_check：同一快照两台相机下点在前/网格在前的遮挡互换、米制深度与实例标签取值、仅点云场景、点云 pass 相对网格 pass 的插入位置、强度→亮度与半径→覆盖互不影响、覆盖面积随分辨率平方增长；七项 CTest 与六项 --validation |
| M5 | M4；待开发 | Observation、处理链入口、呈现隔离、互操作边界 | Beauty+调试同帧不串色；校准/时间一致；真值独立；无需实现全部 F11—F13 即可验收框架 |
| M6 | M5；待开发 | 安装头文件/库/shader、导出 target/依赖 | 新安装前缀的外部项目 find_package 构建并离屏运行，不访问原源码/构建目录；关闭 example 可构建 |

每项保留现有回归。F11—F17 分别立项，先补规格和质量/性能门槛。

## 已有验证与缺口

本轮（M4）：Release 构建成功；sensor_check、temporal_check、request_check、asset_check、pass_check、points_check、graph_check 七项 CTest 全通过；sensor/temporal/request/asset/pass/points 六项分别以 `--validation` 运行，均 0 条校验错误；`gallery --capture m4-capture.png --validation` 退出 0、0 条；捕获图与 example/preview/courtyard.png 差异仍为 37472/4096000 字节（0.915%），`max_delta=1`；temporal_check 数值与 M3 逐位相同。点云路径此前完全无测试覆盖，本轮首次执行该路径即暴露 `beginPass()` 附件数组越界与负号半径两处缺陷，已修复并由 points_check 的覆盖面积断言守住。

本轮（M3 修复）：Release 构建成功；六项 CTest 全通过；sensor/temporal/request/asset/pass 五项 `--validation` 均 0 条校验错误；gallery `--capture --validation` 退出 0、0 条；捕获图与 example/preview/courtyard.png 差异仍为 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`；temporal_check 数值与修复前逐位相同。graph_check 新增 10 个用例，并用旧版源码与四处缺陷注入分别确认它们会失败。

本轮（M3）：Release 构建成功；sensor_check、temporal_check、request_check、asset_check、pass_check、graph_check 六项 CTest 全通过；sensor/temporal/request/asset/pass 五项分别以 `--validation` 运行，均 0 条校验错误；gallery `--smoke --validation` 与 `--capture --validation` 退出 0、0 条校验错误；`gallery --capture` 与 example/preview/courtyard.png 差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`；temporal_check 静止误差 raw=28.623、TAA=6.28194 与 M2 逐位相同，运动轮廓 347.217/293.813（M2 为 293.259，源于 temporal 与 color 拆成两个 pass 的浮点累加顺序）。graph_check 不需要设备，可在无 GPU 机器上跑裁剪与诊断。

上一批：Release 构建成功；sensor_check、temporal_check、request_check 三项 CTest 通过；request_check --validation 和 gallery --smoke --validation 通过。参考环境 Windows、RTX 3080 Ti；精确驱动和完整日志尚未归档，不宣称跨设备验证。

本轮（M2 修复 + 复审追加）：Release 构建成功，构建新增显式 `/EHsc`，此前 asset_check 的 C4530 警告消失；sensor_check、temporal_check、request_check、asset_check 四项 CTest 通过；四项分别以 --validation 运行完成；gallery --smoke --validation 通过；gallery --capture 输出仍与 example/preview/courtyard.png 字节一致（MD5 356ab30b5a9ba71818e38eec7babd280），temporal_check 误差数值与改动前相同（raw=28.623、TAA=6.28194、移动轮廓 347.217/293.259），说明资产模型重构与包围盒只读化未改变画面。参考环境同上一批，驱动版本仍未归档。

缺陷注入核对：把 TAA 历史比较临时放宽为只比几何与变换（去掉材质与贴图身份）后，asset_check 在"Texture swap kept frozen TAA history"处失败；把几何包围盒改回可变成员后，asset_check 以 `static_assert failed: 'Bounds are mutable'`（C2338）编译失败。两处用例因此确认有效，均已还原。

尚缺显存分配自动统计证据、多队列/异步计算路径、显存别名复用，以及安装测试。点云已由 points_check 覆盖单点数据、双相机遮挡、仅点云场景、两种分辨率与强度/半径语义，尚未覆盖大规模点云、逐点运动与阴影投射。活跃 pass 清单已由 pass_check 按输出通道逐项断言，但只覆盖本仓库现存的通道组合，未覆盖全部请求×交付×选项组合。清缓存安全性由 asset_check 的在途结果覆盖，尚未覆盖多视图同时提交的压力场景。引擎未安装 debug messenger，验证层输出只经层默认渠道，未见报错但可见性有限。

## 交接记录

### M4 / F08 模块协议与点云

任务编号 / 功能编号：M4 / F08，并落实 F07 的读改写声明
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待审核
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支 M4 提交「M4：模块协议、网格模块迁移与点云模块」
修改文件 / 接口变化：
- `src/render/Module.hpp`（新增）：`FrameImage`/`FrameBuffer`/`FrameStats`/`FrameResources`/`FrameLifetime`/`ModuleViewDesc`/`ModuleView`/`AssetProvider`/`RenderModule`。模块只拿帧级共享资源与自己的视图状态，不接触 `Renderer::Impl` 私有字段
- `src/render/GraphBridge.hpp`（新增）：声明层语义到 Vulkan 格式/usage/布局的换算，从 `Renderer.cpp` 抽出
- `src/modules/mesh/MeshModule.{hpp,cpp}`（新增）：网格模块。按 `sceneKey(快照, 资产代际)` 与内容哈希复用 `RenderScene`，视图侧持有阴影图、上一帧姿态缓冲与光照描述符集合，声明 `shadow`/`geometry`/`lighting`/`geometry.stable`
- `src/modules/points/RenderPoints.{hpp,cpp}`、`src/modules/points/PointsModule.{hpp,cpp}`（新增）：共享点云缓冲（世界空间 `positionRadius`/`colorIntensity`/`label`，一次 `UploadBatch` 上传）与点云视图，声明 `points`/`points.stable`
- `include/sim/Scene.hpp`、`src/scene/Scene.cpp`：`PointVertex`、不可变 `BufferPointCloud`（构造时算定包围盒与最大半径，包围盒按最大半径外扩）、`PointObject`、`SceneSnapshot::points`、`Scene::addPoints`
- `src/render/graph/FrameGraph.{hpp,cpp}`：新增 `readWrite(ImageHandle, Access, Subresource)` 声明；`Use::readsBack` 使读改写既激活写者又要求先前内容已定义
- `shaders/points.vert`、`shaders/points.frag`（新增，片元编译 `POINTS_HDR=1/0` 两个变体）、`src/render/passes/PointsPass.cpp`（新增）、`src/render/Pipeline.{hpp,cpp}`（`GraphicsOptions`、`splat`/`splatDepth` 管线、`maxColorAttachments`、`beginPass(..., bool load)`）
- `src/backend/Device.cpp`：要求 `shaderDemoteToHelperInvocation`、`independentBlend` 与 `maxColorAttachments >= 6`
- `src/render/Renderer.cpp`、`include/sim/Renderer.hpp`：主调度改为遍历 `modules`；`FrameLifetime` 取代 `RenderScene` 作为 `FrameResult` 的保活凭据；新增 `View::visiblePoints()`
- `tests/points_check.cpp`（新增）、`CMakeLists.txt`：注册第七项 CTest 与点云片元变体
已完成：
- 主调度无点云分支：`Renderer::renderFrame` 只按接口调用 `prepare`/`declare`/`declareLate`/`unchanged`，网格与点云是同一条路径上的两个模块
- 点与网格共用一张硬件深度：点云 pass 走 `beginPass(..., load=true)` 保留网格结果，深度 `LESS_OR_EQUAL` 且回写，网格在前时挡住点、点在前时覆盖网格
- 强度与半径保留：半径以世界单位进入缓冲，着色器按 `abs(focal) * 0.5 / 视深 * 分辨率` 换算像素并取最小值 0.5 像素，强度乘到线性颜色上作为辐亮度按覆盖度预乘混合
- 深度/ID 语义：点写入 `MetricDepth`（`1/gl_FragCoord.w`，与网格同一公式）与 `InstanceId`（最高位置 1、低 31 位为点云节点 ID），`Motion` 写零表示无历史匹配，`AlbedoMetal`/`NormalRough` 与网格同义
仍缺项：
- 点云不做 CPU 剔除、不投射阴影；无逐点运动向量，移动的点云不会生成正确速度，只保证不被 TAA 复用旧像素
- 每个节点把整份点云逐点展开到共享缓冲，总量受 `maxStorageBufferRange` 限制，未做分批绘制
- gallery 示例场景未加入点云，视觉回归仍只覆盖网格路径
- 沿用上一轮：单队列顺序执行、屏障保守、瞬时资源无显存别名
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- `cmake --build build-dev --config Release`
- `ctest --test-dir build-dev -C Release --output-on-failure`
- `sensor_check|temporal_check|request_check|asset_check|pass_check|points_check --validation`
- `gallery --capture build-dev/m4-capture.png --validation`；`python tools/pngdiff.py …`
结果 / 日志截图位置：七项 CTest 全通过；六项 `--validation` 与 gallery 均 0 条校验错误；捕获图与参考图差异 37472/4096000（0.915%），`max_delta=1`；`temporal_check` raw=28.623、TAA=6.28194、移动 347.217/293.813 逐位不变
缺陷注入核对：`points_check` 首次运行时点云路径直接以 `0xC0000409` 崩溃——`beginPass()` 的 `std::array<VkRenderingAttachmentInfo, 5>` 被六个附件写出栈；修好后用例在"Point in front did not own the pixel"失败，进一步把顶点着色器的像素半径以深度通道输出，读到 `pixels=0.5`（钳位值）与 `vp[1][1]=-2.246`，确认半径被算成负数。两处修复后用例通过；把任一处改回（数组改回 5、去掉 `abs`）都会重新失败，故该用例对这两条缺陷有效
性能和画质对比：未做性能测量；网格路径画面与数值与 M3 逐位一致，点云只影响含点云的场景
已知问题 / 下一步：等待复审；通过后进入 M5（Observation、处理链入口、呈现隔离、互操作边界）
审核人 / 审核版本 / 结论 / 问题编号：待填

### M3 复审补充修复 / F07（回应 R-M3-001 补充）

任务编号 / 功能编号：M3 / F07，审核问题 R-M3-001（补充）
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待复审
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支 715233d「M3 修复：执行期缓冲状态与声明期段表分离（回应 R-M3-001 补充）」
修改文件 / 接口变化：
- `src/render/graph/FrameGraph.cpp`：`plan()` 不再复用 `BufferResource::segments` 重放执行期状态，改为本地新建覆盖 `[0,size)` 的执行期区间表，初始状态完全由导入声明推导，并按所有活跃 pass 的 `pieces` 边界预切分（`splitExec`）；缓冲访问与导出收尾都改读这张表
- `tests/graph_check.cpp`：新增两条用例——局部写 + 读另一半 + 整体覆写必须报未初始化；完整写后分段以三种访问方式读、再整体覆写时三条屏障各自的源状态与范围
- `docs/REVIEW_M3_RESPONSE.md`：追写「R-M3-001 补充」一节
已完成：
- 声明期与执行期状态彻底分开：声明期的段合并（为压缩裁剪/排序的依赖）不再影响执行期状态，局部访问不会把未访问字节标成已定义
- 执行期区间表按每次访问的边界预切分，状态更新精确到被访问的字节；导出屏障的范围取执行期真实边界，不跨帧内从未写过的空洞
仍缺项：
- 预切分使执行期区间数随"访问次数 × 边界"增长，尚未做区间合并或按 pass 复用的优化
- 其余沿用上一轮：单队列顺序执行、屏障保守、瞬时资源无显存别名、活跃 pass 断言只覆盖现有组合
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- `cmake --build build-dev --config Release`
- `ctest --test-dir build-dev -C Release --output-on-failure`
- `sensor_check|temporal_check|request_check|asset_check|pass_check --validation`
- `gallery --capture build-dev/m3r2-capture.png --validation`；`python tools/pngdiff.py …`
结果 / 日志截图位置：六项 CTest 全通过；五项 `--validation` 与 gallery 均 0 条校验错误；捕获图与参考图差异 37472/4096000（0.915%），`max_delta=1`、`mean_delta=1`；`temporal_check` raw=28.623、TAA=6.28194、移动 347.217/293.813 与首轮修复逐位相同
缺陷注入核对：用 083f8fe 的真实 `FrameGraph` 跑新用例——第一条在 `Expected a diagnostic containing: reads uninitialized buffer scratch` 失败，第二条在 `Merged range lost the per-segment source state of the overwrite barriers` 失败；另注回"不按访问边界预切分"后用例在 `Buffer export produced no barrier` 失败；均已还原（`grep 缺陷注入` 为 0）
性能和画质对比：未做性能测量；画面与数值与首轮修复一致
已知问题 / 下一步：等待复审；通过后进入 M4（网格模块迁移、协议与点云）
审核人 / 审核版本 / 结论 / 问题编号：待填

### M3 修复 / F07（回应 REVIEW_M3）

任务编号 / 功能编号：M3 / F07，审核问题 R-M3-001 至 R-M3-003
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待复审
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支 083f8fe「M3 修复：帧图的区间与子资源跟踪（回应 REVIEW_M3）」
修改文件 / 接口变化：
- `src/render/graph/FrameGraph.hpp`：`BufferResource` 的单一 `state` 与 `written` 并集改为 `segments`（有序、不重叠、只覆盖已声明字节的区间，每段带 writer/defined/readers/lastWrite）；新增 `Segment` 结构；`Cell` 增加 `used`（本帧是否被活跃 pass 访问）；`Use` 增加精确 `cells`/`pieces`，`writers`/`readers` 改为相交子资源的去重全集；`cellRange` 换成 `cellsOf`，新增 `segmentsOf`/`findSegment`/`mergeSegments`
- `src/render/graph/FrameGraph.cpp`：子资源按 layer × mip 双重展开；缓冲在声明边界切段并用导入状态填补空洞，依赖/屏障按段计算；`cull` 对导出缓冲逐段激活生产者；`order` 用全部写入者与读取者建 WAW/WAR 边并对重复边去重、跳过自环；`plan` 逐段判定未初始化读取并只对写过的段发导出屏障；导出/恢复只转换本帧 `used` 的子资源
- `tests/graph_check.cpp`：新增 10 个用例覆盖上述三点
- `docs/REVIEW_M3_RESPONSE.md`：逐条回应与缺陷注入证据
已完成：
- 缓冲按字节区间跟踪初始化与生产者；导出覆盖全部写过的段；未初始化读取逐区间拒绝；非重叠访问不误报冲突且屏障不外扩
- 多 layer 的 mip 子范围按精确 cell 集展开，导出不再顺带转换未声明的子资源
- 跨多个 mip/layer 或字节区间的写入收集全部旧写入者与读取者，WAW/WAR 边不再丢失
仍缺项：
- 单队列顺序执行、屏障保守、瞬时资源无显存别名（沿用上一轮）
- 段级跟踪使段数随声明范围增长，尚未合并优化；屏障未按段合并
- 图像导出只覆盖本帧访问过的子资源，整图消费者需自行声明整图范围
- 活跃 pass 清单断言仍只覆盖仓库现有通道 × 交付 × 选项组合
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- `cmake --build build-dev --config Release`
- `ctest --test-dir build-dev -C Release --output-on-failure`
- `sensor_check|temporal_check|request_check|asset_check|pass_check --validation`
- `gallery --capture build-dev/m3r-capture.png --validation`；`python tools/pngdiff.py …`
结果 / 日志截图位置：六项 CTest 全通过（graph_check 输出 "PASS: frame graph culling, diagnostics, subresources and import/export"）；五项 `--validation` 与 gallery 均 0 条校验错误；捕获图与参考图差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`；`temporal_check` raw=28.623、TAA=6.28194、移动 347.217/293.813 与上一版逐位相同
缺陷注入核对：旧版 `FrameGraph` 跑新用例在 `Multi-layer mip range touched extra subresources` 失败；注回"只保留首个写入者""丢失 WAR 边""导出只激活最后一段生产者""删除缓冲未初始化诊断"四处缺陷后，用例分别在 WAW、WAR、缓冲导出生产者、缓冲未初始化读取处失败；均已还原
性能和画质对比：未做性能测量；画面与数值与修复前一致，说明是跟踪语义的修正而非渲染变化
已知问题 / 下一步：等待复审；通过后进入 M4（网格模块迁移、协议与点云）
审核人 / 审核版本 / 结论 / 问题编号：审核者 / dev 083f8fe / 需修改 / R-M3-001 补充（声明期合并段被执行期复用），见上方补充修复轮

### M3 / F07 帧图与六类独立 pass

任务编号 / 功能编号：M3 / F07（帧图），关联 F03/F04
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待审核
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支 996a779「M3：帧图与六类独立 pass（资源声明与分配分离）」
修改文件 / 接口变化：
- 新增 `src/render/graph/FrameGraph.hpp`、`src/render/graph/FrameGraph.cpp`：声明层枚举 `Format/Aspect/Layout/Access/ImageUsage/BufferUsage`、`ImageHandle/BufferHandle/PassHandle`、`Subresource{mip,mipCount,layer,layerCount}`、`BufferRange{offset,size}`、`Barrier`、`CompiledPass`、`Transient`；`Declaration::read/write`（图像与缓冲各自重载）、`after()`、`sideEffect()`；`importImage/importBuffer`（声明初始布局与是否已初始化）、`createImage/createBuffer`（瞬时）、`exportImage/exportBuffer`（裁剪根，声明终态布局）、`restoreImage`（非裁剪根，仅回写布局）、`reserve/addPass/bind/compile/execute/reset`；观察接口 `activePasses()/passes()/finalBarriers()/transients()/barrierCount()/names()`
- 新增 `src/render/FrameData.hpp`：从 Renderer.cpp 抽出的 `Globals`（1072 字节，`static_assert` 锁布局）、`Draw`、`temporalSamples`、`halton`、`sameLights`、`corners`、`sceneKey`、`fitShadows`
- 新增 `src/render/passes/Passes.hpp` 与 `ShadowPass.cpp`、`GeometryPass.cpp`、`LightingPass.cpp`、`TemporalPass.cpp`、`ColorPass.cpp`、`ReadbackPass.cpp`：每个 pass 只录制绘制，不做隐式布局转换
- 新增 `shaders/temporal.frag`（原 `resolve.frag` 的 TAA 逻辑，输出 `vec4(color, depth)`）、`shaders/color.frag`（曝光、色调映射、sRGB 编码）；删除 `shaders/resolve.frag`
- `src/render/Pipeline.hpp/.cpp`：新增 `colorSet` 描述符布局、`colorLayout`、`color` 管线（`color.frag` → `colorFormat`）；`temporal` 管线改用 `temporal.frag`；`beginPass` 不再做隐式 transition
- `src/render/Renderer.cpp`：每帧 `reset` → `importImage/importBuffer`（由物理 `VkImageLayout` 换算成声明层 `Layout`）→ `bind` → `reserve/addPass` → 导出/恢复 → `compile` → 按 `transients()` 分配或复用 → `execute`；新增 `transientImage/transientBuffer/beginTransients/sweepTransients`；颜色相关资源仅在请求颜色时导入与绑定；图像分配用的 `VkImageUsageFlags` 统一由 `vkUsageOf(声明用法常量)` 推导，避免声明与分配各写一份而漂移
- `include/sim/Renderer.hpp`：`View::activePasses()`；`renderFrame(..., bool color, const std::vector<Channel>& exports)`
- 新增 `tests/graph_check.cpp`（不需要设备）、`tests/pass_check.cpp`、`tools/pngdiff.py`（纯 Python PNG 差异统计）
- `CMakeLists.txt`：shader 列表改为 `geometry.vert geometry.frag shadow.vert fullscreen.vert lighting.frag temporal.frag color.frag`；`renderer` 增加帧图与六个 pass 源文件；新增 `pass_check`、`graph_check` 两个 CTest
已完成：
- 声明与分配分离：pass 体内不出现 `VkImageLayout`/`VkAccessFlags`，布局与访问由帧图在编译期推导；`ensureColor()` 只申请内存，不再决定布局
- 反向裁剪：`exportImage/exportBuffer` 与 `sideEffect()` 是根，从请求输出反推执行集；GPU 交付的导出同样是根，不会被误裁（pass_check 断言）
- 诊断：未初始化读取、同一 pass 读写同一资源（反馈冲突）、写用只读访问、声明用法与访问不匹配、depth/color aspect 误用、循环依赖（报错信息列出参与循环的 pass 名单）
- 依赖：RAW/WAW 由最近的写入者串联，WAR 用"自上次写入以来的读取者"快照生成边并阻止调度器前移；按子资源 cell（mip × layer）与缓冲字节范围求交，不相关子资源不产生依赖
- 持久资源语义：导入声明初始布局与是否已初始化；导出声明终态布局并作为裁剪根；`restoreImage` 只回写布局、不作为根，供历史槽在帧末回到下一帧导入声明的布局
- 执行：编译出"每 pass 前置屏障 + 帧末导出/恢复屏障"，`execute` 翻译成 `vkCmdPipelineBarrier` 并回写物理布局；屏障数量可经 `barrierCount()` 检查
- 六类独立 pass：Shadow → Geometry → Lighting → TemporalResolve → ColorTransform，加 Readback 与历史清除（`history.clear`）；每个 pass 可用 `activePasses()` 单独可见
- 历史语义修正：只有真正写入历史的帧才推进历史状态与槽奇偶（`historyNumber`），几何真值与 GPU 交付请求不再让 TAA 历史失效；光照/阴影/通道变化仍按原协议作废历史
仍缺项：
- 单图形队列顺序执行，未做多队列或异步计算，屏障未按队列拆分
- 屏障保守：每个 pass 对声明过的资源各发一次屏障，子资源场景存在整图退化，未做屏障合并与去重优化
- 瞬时资源按描述缓存复用，未做显存别名（不同寿命资源共用同一块内存）
- 没有自动的显存占用/分配次数统计，只能从 `transients()` 列表外部检查
- 活跃 pass 清单的断言只覆盖仓库现有通道 × 交付 × 选项组合，未穷举
- 历史失效仍是一次整槽 `vkCmdClearColorImage`，未做按区域局部失效
- 场景准备仍在调用线程同步录制；上传批未限流（沿用 M2 缺项）
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- `cmake -S . -B build-dev`；`cmake --build build-dev --config Release`
- `ctest --test-dir build-dev -C Release --output-on-failure`
- `sensor_check|temporal_check|request_check|asset_check|pass_check --validation`
- `gallery --smoke --validation`；`gallery --capture build-dev/m3-capture.png --validation`
- `python tools/pngdiff.py build-dev/m3-capture.png example/preview/courtyard.png`
结果 / 日志截图位置：六项 CTest 全通过；五项 `--validation` 均 0 条校验错误；gallery 两种模式均 0 条；`pass_check` 输出 "PASS: frame graph culls passes to the requested outputs"；`graph_check` 输出裁剪与诊断断言全通过；捕获图与仓库预览图差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`
性能和画质对比：temporal_check 静止误差 raw=28.623、TAA=6.28194 与 M2 逐位相同；运动轮廓 347.217/293.813（M2 为 293.259），差异源于 temporal 与 color 由同一 pass 拆成两个 pass 后的浮点累加顺序，不是逻辑变化；未做性能测量，无帧时间对比
已知问题 / 下一步：等待审核；审核通过后进入 M4（网格模块迁移、协议与点云）。审核若要求性能数据，建议先补帧图屏障统计（`barrierCount()` 已有）与 pass 级 GPU 计时
审核人 / 审核版本 / 结论 / 问题编号：审核者 / dev 996a779 / 需修改 / R-M3-001 至 R-M3-003（见上方修复轮）

### M2 复审追加修复 / F02（回应 R-M2-001 补充）

任务编号 / 功能编号：M2 / F02，审核问题 R-M2-001 补充（几何包围盒仍可修改）
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待二次复审
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支"M2 复审追加：封闭几何包围盒入口"提交
修改文件 / 接口变化：
- include/sim/Scene.hpp：`BufferGeometry::minimum/maximum` 由公开可变成员改为 const 成员；新增私有嵌套 `Baked`、私有构造与静态 `bake`
- src/scene/Scene.cpp：构造改为"校验内容 + 算包围盒"一次完成，公开构造函数转交给私有构造，取消先构造后赋值
- tests/asset_check.cpp：新增几何与贴图渲染输入的 `static_assert`；新增包围盒取值与"部分可见网格仍被画出"的重放用例
- docs/REVIEW_M2_RESPONSE.md：追加复审回应段
已完成：
- 几何的渲染输入（`vertices`、`indices`、`minimum`、`maximum`、`id`）全部只读，包围盒不再是可写入口；旧快照的 CPU 剔除与阴影范围不会因外部改动而漂移
- 编译期检查锁住该约定：成员改回可变即编译失败，不需要运行期复现
仍缺项：
- 场景准备仍在调用线程同步录制，未提供异步准备
- 实例缓冲按快照版本重建，未做跨版本复用或环形更新
- 上传批未限流，未统计显存预算
- 同代际内被释放的资产，其 GPU 缓冲保留到切换代际才释放
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- cmake -S . -B build-dev；cmake --build build-dev --config Release
- ctest --test-dir build-dev -C Release --output-on-failure
- sensor_check|temporal_check|request_check|asset_check --validation
- gallery --smoke --validation；gallery --capture 输出
结果 / 日志截图位置：四项 CTest 通过；四项 --validation 通过；捕获图与仓库预览图 MD5 相同（356ab30b…）；asset_check 输出 "PASS: shared scene, immutable asset versions, upload dependency and deferred reclamation"
性能和画质对比：temporal_check 误差数值与上一轮逐位相同；未做性能测量
已知问题 / 下一步：等待二次复审；复审通过后 M2 验收，再进入 M3
审核人 / 审核版本 / 结论 / 问题编号：待填

### M2 修复轮 / F02（回应 REVIEW_M2）

任务编号 / 功能编号：M2 / F02，审核问题 R-M2-001 至 R-M2-004
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待复审
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支"M2 修复：固定资产版本与资产身份"提交
修改文件 / 接口变化：
- include/sim/Scene.hpp：删除几何、贴图、材质的 `revision`；新增 `AssetId` 与 `nextAssetId()`（构造时分配、进程内不复用）；`Texture` 改为构造函数并让 `width/height/rgba` 不可变；`MeshStandardMaterial` 成为值类型并带 `operator==`；`Mesh`、`Object3D` 保存材质值；`Scene::add` 增加按值挂载的重载
- src/scene/Scene.cpp：资产身份计数器、Texture 校验、材质按值复制；`Texture::load` 走新构造函数
- src/assets/AssetCache.hpp：几何/贴图按 `AssetId` 索引、材质按内容索引；新增 `pendingUploads()` 与 `stagingBytes()`
- src/backend/Device.hpp、Device.cpp：`UploadBatch::stagingBytes()`
- src/render/RenderScene.hpp、RenderScene.cpp：`Item` 固定材质值与几何版本；`matches` 比较材质内容与包围盒
- src/render/Renderer.cpp：`sceneKey` 改用资产身份与材质内容；`MeshHistory` 保存固定资产（几何 + 材质值）；`collect()` 同时回收当前代际的上传批；`statsOf` 汇总各代际的待回收批次与暂存字节
- include/sim/Renderer.hpp：`AssetStats` 增加 `pendingUploads`、`stagingBytes`，更新计数含义
- tests/asset_check.cpp：重写，覆盖四项审核问题
- example/Gallery.cpp、src/assets/Gltf.cpp：贴图改为先建像素再构造
- CMakeLists.txt：显式 `/utf-8 /EHsc`，修复此前的 C4530 警告
已完成：
- 旧快照固定在发布时的资产版本：材质按值进入快照，几何与贴图对象被快照持有；修改用户手里的材质对象只影响之后创建的快照，清缓存后旧快照按同一份固定资产重新上传
- 缓存键不再依赖地址：几何与贴图用构造时分配的身份，材质用内容加贴图身份，释放后的地址被复用也不会错误命中
- TAA 历史记录几何身份与材质值（含贴图身份），资产变化使静止冻结与重投影失效
- 上传批在每次准备时回收已完成批次，当前代际与旧代际都回收；暂存副本随批次释放
仍缺项：
- 场景准备仍在调用线程同步录制，未提供异步准备
- 实例缓冲按快照版本重建，未做跨版本复用或环形更新
- 上传批未限流，未统计显存预算
- 同代际内被释放的资产，其 GPU 缓冲保留到切换代际才释放；后续可加按代际内引用的驱逐
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- cmake -S . -B build-dev；cmake --build build-dev --config Release
- ctest --test-dir build-dev -C Release --output-on-failure
- sensor_check|temporal_check|request_check|asset_check --validation
- gallery --smoke --validation；gallery --capture 输出
结果 / 日志截图位置：四项 CTest 通过；四项 --validation 通过；捕获图与仓库预览图 MD5 相同（356ab30b…）；asset_check 输出 "PASS: shared scene, immutable asset versions, upload dependency and deferred reclamation"
性能和画质对比：temporal_check 误差数值与修复前逐位相同；未做性能测量
已知问题 / 下一步：等待复审；复审通过后进入 M3
审核人 / 审核版本 / 结论 / 问题编号：待填

### M2 / F02（首轮，保留为历史）

任务编号 / 功能编号：M2 / F02
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待审核
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支 M2 提交
修改文件 / 接口变化：
- 新增 src/render/RenderScene.hpp、src/render/RenderScene.cpp：快照版本的共享 GPU 数据，含绘制项、静态实例缓冲与上传完成值
- 新增 tests/asset_check.cpp，加入 CTest
- src/assets/AssetCache.hpp：缓存键改为身份 + 版本（贴图再加色彩空间）；上传批次化并记录计数
- src/backend/Device.hpp、Device.cpp：新增 UploadBatch、时间线信号量与完成值；设备启用 timelineSemaphore
- src/render/Renderer.cpp：新增 prepare 复用共享场景；视图只保留相机相关历史；帧提交等待上传时间线
- shaders/frame.glsl、geometry.vert、geometry.frag：静态实例与视图历史拆为两个 storage buffer
- include/sim/Scene.hpp：几何、贴图、材质新增 revision（已在修复轮删除）
- include/sim/Renderer.hpp：新增 AssetStats 与 Renderer::assetStats()
已完成：
- 同内容快照只准备一次，多相机共享网格、材质与静态实例数据
- 缓存键区分身份、版本与贴图色彩空间；原地修改必须递增版本才重新发布
- 场景首次出现的资产合并为一次上传提交，帧提交以时间线值声明依赖
- clearAssetCache 只切换代际；旧代际被在途结果或视图持有时不释放，最后一个引用消失后回收
仍缺项：
- 场景准备仍在调用线程同步录制，未提供异步准备
- 实例缓冲按快照版本重建，未做跨版本复用或环形更新
- 上传批未限流，未统计显存预算
环境 / 精确构建测试命令：Windows、Visual Studio 2022 x64、Vulkan SDK 1.4.357.0、RTX 3080 Ti
- cmake --build build-dev --config Release
- ctest --test-dir build-dev -C Release --output-on-failure
- sensor_check|temporal_check|request_check|asset_check --validation
- gallery --smoke --validation；gallery --capture 输出
结果 / 日志截图位置：四项 CTest 通过；捕获图与仓库预览图 MD5 相同；完整日志未归档
性能和画质对比：temporal_check 误差与改动前一致（raw=28.623、TAA=6.28194、移动轮廓 347.217/293.259）；未做性能测量
已知问题 / 下一步：该轮已被审核判定需修改，问题见 R-M2-001 至 R-M2-004；修复见上一节
审核人 / 审核版本 / 结论 / 问题编号：审核者 / dev bdfab25 / 需修改 / R-M2-001 至 R-M2-004

## 每次交接追加记录模板

```text
任务编号 / 功能编号：
负责人 / 日期 / 状态：
代码版本（commit；无 Git 时文件清单和 SHA256）：
修改文件 / 接口变化：
已完成 / 仍缺项：
环境 / 精确构建测试命令：
结果 / 日志截图位置：
性能和画质对比（若适用）：
已知问题 / 下一步：
审核人 / 审核版本 / 结论 / 问题编号：
```

保留失败和审核历史。开发者只能更新到待审核，独立审核者确认验收后再标已验收。
