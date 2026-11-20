# 实现进度

## 最新状态（2026-11-20 / dev M3 提交）

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
| F02 缓存拆分 | 已有实现，待二次复审；render/RenderScene.hpp、render/RenderScene.cpp、assets/AssetCache.hpp、backend/Device.hpp、tests/asset_check.cpp | 场景准备仍在调用线程录制；实例缓冲按快照版本重新分配，未跨版本复用；上传批未限流 |
| F07 帧图 | 已有实现，待审核；render/graph/FrameGraph.hpp、FrameGraph.cpp、tests/graph_check.cpp | 单队列顺序执行；保守屏障（每 pass 保守 stage/access，不做合并优化）；瞬时资源按描述缓存复用，未做显存别名 |
| F08 | 待开发 | 无 mesh/points 模块协议，几何通道仍是单一 opaques 列表 |
| F09 | 部分实现 | 已有内参、时间、真值输出和互操作头；无独立 Observation/sensors/presentation |
| F10 | 待开发 | 无 install/export/find_package 验证 |
| F11—F17 | 未实现 | 后续规划，不列为已支持 |

GPU 遮挡剔除已移除，保留 CPU 视锥剔除。

## 可交接任务

| 任务 | 依赖/状态 | 范围 | 验收证据 |
|---|---|---|---|
| M1 | 已有实现，待审核 | F05/F06 请求、结果、槽位、互操作 | 双相机、旧结果不变、Busy/恢复、销毁 View/Renderer 后读取、非法请求及 GPU 寿命 |
| M2 | M1；已有实现，待二次复审 | RenderScene、共享静态实例数据、不可变资产版本、上传依赖、延迟回收 | asset_check：同快照两相机准备与上传各一次、旧快照在原对象被修改后与清缓存后仍重现原画面、身份不复用、静止收敛后换贴图的 Color 更新、上传批与暂存回落、旧代际最终回收、渲染输入只读的编译期断言 |
| M3 | M2；已有实现，待审核 | 帧图及六类独立 pass，资源声明与分配分离 | graph_check：裁剪、未初始化读取、同 pass 读写冲突、循环依赖、子资源屏障、缓冲字节范围、usage 诊断、导出终态；pass_check：按输出通道断言活跃 pass 清单（含首帧历史清除与状态变化后的重建）；六项 CTest 与五项 --validation
| M4 | M3；待开发 | 网格模块迁移、协议和点云 | 双相机下点/网格前后遮挡；主调度无点云分支；强度/半径保留；深度/ID 语义明确 |
| M5 | M4；待开发 | Observation、处理链入口、呈现隔离、互操作边界 | Beauty+调试同帧不串色；校准/时间一致；真值独立；无需实现全部 F11—F13 即可验收框架 |
| M6 | M5；待开发 | 安装头文件/库/shader、导出 target/依赖 | 新安装前缀的外部项目 find_package 构建并离屏运行，不访问原源码/构建目录；关闭 example 可构建 |

每项保留现有回归。F11—F17 分别立项，先补规格和质量/性能门槛。

## 已有验证与缺口

本轮（M3）：Release 构建成功；sensor_check、temporal_check、request_check、asset_check、pass_check、graph_check 六项 CTest 全通过；sensor/temporal/request/asset/pass 五项分别以 `--validation` 运行，均 0 条校验错误；gallery `--smoke --validation` 与 `--capture --validation` 退出 0、0 条校验错误；`gallery --capture` 与 example/preview/courtyard.png 差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`；temporal_check 静止误差 raw=28.623、TAA=6.28194 与 M2 逐位相同，运动轮廓 347.217/293.813（M2 为 293.259，源于 temporal 与 color 拆成两个 pass 的浮点累加顺序）。graph_check 不需要设备，可在无 GPU 机器上跑裁剪与诊断。

上一批：Release 构建成功；sensor_check、temporal_check、request_check 三项 CTest 通过；request_check --validation 和 gallery --smoke --validation 通过。参考环境 Windows、RTX 3080 Ti；精确驱动和完整日志尚未归档，不宣称跨设备验证。

本轮（M2 修复 + 复审追加）：Release 构建成功，构建新增显式 `/EHsc`，此前 asset_check 的 C4530 警告消失；sensor_check、temporal_check、request_check、asset_check 四项 CTest 通过；四项分别以 --validation 运行完成；gallery --smoke --validation 通过；gallery --capture 输出仍与 example/preview/courtyard.png 字节一致（MD5 356ab30b5a9ba71818e38eec7babd280），temporal_check 误差数值与改动前相同（raw=28.623、TAA=6.28194、移动轮廓 347.217/293.259），说明资产模型重构与包围盒只读化未改变画面。参考环境同上一批，驱动版本仍未归档。

缺陷注入核对：把 TAA 历史比较临时放宽为只比几何与变换（去掉材质与贴图身份）后，asset_check 在"Texture swap kept frozen TAA history"处失败；把几何包围盒改回可变成员后，asset_check 以 `static_assert failed: 'Bounds are mutable'`（C2338）编译失败。两处用例因此确认有效，均已还原。

尚缺显存分配自动统计证据、多队列/异步计算路径、显存别名复用，以及点云/安装测试。活跃 pass 清单已由 pass_check 按输出通道逐项断言，但只覆盖本仓库现存的通道组合，未覆盖全部请求×交付×选项组合。清缓存安全性由 asset_check 的在途结果覆盖，尚未覆盖多视图同时提交的压力场景。引擎未安装 debug messenger，验证层输出只经层默认渠道，未见报错但可见性有限。

## 交接记录

### M3 / F07 帧图与六类独立 pass

任务编号 / 功能编号：M3 / F07（帧图），关联 F03/F04
负责人 / 日期 / 状态：开发者 / 2026-11-20 / 待审核
代码版本（commit；无 Git 时文件清单和 SHA256）：dev 分支"M3：帧图与六类独立 pass"提交
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
审核人 / 审核版本 / 结论 / 问题编号：待填

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
