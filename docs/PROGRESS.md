# 实现进度

基线核对日期：2026-11-20。M2 在 dev 分支完成实现，本轮执行了 Release 构建、四项 CTest、验证层运行和画廊捕获；结论仍为待审核，不等同独立审核。

状态：待开发 → 开发中 → 待审核 → 已验收。部分实现必须列缺项，不用百分比代替证据；“已验收”需独立审核结论。

## 代码事实

| 功能 | 状态与位置 | 缺项/限制 |
|---|---|---|
| F01/F02 | 已有实现，待独立审核；Scene.hpp、src/scene、src/assets/Gltf.cpp | 静态不透明导入子集；资产版本与共享场景见下方“F02 缓存拆分”行 |
| F03/F04 | 已有实现，待审核；Renderer.cpp、Pipeline.cpp、shaders、temporal_check | 固定 pass；resolve 合并颜色变换；动态效果失效协议未实现 |
| F05/F06 | 已有实现，待审核；Output.hpp、Renderer.hpp、VulkanInterop.hpp、request_check | 三份结果上限，导出复制，同 View 串行；旧 API 仍为最近帧 |
| F05 几何路径 | 已有实现，待审核；submit/renderFrame | 跳过光照/TAA，新 View 不建颜色历史；仍写五个 G-buffer，用过颜色的 View 保留颜色资源 |
| F02 缓存拆分 | 已有实现，待审核；render/RenderScene.hpp、render/RenderScene.cpp、assets/AssetCache.hpp、backend/Device.hpp、tests/asset_check.cpp | 场景准备仍在调用线程录制；实例缓冲按快照版本重新分配，未跨版本复用；上传批未限流 |
| F07/F08 | 待开发 | 无通用帧图及 mesh/points 模块协议 |
| F09 | 部分实现 | 已有内参、时间、真值输出和互操作头；无独立 Observation/sensors/presentation |
| F10 | 待开发 | 无 install/export/find_package 验证 |
| F11—F17 | 未实现 | 后续规划，不列为已支持 |

GPU 遮挡剔除已移除，保留 CPU 视锥剔除。

## 可交接任务

| 任务 | 依赖/状态 | 范围 | 验收证据 |
|---|---|---|---|
| M1 | 已有实现，待审核 | F05/F06 请求、结果、槽位、互操作 | 双相机、旧结果不变、Busy/恢复、销毁 View/Renderer 后读取、非法请求及 GPU 寿命 |
| M2 | M1；已有实现，待审核 | RenderScene、共享静态实例数据、版本缓存、上传依赖、延迟回收 | asset_check：同快照两相机准备与上传各一次、新旧版本隔离、清缓存后在途结果有效、旧代际最终回收 |
| M3 | M2；待开发 | 帧图及六类独立 pass，资源声明与分配分离 | 未初始化读取/循环/冲突诊断；图像子资源/缓冲依赖；导入/导出；按需活跃 pass 清单；validation |
| M4 | M3；待开发 | 网格模块迁移、协议和点云 | 双相机下点/网格前后遮挡；主调度无点云分支；强度/半径保留；深度/ID 语义明确 |
| M5 | M4；待开发 | Observation、处理链入口、呈现隔离、互操作边界 | Beauty+调试同帧不串色；校准/时间一致；真值独立；无需实现全部 F11—F13 即可验收框架 |
| M6 | M5；待开发 | 安装头文件/库/shader、导出 target/依赖 | 新安装前缀的外部项目 find_package 构建并离屏运行，不访问原源码/构建目录；关闭 example 可构建 |

每项保留现有回归。F11—F17 分别立项，先补规格和质量/性能门槛。

## 已有验证与缺口

上一批：Release 构建成功；sensor_check、temporal_check、request_check 三项 CTest 通过；request_check --validation 和 gallery --smoke --validation 通过。参考环境 Windows、RTX 3080 Ti；精确驱动和完整日志尚未归档，不宣称跨设备验证。

本轮（M2）：Release 构建成功；sensor_check、temporal_check、request_check、asset_check 四项 CTest 通过；四项分别以 --validation 运行完成；gallery --smoke --validation 通过；gallery --capture 输出与 example/preview/courtyard.png 字节一致（MD5 356ab30b5a9ba71818e38eec7babd280），temporal_check 误差数值与改动前相同，说明实例数据拆分为静态/视图两部分未改变画面。参考环境同上一批，驱动版本仍未归档。

尚缺按需 pass/显存分配自动证据、全部通道/交付组合，以及帧图/点云/安装测试。清缓存安全性由 asset_check 的在途结果覆盖，尚未覆盖多视图同时提交的压力场景。引擎未安装 debug messenger，验证层输出只经层默认渠道，未见报错但可见性有限。

## 交接记录

### M2 / F02

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
- include/sim/Scene.hpp：几何、贴图、材质新增 revision
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
已知问题 / 下一步：M3 帧图及六类独立 pass，资源声明与分配分离；构建未启用 /EHsc，测试依赖异常路径时有 C4530 警告，属既有问题，本轮未改
审核人 / 审核版本 / 结论 / 问题编号：待填

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
