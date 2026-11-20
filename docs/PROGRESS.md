# 实现进度

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
| F03/F04 | 已有实现，待审核；Renderer.cpp、Pipeline.cpp、shaders、temporal_check | 固定 pass；resolve 合并颜色变换；动态效果失效协议未实现 |
| F05/F06 | 已有实现，待审核；Output.hpp、Renderer.hpp、VulkanInterop.hpp、request_check | 三份结果上限，导出复制，同 View 串行；旧 API 仍为最近帧 |
| F05 几何路径 | 已有实现，待审核；submit/renderFrame | 跳过光照/TAA，新 View 不建颜色历史；仍写五个 G-buffer，用过颜色的 View 保留颜色资源 |
| F02 缓存拆分 | 已有实现，待二次复审；render/RenderScene.hpp、render/RenderScene.cpp、assets/AssetCache.hpp、backend/Device.hpp、tests/asset_check.cpp | 场景准备仍在调用线程录制；实例缓冲按快照版本重新分配，未跨版本复用；上传批未限流 |
| F07/F08 | 待开发 | 无通用帧图及 mesh/points 模块协议 |
| F09 | 部分实现 | 已有内参、时间、真值输出和互操作头；无独立 Observation/sensors/presentation |
| F10 | 待开发 | 无 install/export/find_package 验证 |
| F11—F17 | 未实现 | 后续规划，不列为已支持 |

GPU 遮挡剔除已移除，保留 CPU 视锥剔除。

## 可交接任务

| 任务 | 依赖/状态 | 范围 | 验收证据 |
|---|---|---|---|
| M1 | 已有实现，待审核 | F05/F06 请求、结果、槽位、互操作 | 双相机、旧结果不变、Busy/恢复、销毁 View/Renderer 后读取、非法请求及 GPU 寿命 |
| M2 | M1；已有实现，待二次复审 | RenderScene、共享静态实例数据、不可变资产版本、上传依赖、延迟回收 | asset_check：同快照两相机准备与上传各一次、旧快照在原对象被修改后与清缓存后仍重现原画面、身份不复用、静止收敛后换贴图的 Color 更新、上传批与暂存回落、旧代际最终回收、渲染输入只读的编译期断言 |
| M3 | M2；待开发 | 帧图及六类独立 pass，资源声明与分配分离 | 未初始化读取/循环/冲突诊断；图像子资源/缓冲依赖；导入/导出；按需活跃 pass 清单；validation |
| M4 | M3；待开发 | 网格模块迁移、协议和点云 | 双相机下点/网格前后遮挡；主调度无点云分支；强度/半径保留；深度/ID 语义明确 |
| M5 | M4；待开发 | Observation、处理链入口、呈现隔离、互操作边界 | Beauty+调试同帧不串色；校准/时间一致；真值独立；无需实现全部 F11—F13 即可验收框架 |
| M6 | M5；待开发 | 安装头文件/库/shader、导出 target/依赖 | 新安装前缀的外部项目 find_package 构建并离屏运行，不访问原源码/构建目录；关闭 example 可构建 |

每项保留现有回归。F11—F17 分别立项，先补规格和质量/性能门槛。

## 已有验证与缺口

上一批：Release 构建成功；sensor_check、temporal_check、request_check 三项 CTest 通过；request_check --validation 和 gallery --smoke --validation 通过。参考环境 Windows、RTX 3080 Ti；精确驱动和完整日志尚未归档，不宣称跨设备验证。

本轮（M2 修复 + 复审追加）：Release 构建成功，构建新增显式 `/EHsc`，此前 asset_check 的 C4530 警告消失；sensor_check、temporal_check、request_check、asset_check 四项 CTest 通过；四项分别以 --validation 运行完成；gallery --smoke --validation 通过；gallery --capture 输出仍与 example/preview/courtyard.png 字节一致（MD5 356ab30b5a9ba71818e38eec7babd280），temporal_check 误差数值与改动前相同（raw=28.623、TAA=6.28194、移动轮廓 347.217/293.259），说明资产模型重构与包围盒只读化未改变画面。参考环境同上一批，驱动版本仍未归档。

缺陷注入核对：把 TAA 历史比较临时放宽为只比几何与变换（去掉材质与贴图身份）后，asset_check 在"Texture swap kept frozen TAA history"处失败；把几何包围盒改回可变成员后，asset_check 以 `static_assert failed: 'Bounds are mutable'`（C2338）编译失败。两处用例因此确认有效，均已还原。

尚缺按需 pass/显存分配自动证据、全部通道/交付组合，以及帧图/点云/安装测试。清缓存安全性由 asset_check 的在途结果覆盖，尚未覆盖多视图同时提交的压力场景。引擎未安装 debug messenger，验证层输出只经层默认渠道，未见报错但可见性有限。

## 交接记录

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
