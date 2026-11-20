# M3 审核回应

回应日期：2026-11-20。被审版本 dev / 996a779（文档 51490c7）；首轮修复版本为 dev 分支"M3 修复：帧图的区间与子资源跟踪"提交（083f8fe，文档 ecb81a3）；复审补充项见文末"R-M3-001 补充"。状态：待复审。

三项阻塞问题的共同特征是"用一个状态代表多个已声明的区间"：缓冲把整块当一个状态、图像把 mip×layer 压平成连续区间、写入的依赖只取第一个写入者与最后一个 cell 的读取者。修复把这三处都改成逐个声明区间跟踪，而不是在原有单状态上加特例。

## R-M3-001 / P1：缓冲按区间跟踪初始化与生产者

修复：`BufferResource` 不再有单一 `state` 与"最小到最大"的 `written` 并集，改为 `segments`——一组有序、不重叠、只覆盖已声明字节的区间，每段各自带 `writer`、`defined`、`readers`、`lastWrite`。

- `segmentsOf(buffer, begin, end)` 在声明边界处切分已有段，并用导入声明推导的默认状态（无写入者、`defined = initialized`）填补空洞，返回恰好覆盖该次访问的字节区间。中段未写的空洞不会被并入已写范围。
- 依赖、禁用条件与屏障都按段计算，屏障范围就是段的实际字节范围，不再外扩到整块。
- 裁剪对导出缓冲逐段激活生产者：两段各自写入后整体导出，两个生产者都保留。
- 导出只对本次真正写过的段发屏障。
- 未初始化读取逐段拒绝：写了 `[0,32)` 却整体读 `[0,64)` 会在编译期报错。

验证（`tests/graph_check.cpp`）：
- 完全未写的缓冲被读取并标为副作用 → `reads uninitialized buffer scratch`。
- 只写了一半再整体读取 → `reads uninitialized buffer half`。
- 非重叠访问（读 `[0,32)`、写 `[32,64)`）不产生冲突，且两边的屏障范围分别等于各自声明的区间，没有覆盖对方字节。
- 两段各自写入后整体导出：两个生产者都在执行清单里，导出屏障两条、分别对应 `[0,32)` 与 `[32,64)`。

缺陷注入核对：
- 把"导出逐段激活生产者"改回"只激活最后一段"后，用例在 `Buffer export only kept the last range producer` 失败。
- 删掉缓冲的未初始化诊断后，用例在 `Expected a diagnostic containing: reads uninitialized buffer scratch` 失败。
- 两处均已还原。

## R-M3-002 / P1：多 layer 的 mip 子范围按精确 cell 集展开

修复：`cellRange()` 返回首末下标的写法删除，改为 `cellsOf()` 按 layer × mip 双重循环展开成精确下标集。`Subresource{mip=1,mipCount=1,layer=0,layerCount=2}` 在 4 mip、2 layer 的图上只得到 cell 1 与 cell 5，不再访问 1..5。

同时补了导出/恢复的边界：`Cell` 增加"本帧是否被活跃 pass 访问过"标记，导出与恢复只转换这些子资源。此前导出会把从未声明的 mip 一并转到目标布局。

验证（`tests/graph_check.cpp`）：
- 只写 `mip1 / layer0..1` 后导出：最终屏障恰好两条，分别为 `(layer 0, mip 1)` 与 `(layer 1, mip 1)`，每条都是 `mipCount=1`、`layerCount=1`。
- 同一张图读取未被声明的 `mip2` → `reads uninitialized image atlas`，说明压平实现造成的"误标已初始化"已消除。

缺陷注入核对：用 996a779 的 `FrameGraph` 跑同一份用例，在 `Multi-layer mip range touched extra subresources` 失败（压平到 5 个子资源）；还原后通过。

## R-M3-003 / P1：多子资源写入建立完整的 WAW/WAR 边

修复：`Use` 现在记录本次访问的精确 `cells`（图像）与 `pieces`（缓冲），并把相交子资源上的依赖去重合并：

- `writers` 收集**全部**相交 cell/段上的写入者（去重、丢弃 `-1`），不再只取第一个。
- `readers` 收集**全部**相交子资源上的读取者，不再被最后一个 cell 的读取者覆盖。
- `order()` 对每次写入建立 WAW（所有旧写入者 → 本次写入）与 WAR（所有旧读取者 → 本次写入），并对重复的 pass 对去重；读取访问对全部写入者建立 RAW。自环被跳过，避免同一 pass 多次声明同一资源时被误判成循环。

