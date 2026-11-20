# M2 独立审核

## 复审：8c35d79（2026-11-20）

已阅读 REVIEW_M2_RESPONSE.md 并核对代码。独立执行 Release 构建、四项 CTest、asset_check --validation、gallery --smoke --validation，全部通过；未复做本版截图比较或缺陷注入实验。

R-M2-002、003、004 的对应修复与专项测试通过复核：完整材质值进入历史匹配，当前代际上传被收集，缓存使用 AssetId。R-M2-001 的材质与贴图隔离已修复，但几何边界仍有一个未封闭入口：

### R-M2-001 补充 / P2：几何包围盒仍可修改

include/sim/Scene.hpp:28 的 minimum/maximum 仍为公开可变成员。调用方可以直接构造 shared_ptr<BufferGeometry>，发布快照后保留该可变引用并修改包围盒。RenderScene::matches 会发现边界变化并按新边界重建，旧快照重新提交时 CPU 剔除/阴影范围随之变化，可能将原本可见网格剔除。顶点/像素 const 并未让整个渲染资产不可变。

建议将包围盒作为构造时计算的只读属性（或快照固定其值），补发布后边界不可变的编译期检查或旧快照重放测试。当前结论仍为需修改；前三项已修复不必重复改动，封闭该入口后再关闭 R-M2-001 并验收 M2。此补充为代码检查发现，尚未添加运行时复现测试。

---

日期：2026-11-20。审核版本：dev / bdfab25。结论：需修改，不能标为已验收。本轮未修改产品代码。

## 功能验证

独立执行 build-dev Release 构建、四项 CTest（sensor_check、temporal_check、request_check、asset_check）全部通过。四个测试逐一以 --validation 运行通过，gallery --smoke --validation 退出成功，控制台未见验证错误。未配置 debug messenger，因此不把无控制台输出等同于完整捕获验证消息。

TAA 数值：静止 raw=28.623、TAA=6.28194；运动 raw=347.217、TAA=293.259。本轮未重做截图比较和性能测试。以下问题是代码路径审核发现，尚无新增自动化复现测试。

## 阻塞问题

### R-M2-001 / P1：旧快照未固定资产版本

Scene::snapshot 复制共享指针而非版本化内容；RenderScene::Item 同样保存当前对象引用，matches 比较两侧引用上的实时 revision。按新增公开约定原地修改材质并递增 revision 后，再提交旧快照，sceneKey 会读取新 revision 并准备新内容，旧快照无法重现原画面。现有 asset_check 只验证新对象发布的旧快照隔离，没有在原地修改后重提受影响的旧快照。

修复：发布时持有不可变资产版本，或取消原地发布承诺并强制新对象；快照、RenderScene 和历史记录保存固定版本。补旧快照在原地更新后、清缓存后重新提交的验证。

### R-M2-002 / P1：TAA 历史未纳入资产版本

Renderer.cpp 的 MeshHistory/known 比较指针与少量材质标量，没有几何/材质/贴图 revision。相机静止积累满 16 帧后，在同一材质对象上更换贴图并递增材质 revision，GPU 材质可重新上传，但 stationary 仍可能成立，info.w=0 继续输出旧历史，颜色不会更新。

修复：历史使用固定资产及依赖版本，变更使静止冻结和不兼容重投影失效。补静止收敛后更换纹理的 Color 测试，不能仅测 AlbedoMetal。

### R-M2-003 / P1：当前代际上传临时资源不回收

Renderer::Impl::collect 只遍历 retired，没有调用当前 assets->collect。AssetCache::flush 持续向 inflight 追加 UploadBatch，上传完成的 staging 和命令缓冲在正常持续加载过程中仍保留，直到清缓存/销毁。该问题不只是上传批缺少限流，而是已完成上传也不回收。

修复：正常准备/提交路径收集当前代际已完成批次；保留未完成批次。增加批次数/暂存字节可观测值，验证多批上传完成后临时资源回落。

### R-M2-004 / P1：缓存裸地址身份可复用

AssetCache 的 GeometryKey/MaterialKey/TextureKey 使用裸地址和 revision，但缓存条目不保活相应 CPU 对象。所有快照/视图释放某资产后缓存仍在；新资产若分配到同一地址且默认 revision=1，就可能错误命中旧网格/纹理/材质。

修复：使用不会因地址复用而冲突的稳定资产 ID，或保活身份对象并定义回收策略；补释放旧对象、复用身份位置后的内容验证。

## 后续复核

开发者修复后，提供新版本及上述专项测试，再重复现有回归与 validation。M2 维持需修改，M3 不应建立在上述版本和寿命漏洞之上。
