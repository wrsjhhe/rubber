# M2 审核回应

回应日期：2026-11-20。被审版本 dev / bdfab25；修复版本为 dev 分支"M2 修复：固定资产版本与资产身份"提交。复审后追加"R-M2-001 补充"的修复，状态：待二次复审。

## 复审追加：R-M2-001 补充 / 几何包围盒（已封闭）

复审指出顶点与像素为 const 并未封闭整个渲染资产：`minimum/maximum` 仍是公开可变成员，调用方可保留 `shared_ptr<BufferGeometry>` 并改边界，`RenderScene::matches` 会按新边界重建，旧快照的 CPU 剔除与阴影范围随之漂移。

修复：包围盒不再是可写字段，而是与顶点一起在构造时算定的 const 成员。构造改为校验与算边界一次完成（`BufferGeometry::Baked` + 私有构造），公开构造函数只做转交，因此不存在先构造再赋值的窗口。`BufferGeometry` 的五个渲染输入（`vertices`、`indices`、`minimum`、`maximum`、`id`）现在全部只读。

验证：
- 编译期：`tests/asset_check.cpp` 对几何与贴图的每个渲染输入加 `static_assert`，断言成员类型为 const。缺陷注入核对——把 `minimum/maximum` 改回可变后，`asset_check` 立即以 `static_assert failed: 'Bounds are mutable'` 编译失败（C2338），确认该检查不是空转；随后已还原。
- 运行期：`BoxGeometry(2,4,2)` 的边界断言为 `(-1,-2,-1)..(1,2,1)`；把该网格上移 1.8 米使只有一部分落在视锥内，连续三次重放同一快照都画出中心深度 4.0，说明剔除与阴影范围使用的是发布时的边界。
- 回归：`ctest` 4/4、四项 `--validation`、`gallery --smoke --validation` 通过；`gallery --capture` 仍与 `example/preview/courtyard.png` 字节一致（`356ab30b…`），`temporal_check` 误差数值不变。

复审同时确认 R-M2-002 / 003 / 004 的修复通过复核，本轮未再改动这三处。

四项阻塞问题的根因是同一个：资产"版本"由可变的 `revision` 字段和裸地址表达。修复把这个模型换掉，而不是逐条打补丁：

- **固定资产**：几何顶点、索引与贴图像素改为 const 成员，构造后无法原地修改；材质改成值类型，快照按值保存。快照因此持有它发布时的那份内容，而不是指向一个随时可变的对象。
- **资产身份**：新增 `AssetId`，构造时从进程内单调计数器分配，永不复用。缓存按身份索引，不再依赖地址。

`revision` 已从公开接口删除。这是接口变更：修改资产内容的唯一方式是创建新对象，`README`、`TECHNICAL_DESIGN`、`ARCHITECTURE` 已同步。

## R-M2-001 / 旧快照未固定资产版本

修复：`SceneSnapshot::Mesh` 与 `RenderScene::Item` 都按值保存 `MeshStandardMaterial`，并持有 `Geometry` 与 `Texture` 的共享引用；`sceneKey` 与 `matches` 只读这些固定值，不再读对象上的实时字段。顶点与像素为 const，原地修改在编译期被拒绝，"原地更新后旧快照无法重现"的路径不再存在。

验证（tests/asset_check.cpp）：
- 先按 0.8 灰发布快照，再修改用户手里的材质对象和场景节点上的材质值，重提旧快照仍为 0.8 灰，且没有重新准备共享场景。
- 新快照应用新材质值（绿色），场景准备次数加一。
- `clearAssetCache()` 两次后重提旧快照，仍是 0.8 灰——新代际按快照自己持有的固定资产重新上传。

## R-M2-002 / TAA 历史未纳入资产版本

修复：`MeshHistory` 保存 `Geometry` 与完整的材质值（含三张贴图的不可变身份），`operator==` 覆盖全部字段；`known` 判断同样比较材质。资产或材质变化即令 `stationary` 为假，`info.w=-1` 走重投影路径，同时 `velocity.w=0` 使该像素不继承旧历史。

验证（tests/asset_check.cpp）：相机静止 18 帧收敛到红色贴图后换成绿色贴图，3 帧内 Color 通道由红转绿（此前同对象换贴图会让 `stationary` 继续成立、`info.w=0` 一直输出旧历史）。

缺陷注入核对：把历史比较临时放宽为只比几何与变换，同一用例在"Texture swap kept frozen TAA history"处失败，确认它确实覆盖本问题；随后已还原。

## R-M2-003 / 当前代际上传临时资源不回收

修复：`Renderer::Impl::collect()` 现在同时调用 `assets->collect()`；`AssetCache::collect()` 丢弃已完成批次并 `wait()`，`UploadBatch::wait()` 清空复制源。未完成批次保留，帧提交仍在等待其时间线值。新增 `AssetStats::pendingUploads` 与 `stagingBytes`，汇总当前代际与各旧代际。

验证（tests/asset_check.cpp）：连续 5 个不同几何各产生一批上传，随后用同一快照再提交一次（不新增上传），`pendingUploads==0`、`stagingBytes==0`、`uploadBatches` 不再增加。

## R-M2-004 / 缓存裸地址身份可复用

修复：`AssetId nextAssetId()` 用原子计数器，构造时分配、进程内单调；`BufferGeometry` 与 `Texture` 各带 `id`。缓存改为 `std::map<AssetId, GpuMesh>` 与 `TextureKey{identity, srgb}`；材质键由内容加三张贴图身份组成。复制几何或贴图会带着同一 id，但两者负载是 const，复制体与原对象逐字节相同，因此同 id 不会产生歧义。

验证（tests/asset_check.cpp）：上传一个 1×1×1 盒子后释放全部引用（快照、场景、视图、结果都销毁），新建顶点数相同、深度不同的 1×1×3 盒子，断言身份严格增大、网格重新上传一次、中心深度为 3.5 而不是旧的 4.5。

## 一并处理

- CMakeLists.txt 显式加 `/utf-8 /EHsc`：本机缓存里 `CMAKE_CXX_FLAGS` 为空，之前所有目标实际没有 `/EHsc`，asset_check 编译 `<chrono>` 时报 C4530。属构建配置缺口，不是本轮引入，但会影响异常展开语义，顺手修掉。
- Gallery 与 glTF 导入改为先建像素再构造贴图，输出像素与改动前逐字节相同。

## 仍未解决

- 同代际内被释放的资产，其 GPU 缓冲保留到切换代际才释放（身份不复用，所以是内存占用而非正确性问题）。可加代际内引用驱逐，本轮未做。
- 场景准备仍在调用线程同步录制；上传批未限流，未统计显存预算。这两项是 M3 之前仍存在的性能缺口。
- 未配置 debug messenger，验证层信息只经层默认渠道，可见性有限。

## 复现

```text
cmake -S . -B build-dev
cmake --build build-dev --config Release
ctest --test-dir build-dev -C Release --output-on-failure
asset_check --validation
gallery --smoke --validation
gallery --capture out.png   # 应与 example/preview/courtyard.png 字节一致（MD5 356ab30b5a9ba71818e38eec7babd280）
```