验证（`tests/graph_check.cpp`）：
- 两个 mip 各有生产者、另有被导出的产出，整体覆写句柄按更小下标预留 → 执行顺序为 `mip0 → mip1 → overwrite`；漏掉任一 WAW 边都会让覆写插到生产者之前。
- 读取者只读其中一个 mip 并被 `after` 拖后 → 顺序为 `late → hold → overwrite`，验证 WAR 边来自"全部相交子资源"而不是最后一个 cell。
- 缓冲侧同样两条：跨区间 WAW（`write-high → write-low → overwrite`）与跨区间 WAR（`hold → overwrite`）。

缺陷注入核对：
- 把写入分支改回只保留第一个写入者 → `Cross-subresource WAW edge was lost`。
- 去掉 WAR 边 → `Cross-subresource WAR edge was lost`。
- 两处均已还原。

## 行为变化

导出与恢复现在只转换本帧被活跃 pass 访问过的子资源；未被声明的 mip/layer 保持原状。消费者的注意事项是：需要整图可读时，就声明整图的读写范围，而不是依赖导出顺带把未使用的子资源一并转换。

## 回归

- Release 构建通过；`sensor_check`、`temporal_check`、`request_check`、`asset_check`、`pass_check`、`graph_check` 六项 CTest 全通过。
- 五项分别以 `--validation` 运行，均 0 条校验错误；`gallery --capture --validation` 退出 0、0 条校验错误。
- 画面与上一版一致：捕获图与 `example/preview/courtyard.png` 差异 37472/4096000 字节（0.915%），`max_delta=1`、`mean_delta=1`。
- `temporal_check` 静止 raw=28.623 / TAA=6.28194、运动 347.217 / 293.813，与上一版逐位相同，说明区间与子资源跟踪的改写没有改变渲染结果。

## 其它

按复核要求，`docs/PROGRESS.md` 中 M2 的状态表已整理为"已验收（278371f）"，历史"待二次复审"行标注为已完成，避免与当前状态混淆。

---

## R-M3-001 补充 / P1：声明阶段的合并段不能用于执行阶段状态重放

复核指出的问题成立：`mergeSegments` 会把最终状态相同的相邻区间合并，而 `plan()` 在重放时把 `defined`/`stage`/`access`/`lastWrite` 写到整段 `segment.state` 上，于是对 `[0,32)` 的一次写入会把 `[32,64)` 一并标成已定义。

修复：**执行期状态表与声明期段表彻底分开**，不再共用一个结构。

- 声明期的 `segments` 只服务裁剪与排序：需要 `writer`/`readers`/`defined`，合并是正确的压缩，保留。
- `plan()` 开头为每个缓冲新建一张覆盖 `[0, size)` 的执行期区间表，初始状态完全由导入声明推导（`defined = lastWrite = initialized`），与声明期的段划分无关。
- 重放前按**所有活跃访问的 `pieces` 边界**预切分这张表。每个边界都是某次访问的起止，因此每次访问恰好等于若干整段，更新范围精确到被访问的字节，不存在"截取 barrier 范围却共享整段状态"。
- 导出收尾改为读执行期区间表：只对 `lastWrite` 为真的区间发屏障，范围取执行期真实边界，不会覆盖帧内从未写过的空洞。

验证（`tests/graph_check.cpp` 新增两条）：

- **局部写 + 读另一半 + 整体覆写**：A 写 `[0,32)`（带独立副作用，因此不会被覆写裁掉、且必定先于读取执行）、B 读 `[32,64)`、C 写 `[0,64)` 并导出。声明期 A 与 C 的写使两段最终状态相同、被合并成 `[0,64)`，执行期仍必须报 `reads uninitialized buffer scratch`。
- **完整写后分段读、再整体覆写**：以存储读、传输读、uniform 读分别读 `[0,16)`、`[16,32)`、`[32,64)`，随后整体覆写。覆写 pass 的三条屏障源访问分别是 `SHADER_READ`、`TRANSFER_READ`、`UNIFORM_READ`，范围分别是 `[0,16)`、`[16,32)`、`[32,64)`——每段的状态来自该段自己的最后一次访问；导出收尾覆盖本帧写过的全部 64 字节。

缺陷注入核对：
- 用 083f8fe 的 `FrameGraph` 跑同一份用例：第一条在 `Expected a diagnostic containing: reads uninitialized buffer scratch` 失败（合并段把未写区间标成已定义），第二条在 `Merged range lost the per-segment source state of the overwrite barriers` 失败。还原修复后两条通过。
- 注回"不按访问边界预切分执行期区间表"后，用例在 `Buffer export produced no barrier` 失败，说明预切分是这套跟踪的必要条件，不是可选优化。
- 注入均已还原（`grep 缺陷注入` 为 0）。

回归结果与首轮修复逐位一致：六项 CTest 全通过；五项 `--validation` 与 `gallery --capture --validation` 均 0 条校验错误；捕获图与参考图差异 37472/4096000（0.915%）、`max_delta=1`；`temporal_check` 静止 raw=28.623 / TAA=6.28194、运动 347.217 / 293.813 不变。
