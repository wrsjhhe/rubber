# M4 独立审核

## 复审：2b763cc（文档 1a1857f，2026-11-20）

结论：仍需修改一处历史推进条件。R-M4-001 纯投影双轴焦距、R-M4-003 附件读取掩码已修复；无 HDR 时选择五附件管线及点云 SSBO 声明也已核对。R-M4-002 的一般颜色/几何请求已修复，但 Hdr-only 请求仍存在缺口。

独立 Release、七项 CTest、六项 --validation、Gallery smoke 全通过；控制台未见错误。TAA 数值保持静止 28.623/6.28194、运动 347.217/293.813。本轮未独立复做缺陷注入、截图差异比较或同步验证专门配置。

### R-M4-002 补充 / P1：Hdr-only 会推进实际未写入的颜色历史

Renderer::submit 对 Channel::Hdr 也设置 color=true，传给 res.color。PointsView::declare 因此更新 historyVersion/historyCount。然而仅导出 Hdr 时，TemporalResolve/ColorTransform 无导出根，可被帧图裁掉；Renderer 在提交后仍按 if(color) 更新 stillSamples、lastVP、historyNumber 等。

例如先将静止点云的 Color 收敛，修改点云后只请求 Hdr，再请求 Color：点云基线已被 Hdr 请求推进，但颜色历史仍是旧内容，下一帧可误判静止并冻结旧图。即便点云不变，Hdr-only 推进历史槽奇偶也会使后续读取的历史不再对应实际最近的时序输出。现有补测只覆盖 geometry-only，不覆盖 Hdr-only。

修复要求：区分“需要 HDR/光照”和“实际执行并成功提交 TemporalResolve”；View 和各模块的颜色历史基线只能在后者成立时提交更新。避免在 declare 阶段提前提交历史状态。补 Color 收敛 → 改点云 → Hdr-only → Color，以及连续 Hdr-only 穿插 Color 的测试，验证活跃 pass、历史槽与输出内容。此补充为代码路径检查，本轮尚未添加专项复现测试。

---

版本：d4f07f0（文档 1b56a74）；日期：2026-11-20。结论：**需修改，M4 暂不验收**。本轮未修改产品代码，以下问题来自代码路径检查，尚未新增专项复现程序。

## 独立验证

build-dev Release 构建、七项 CTest 全通过；points_check --validation、Gallery smoke --validation 成功，控制台未见校验错误。本轮未独立重做截图比较、其余测试的 validation 或性能测量。无 debug messenger 时，不能把控制台无错误等同于完整捕获验证消息。

## R-M4-001 / P1：使用 VP 元素作为焦距，点尺寸随相机姿态错误变化

shaders/points.vert 使用 abs(frame.vp[1][1]) 换算半径，但 vp 是投影乘视图，不是纯投影矩阵。取绝对值只修复负号，不能消除相机旋转：俯仰/翻滚会改变该元素，90 度翻滚时可接近零，圆盘又退化到最小像素。像素内参 fx!=fy 时仅用纵向尺度也不能正确表达两个轴的投影。

修复：显式传递纯投影焦距/两个轴尺度，或在视空间构造圆盘后用投影矩阵变换。补固定距离下相机俯仰/翻滚，以及 fx!=fy 的覆盖测试；不要只测试前后方向与分辨率。

## R-M4-002 / P1：点云未实现历史变化声明，静止 TAA 会冻结旧点云

PointsView 未覆写 ModuleView::unchanged，始终继承 true。Renderer 汇总 unchanged 后可令 info.w=0；相机与网格静止积累满 16 帧后，移动、替换、删除点云或改变半径/强度，仍判定 stationary，temporal 的静止分支继续使用旧历史。Motion 写零不能解决此问题，因为静止分支不使用运动匹配判断。

修复：每个 PointsView 保存上次实际颜色历史对应的点云身份/变换/内容状态，变化返回 false，并处理从有点云到无点云；几何-only 请求不能错误推进颜色历史基线。补静止收敛后移动/换色/删除点云的 Color 回归及两相机独立历史测试。

## R-M4-003 / P1：颜色读改写屏障缺少附件读取访问

FrameGraph::Declaration::readWrite(..., ColorWrite) 保留了旧生产者，但 stageAccess(ColorWrite) 仍只有 COLOR_ATTACHMENT_WRITE。点云 pass 使用 LOAD 和 HDR 混合，需要读取旧附件值；plan 未根据读改写标志增加 COLOR_ATTACHMENT_READ，故依赖图有边不代表屏障已使先前写入对附件读取可见。

修复：为附件读改写建立正确的读写访问掩码，或单独定义附件读写访问类型。补 graph_check 检查 LOAD/混合对应屏障的 dstAccess 包含 READ|WRITE，并在支持时启用同步验证检查实际混合路径。

## 后续事项

点云 SSBO 在 PointsView 中绑定但没有在 declare/declareLate 登记为帧图 StorageRead；目前上传时间线与 FrameLifetime 在图外提供同步/保活，但与完整资源声明目标不一致，建议补登记及测试。RenderPoints 的 UploadBatch 随整个点云驻留，已完成的 staging 未主动回收；大点云应补回收接口与暂存字节检查。这两项也应在交接中明确，避免声称点云资源已全部纳入图管理。

请先修复三项阻塞问题并补能捕获原错误的测试，再提交复审。网格回归通过和模块目录拆分不替代点云变化与任意相机姿态的正确性验收。
