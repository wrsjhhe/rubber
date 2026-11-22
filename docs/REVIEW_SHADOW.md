# 方向光阴影：CSM+PCSS+接触阴影 / Ray Query 独立审核

日期：2026-11-21。对象：工作树中**未提交**的阴影改动（`git status` 含 19 个修改文件、`src/render/RayShadows.cpp`、`example/ShadowDemo.*`、`docs/phase2/`）。基线为 M6 已验收代码 `67e11ec`。

结论：**两条路径都实现得可用，架构（能力探测 + 显式强制）方向正确，但当前不能按现状合入**。三项 P1 都集中在"默认值/默认路径"上：默认阴影方案由硬件决定、默认参数改变全部既有调用者的画面、新快照每帧同步重建 TLAS。路线选择本身不是二选一问题，见文末建议。

**复审（2026-11-22，工作树第二版）**：改动把路线收敛为"CSM+PCSS 为默认、Ray Query 只增强接触阴影"，R-S-001、R-S-003、R-S-005、R-S-008、R-S-010 关闭；R-S-002（默认画面改变）仍开放，只缺基线证据；新增 N-1—N-7，其中 N-1（没有"射线确实变暗"的断言）、N-2（示例场景体现不出增强价值）建议提交前处理。详见文末复审节。

---

### 开发补证（代码 `12e09f8`）

默认画面对照和新基线、画面外遮挡的正向回归及缺陷注入、Offscreen caster 示例、PCSS 搜索核说明均已记录在 [PROGRESS](PROGRESS.md#方向光阴影审核补证2026-11-22--12e09f8)。下文保留各轮审核当时的状态。

## 1. 被审范围

| 部分 | 内容 |
|---|---|
| 设备 | 三级扩展 + 特性探测，`rayQuery` 开关，函数指针装载，AS 属性查询 |
| 光追路径 | `RayShadows.cpp`：BLAS 按 GpuMesh 缓存、TLAS 按 RenderScene 缓存、同步 fence 构建 |
| 光栅路径 | `lighting.frag` 新增 PCSS（16 tap 遮挡搜索 + 16 tap 滤波）、屏幕空间接触阴影 |
| 管线 | `lighting.ray.frag` / `lighting.unshadowed.frag` 两个变体，独立描述符布局（binding 6 为 AS） |
| 公共接口 | `DeviceOptions::forceRasterShadows`、`RenderOptions::{sunAngularRadius, contactShadowLength, forceRasterShadows}`、`Renderer::usesRayQueryShadows()` |
| 示例/测试 | `--shadow-demo`、`--shadow-benchmark`、ShadowScene、`pass_check` 新增断言 |
| 文档 | README、TECHNICAL_DESIGN 有说明；PROGRESS / FEATURES 无条目 |

## 2. 独立验证（本轮实际运行）

- Release 增量构建通过（`gallery`、`pass_check`、`temporal_check`）。
- `pass_check --validation`：退出 0，控制台 0 条 VUID。覆盖默认 Ray Query、强制 CSM、同视图来回切换、空 TLAS、投射物开关。**这台机器支持光追，非光追分支未被覆盖。**
- `temporal_check`：`328 silhouette pixels raw=4.78522 TAA=1.23697`，与 M6 记录完全一致——因为该用例 `shadows=false`，不能作为阴影路径未回归的证据。
- 默认 gallery 场景 1280×800，`--capture` 默认（Ray Query）对比 `--raster-shadows`（CSM）：
  `differing_bytes=491506/4096000 (12.000%) max_delta=70 mean_delta=3.687`。
  M6 同期参考比对接受的量级是 `0.915% / max_delta=1`。
- `gallery --shadow-benchmark`，960×600，RTX 3080 Ti，Release，render+wait（与 README 表相差 8–12%，可重复）：

| 场景 | 无阴影 | CSM+PCSS+接触 | Ray Query |
|---|---:|---:|---:|
| 长柱廊 静止 | 0.948 / 1.490 | 3.747 / 4.714 | 1.074 / 1.760 |
| 长柱廊 移动投射物 | 1.066 / 1.175 | 3.502 / 3.730 | 1.795 / 2.014 |

- 未运行：七项 `--validation` 全量、`gallery --smoke`、`package_check`、定向缺陷注入、接触近景基准。

## 3. P1（阻塞）

### R-S-001：默认阴影路径由硬件能力决定，且没有逐帧记录

`Device::init` 探测到 `VK_KHR_ray_query` + AS + BDA + Synchronization2 即置 `rayQuery=true`；`MeshView::declare` / `renderFrame` 随后直接 `d.rayQuery && !options.forceRasterShadows`。`RenderOptions` 没有"用哪个模型"的字段，`FrameResult`/`FrameStats` 也不记录实际路径。

后果是可量化的：同一份源码和资产，默认画面在两台机器上差 12.0% 字节、最大通道差 70（满量程 255）。这对一个以"真值、可复现、参考图比对"为验收方式的仿真渲染器不成立——它意味着合成数据集的内容取决于生成它的工作站有没有 RT 单元。

`usesRayQueryShadows()` 返回的是**设备能力**而非本帧选择：同一 Renderer 上 `forceRasterShadows=true` 渲染后查询仍为 `true`，关闭 shadows 时也为 `true`。名称与语义不符，示例和测试都据此分支，容易误判。

要求：把模型变成显式枚举（见 R-S-010），默认值与硬件无关；逐帧记录实际使用的模型与参数（至少进 `FrameStats`/结果元数据）；参考图按模型分别建立基线。

### R-S-002：默认参数改变全部既有调用者的画面和开销，且没有基线证据

`sunAngularRadius` 默认 `0.012`、`contactShadowLength` 默认 `0.3`，两者都直接进入 `RenderOptions`。任何不显式设置这些字段的既有代码（gallery、外部消费者、旧测试）都会**静默地**从 9 tap PCF 变成 PCSS + 16 步接触阴影，着色期每像素最多 48 次纹理取样。

这与 `docs/phase2/VALIDATION.md`「Gallery 默认画面不应变化」直接冲突，也没有伴随证据：没有截图对比、没有七项 validation 记录、`docs/PROGRESS.md` 无条目。README 只记录了性能表，没有记录"默认画面已改变"。

要求：默认保持 M6 行为（`radius=0`、`contact=0`），把质量提升做成显式预设；或者先按流程给出全量基线与重新验收证据，再改默认。

### R-S-003：新快照每帧同步重建 TLAS，默认路径上串行化 CPU/GPU

`RenderScene::shadowAcceleration` 在 `declare()` 内自建命令缓冲、`vkQueueSubmit`、`vkWaitForFences(UINT64_MAX)`。移动投射物每帧产生新快照 → 每帧一次全队列排空，同时重新分配 scratch。

实测 960×600：静止 1.074 ms，移动 1.795 ms（+0.72 ms）。本机单帧约 1 ms，所以并行损失被掩盖；真实仿真负载（几十毫秒一帧）下，这个等待等于每帧丢掉一次 CPU/GPU 重叠，`first` 5.655 ms 也说明首次构建不便宜。

要求：AS 构建进入本帧命令缓冲（帧图声明 AS 读、执行侧构建），或用双缓冲 TLAS + 时间线信号量，不在 CPU 上等；scratch 缓存复用；基准把"首次构建/每帧重建"单列。

## 4. P2

### R-S-004：PCSS 搜索半径的物理量用错

`searchRadius = clamp(q.z * texelsPerDepth, 2, 12)`。`texelsPerDepth` 的定义是"每单位 NDC 深度差对应多少 texel"（已含 `tan(角半径)` 与 `1/depthScale`），而 `q.z` 是受光面在光源投影下的归一化深度，不是"受光面与遮挡物的间距"。典型级联里 `q.z*texelsPerDepth` 远大于 12，于是搜索半径恒定饱和在 12 texel，16 个 tap 覆盖 24 texel 直径，遮挡深度均值抖动明显。滤波半径那一行 `(q.z - blockerDepth)*texelsPerDepth` 才是量纲正确的。

要求：搜索半径由最大遮挡间距（或级联范围）推导，写清 12 texel 上限的物理含义；如果默认 0.7° 太阳下半影只有几个 texel，应说明 PCSS 相比原 PCF 的实际收益，并提供 PCF/PCSS/接触三段的分项计时。

### R-S-005：光追软阴影只有 4 条固定方向射线

`sunVisibility` 用固定的 ±0.7071 切向偏移打 4 条射线，无抖动、不参与历史累积，结果量化到 0 / 0.25 / 0.5 / 0.75 / 1 五级。默认 0.7° 太阳下半影只有几像素宽，五级量化可见；而在大角半径下又明显不如 PCSS 平滑。

要么在文档里明确"首版仅 4 射线近似、不作为软阴影质量目标"，要么补 blue-noise 抖动 + TAA 累积规则，或做 ray-based blocker search + filter。**当前形态下，光追路径的软阴影质量并不优于 PCSS，只有硬阴影精度和遮挡正确性占优。**

### R-S-006：新增测试只断言"字节不同"

`pass_check` 的 `expect(pcf[0] != pcss[0])`、`expect(pcss[0] != contact[0])` 对任何扰动都成立（改偏置、改抖动都能通过），不能证明本影更暗、半影单调、接触长度为 0 时回退、两条路径在硬阴影区一致。CI 上没有 RT 设备时，ray 分支完全不执行，只打印一行也不算覆盖。

要求补：几何可判定的场景（盒体投影到地面），断言指定像素的分类（本影 < 半影 < 受光）；`contactShadowLength=0` 且 `radius=0` 时逐字节回到 PCF；两路径在纯本影/纯受光区的差 ≤ 容差；对 `sunAngularRadius` 越界、AS 超限的失败路径。

### R-S-007：加速结构资源未计入统计、无上限、无回退

BLAS/TLAS/scratch 不进 `AssetStats`，phase2 要求峰值内存有界；scratch 每次构建新分配不缓存；`maxPrimitiveCount`/`maxInstanceCount` 超限直接 `throw`，而光栅路径没有对应限制——同一个场景可能在 RT 机器上渲染失败、在非 RT 机器上正常。

### R-S-008：AS 构建的同步依赖是隐含的

构建能看到顶点数据，靠的是"上传批先提交 + 单队列提交顺序 + 命令缓冲内的 TRANSFER→AS_BUILD 屏障"，没有时间线信号量显式等待。已知缺项"多队列"一旦落地，这条会静默失效。至少要把它写成不变量，或直接等待 `uploadTimeline`。

### R-S-009：安装与着色器探测没覆盖新变体

`shader_check` 只探 `geometry.vert.spv`；`package_check` 不检查 `lighting.ray.frag.spv`、`lighting.unshadowed.frag.spv` 是否随包安装。缺文件时症状是运行期 `Pipelines` 构造抛错，而不是安装期失败。

### R-S-010：公共接口命名

`DeviceOptions::forceRasterShadows` 与 `RenderOptions::forceRasterShadows` 同名不同层，双重否定，调用方很难判断该设哪个。建议统一为 `ShadowModel { Cascades, RayQuery, Off }`，需要硬件自适应时再显式写 `Auto`（并在文档里声明"允许结果随硬件变化"）。

### R-S-011：流程

改动未提交、无功能编号与验收门槛、PROGRESS 未记录；自建的 `gallery --shadow-benchmark` 与 M7 计划中的 `gallery --bench` 重复。建议立 F18（阴影模型）并入 M7，按既有流程走：规格 → 实现 → 七项 validation + 截图基线 + 缺陷注入 → 两段提交。

## 5. 两条路线对比

| 维度 | CSM + PCSS + 接触阴影 | Ray Query |
|---|---|---|
| 硬件门槛 | 任意 Vulkan 1.3 | 需 RT 扩展 + AS + BDA + Sync2 |
| 输出可复现 | 与设备无关（同一驱动内确定） | 与设备/驱动相关，跨机不同 |
| 遮挡正确性 | 级联范围外无阴影；有 bias/acne/彼得潘 | 真实遮挡物，含视锥外，无级联边界 |
| 近场接触 | 依赖屏幕空间，离屏几何看不到 | 自然正确 |
| 软阴影质量 | PCSS 半影连续（但半径公式需修正） | 4 射线五级量化，明显粗 |
| 远场分辨率 | 受级联分辨率与 texel 对齐限制 | 与距离无关 |
| 本机成本（960×600） | 3.50–3.75 ms | 1.07 ms 静止 / 1.80 ms 移动 |
| 成本随什么增长 | 级联数 × 图集分辨率 + 每像素 48 次取样 | 像素数 × 场景复杂度（射线遍历） |
| 新失败模式 | 无 | AS 超限、TLAS 重建停顿、驱动差异 |
| 与既有验收 | 已在 M1–M6 路径内 | 全新增量，验证面大 |

成本数据要谨慎解读：CSM 的 3.5 ms 是"4 级 1024² 深度 pass + PCSS + 接触阴影"的整体，不是 PCF 的成本；光追的 1.8 ms 已含每帧 TLAS 重建。要拿这个做选型，还缺 1080p、真实仿真资产、开 TAA 的对照。

## 6. 建议

1. **不要二选一**：现有架构（能力探测 + 显式强制 + 共用光照 pass）是对的，保留双路径。
2. **默认必须与硬件无关。** 在本项目约束下（通用安装、参考图比对、数据集一致性），默认选 CSM，Ray Query 作为显式质量档；如果车队统一是 RT 卡、且更看重遮挡正确性，可以反过来，但必须先满足三条：逐帧记录模型、随机采样替代 4 固定射线、构建不阻塞 CPU。
3. **短期先把 P1 处理掉**，再谈默认值；R-S-002 的"默认改画面"不解决，任何阴影选型都会污染既有验收证据。
4. **长期**：光追侧值得继续投入（ray-based PCSS 或随机采样 + TAA），但停在 4 条固定射线时，它只是"更准的硬阴影 + 更差的半影"，不足以替换 PCSS。
5. **如果本轮只想留一条**：留 CSM。它是唯一在所有设备上都存在、且能与历史基线对齐的路径；光追作为可选质量档，不参与默认与验收门槛。

## 7. 需要开发者答复

- 目标客户的 GPU 分布是否保证 RT？数据集是否允许同一场景在不同机器上产出不同画面？
- 1080p、真实仿真资产、开 TAA 时两条路径的 p50/p95，以及 CSM 里 PCF/PCSS/接触三段的分项耗时？
- 是否接受把 AS 构建移进帧图（去掉 `declare()` 内的 fence 等待）？
- `sunAngularRadius` / `contactShadowLength` 的默认值是否愿意回到 0，把质量提升作为显式预设？

---

# 复审（2026-11-22，工作树第二版）

对象：同一批未提交改动的第二版。改动性质是**重新收敛路线**——不再是"整个方向光阴影二选一"，而是"CSM+PCSS 恒为底座，Ray Query 只替换接触阴影"。这个收敛解决了上一轮最根本的问题（默认画面随硬件漂移），我认可方向。

## 1. 本轮实际运行

- Release 增量构建通过（gallery / pass_check / temporal_check）。
- `ctest -C Release`：**10/10 通过**，含 `package_check`（165.96 s，实际完成安装 + 外部消费者构建与离屏运行，说明 `lighting.ray.frag.spv` 已随包安装）。
- 七项设备测试逐个带 `--validation`：`asset_check`、`pass_check`、`points_check`、`request_check`、`sensor_check`、`temporal_check`、`observation_check`，全部退出 0，**0 条 VUID**。`pass_check --validation` 覆盖了新路径：帧内构建、跨视图复用 TLAS、移动投射物重建、屏幕空间↔Ray Query 双向切换。
- `gallery --smoke --validation` 与 `gallery --shadow-demo --smoke --validation`：退出 0，0 条 VUID。
- 默认捕获 vs `--raster-shadows` 捕获（1280×800，默认场景）：**0 字节不同，max_delta=0**。默认输出已与硬件能力无关。
- `temporal_check`：328 / raw=4.78522 / TAA=1.23697 / 105.933 / 62.5146，与 M6 记录一致（该用例 `shadows=false`）。
- `--shadow-benchmark`（960×600，RTX 3080 Ti）：静止 关阴影 0.929 / 屏幕接触 3.593 / 射线接触 3.575；移动 1.123 / 3.686 / 4.010；`submit p50` 3.289 / 3.312、3.503 / 3.681。与 README 表相差 <1%。
- `--shadow-demo --capture` 左右两半做逐通道比对（本机自写 PNG 解码，非仓库工具）：接触近景 0.027% 通道不同（max 56），长柱廊 0.197%（max 71）。

## 2. 上轮问题状态

| 编号 | 状态 | 依据 |
|---|---|---|
| R-S-001 默认随硬件漂移 | **关闭** | 默认 `rayQueryContactShadows=false`；默认与禁能两路捕获逐字节相同；`ShadowInfo` 逐帧记录 `cascaded`/`contact`/`rayQueryRequested`/参数 |
| R-S-002 默认参数改变既有画面 | **仍开放** | `sunAngularRadius=0.012`、`contactShadowLength=0.3` 仍是默认值，默认 Beauty 必然不同于 M6 的 9-tap PCF；README 已声明，但没有量化与基线记录 |
| R-S-003 新快照同步建 TLAS | **关闭** | 构建进入帧图 `shadow.acceleration` pass，`declare()` 内不再有 `vkQueueSubmit`/`vkWaitForFences`；scratch 与实例输入缓冲按视图复用；移动场景 submit p50 仅比静止高 0.4 ms，不再出现全队列排空 |
| R-S-004 PCSS 搜索半径量纲 | **未改** | `searchRadius=clamp(q.z*texelsPerDepth,2,12)` 原样保留；默认 0.012 rad 下恒饱和在 12 texel。文档已写"最多 12 texel"，但没有说明这是上限而不是物理半影 |
| R-S-005 4 条固定射线 | **消解** | 整帧光追路径已移除，改为单条接触射线 + 最近距离淡出，不再是半影方案 |
| R-S-006 测试只比字节 | **部分关闭** | 新增球体明暗交界、跨视图一致性、回退元数据、0 接触距离保留 CSM、pass 顺序断言，质量明显提升；但仍有一条关键缺口，见 N-1 |
| R-S-007 AS 资源未计入统计 | **未改** | BLAS/TLAS/scratch 仍不在 `AssetStats`；`maxPrimitiveCount`/`maxInstanceCount` 超限仍直接抛异常，光栅路径无对应限制 |
| R-S-008 构建同步依赖隐含 | **关闭** | 上传时间线等待的 stage 增加 `ACCELERATION_STRUCTURE_BUILD`；构建与查询在同一命令缓冲内用 Synchronization2 屏障连接 |
| R-S-009 安装未覆盖新变体 | **基本关闭** | `shader_outputs` 已含 `lighting.ray.frag.spv`，`package_check` 通过说明安装树可用；`shader_check` 仍只探 `geometry.vert.spv`，无 RT 设备时该变体不会被加载 |
| R-S-010 接口命名 | **关闭** | 改为正向 `rayQueryContactShadows`、`supportsRayQueryContactShadows()`，并新增 `ShadowInfo`。`DeviceOptions::forceRasterShadows` 名称与新语义不符，见 N-5 |
| R-S-011 流程 | **待提交** | PROGRESS / FEATURES 仍无阴影条目；提交时需一并记录 |

## 3. 新发现

### N-1 / P2：没有断言"射线接触阴影确实变暗"

现有测试只证明射线路径**不该变的时候不变**（凸球明暗交界逐字节相等）、**元数据正确**、**回退一致**；没有任何用例断言它在应当遮挡的位置产生更暗的结果。把 `rayContactVisibility` 改成恒返回 1，全部测试仍会通过。

要求：在"盒体立在地面、太阳高度角约 20°"的场景里，取 CSM 半影外、接触距离内的一个区域，断言射线模式的平均辐射低于屏幕空间模式（或至少存在超过阈值的差异像素）。这一条比现有的字节不等断言更有区分度。

### N-2 / P2：示例场景体现不出增强的价值

两条接触路径在当前两个场景里只差 0.027%（近景，474/1.73M 通道）和 0.197%（柱廊，3403/1.73M 通道）。原因是这些场景的遮挡物都在画面内，屏幕空间本来就能看到；射线路径的真正优势（**遮挡物在相机后方或画面外**）没有被演示。

建议加第三个场景：相机贴近墙面，投射物位于相机背后，屏幕空间必然漏光而射线能正确压暗。否则 `--shadow-demo` 的说服力与基准数字不相称，评审也无法判断这个增强值不值得维护。

### N-3 / P2：TECHNICAL_DESIGN 出现重复标题

`docs/TECHNICAL_DESIGN.md` 第 46 行和第 50 行都是 `## 共享、同步与结果`，中间夹着一段孤立的"两条路径都只调制方向光直射分量……"。插入"方向光阴影"一节时把原小节标题复制了一份。合并即可。

### N-4 / P2：加速结构仍无预算与统计

同上轮 R-S-007。现在是可选功能，影响面小于上一版，但 phase2 的"峰值内存有界、缓存不随捕获次数增长"仍没有覆盖 AS。至少把 BLAS/TLAS/scratch 字节数计入 `AssetStats`，或在文档里明确"不计入"。

### N-5 / P3：两处命名/注释与语义脱节

- `DeviceOptions::forceRasterShadows` 现在只禁用 Ray Query 接触能力，不再"强制光栅方向光阴影"，名称会让调用方误判。
- `RenderOptions::sunAngularRadius` 的注释仍写"为零时使用单条阴影射线或固定核 PCF"，单条阴影射线的语义已不存在。

### N-6 / P3：空 TLAS 用例在重构中被删除

上一版有"关闭全部投射物后输出等于无阴影"的断言，这一版删掉了。空实例列表会走 `primitiveCount=0` 的构建尺寸与 `max(bytes, sizeof(instance))` 的分配分支；建议补回，顺带覆盖 0 实例的构建与查询。

### N-7 / P3：镜像实例的正面判定

短射线用 `rayQueryGetIntersectionFrontFaceEXT` 过滤"同一实例的背面"。负行列式（镜像缩放）实例的绕序被翻转，正面/背面判定随之反转，这条过滤会跳错面。当前场景没有镜像实例；若以后支持，需要按行列式符号翻转判定，或在文档里声明不支持。

## 4. 复审结论

**可以提交**，前提是补上三件小事：

1. R-S-002 的基线：用默认参数重新捕获 gallery 参考图，记录变化量与性能差，并写进 PROGRESS；说明这是有意的默认质量升级，而不是实现副产物。
2. N-1 的断言（射线确实会压暗），否则这条路径没有回归保护。
3. N-3 的重复标题。

N-2、N-4 建议同批处理；N-5—N-7 可留到后续。R-S-004 的 PCSS 半径公式如果确认按"12 texel 上限 + 固定搜索核"作为设计选择，请在 TECHNICAL_DESIGN 里明说，不要再写成"按角半径估算半影"——现在的文字暗示它随距离变化，实际被上限吃掉。

---

# 三审（2026-11-22，工作树第三版）

对象：同一批未提交改动的第三版，回应二审核定的三件必办事项（R-S-002 基线、N-1 正向断言、N-3 重复标题），并顺带处理了 N-2 与 R-S-004 文档。复核原则：记录里的数字一律重算，不采信自述。

## 1. 独立复核

### R-S-002 基线：通过

- 重算 `example/preview/courtyard-pcss.png` 与 `build-dev/m6fix-capture.png`：1280×800，通道不一致 **68,019 / 4,096,000（1.661%）**，其中 57,746 个差 >1，最大 56，受影响像素 **23,748 / 1,024,000**。与 PROGRESS 逐项相同。
- 与仓库旧预览 `courtyard.png`：**104,111（2.542%）**，最大 56。另复核 `m6fix-capture.png` 与旧预览只差 0.915%、`max_delta=1`，因此"后一组包含历史预览自身偏差"的说法成立，不是把旧差异算进新基线。
- 一次性：`courtyard-pcss.png`、`build-dev/courtyard-new.png`、`build-dev/courtyard-confirm.png` 三者 MD5 全同（`9498b94d…`），两次捕获确为逐字节一致。
- 更强的一条：我用当前默认参数重新执行 `gallery --capture`，与提交的参考图 **0 字节不同**。参考图来自当前代码路径，不是过期图。

### N-1 正向断言：通过，负向保护也是活的

- 正常着色器：`pass_check --validation` 退出 0、0 条 VUID，输出 `PASS: CSM and hybrid contact shadows, shared and dynamic TLAS`。
- 注入 A（你声明的那条）：用 `build-dev/offscreen-negative-control` 的恒 `1` 着色器运行，在 `Ray Query did not darken the predicted offscreen contact` 失败。断言确实拦得住"接触查询不生效"。
- 注入 B（我自己补的，验证另一条断言不是恒真）：删掉 `rayQueryGetIntersectionFrontFaceEXT` 的背面过滤并重编译，在 `Ray Query darkened the terminator of a convex sphere` 失败。说明凸球明暗交界的负向保护同样在起作用。
- 断言结构本身合理：整帧遍历确认遮挡条标签未进 G-buffer（挡住"其实看得见"的假阳性），板中心要求至少 6.25% 的暗化余量（half 位加 64），板外对照像素要求逐字节相等。

### N-2 第三场景：通过，且比预期更干净

我原本怀疑左半其实有 CSM 阴影（CSM 从光源视角渲染，相机背后的物体也可能落在贴合视锥内），那样这个 demo 就只是"接触精度"对比。实测否掉了这个怀疑：

- 左半蓝通道最小值 187，`<170` 的像素 **0 个**；右半 `<170` 有 102,672 个。左右列均值在非阴影列完全重合（202 / 192 / 193）。
- 即相机背后 0.21 m 的五根条既不在 G-buffer，也不在 CSM 贴合视锥里，左侧是纯环境梯度。这是真正的"两条路径都看不见、只有 Ray Query 查得到"，不是拿模糊的级联阴影冒充。
- 左右差 **308,016 / 2,304,000 通道（13.369%）、102,672 / 576,000 像素（17.825%）、最大 63**，与记录一致；重新捕获的 PNG 与 `build-dev/shadow-offscreen.png` MD5 相同（`38624181…`），可复现。

### N-3 / R-S-004 文档：通过

- `## 共享、同步与结果` 在 TECHNICAL_DESIGN.md 只剩 1 处。
- 文字与代码对齐：`shadowDisk[16]` 固定 16 点、`searchRadius=clamp(q.z*texelsPerDepth,2,12)`、`radius=clamp((q.z-blockerDepth/blockers)*texelsPerDepth,0,12)`。文档改为分别描述"搜索尺度有 12 texel 上限"和"滤波半径另算"，不再暗示半影随距离线性增长。
- 两条接触路径的上限一致（都是 `1-.85*(1-smoothstep(.65,1,t))`），"最大遮挡强度一致"成立。

### 构建与测试

- Release 增量构建通过；`ctest -C Release` **10/10**（package_check 127.73 s，总计 130.35 s）。
- `pass_check --validation` 0 VUID；`gallery --shadow-demo --shadow-offscreen --smoke --validation` 退出 0、0 VUID。
- `--shadow-benchmark`：静止 3.610 / 3.572 ms、移动 3.726 / 4.062 ms（屏幕 / 射线），与 README 表差 <2%。

## 2. 新增发现

### N-8 / P2：`FEATURES.md` 仍没有阴影条目

整套阴影（CSM 图集、PCSS、接触、Ray Query、AS 缓存）现在有技术设计、README 说明、PROGRESS 记录，但 `docs/FEATURES.md` 里唯一的"阴影"字样是 F16c 把软阴影列为未来工作。结果是这个功能没有规格级的验收门槛：参考图容差多少、RT 路径的额外开销上限、什么叫"离屏遮挡可用"、非 RT 设备上必须等价到什么程度，都没有可判定条文。后续任何人调整默认参数或射线参数，只能靠现场感觉判断是否回归。

建议补一节（F18 或 F16 的子项），至少写：默认路径契约（无 RT 设备必须逐字节等价）、参考图容差、两条路径的像素级不变量（几何真值通道不得变化）、性能门槛与测量协议。

### N-9 / P2：没有 RT 设备时 N-1 静默跳过

`tests/pass_check.cpp` 的行为是：`if (automatic.supportsRayQueryContactShadows())` 才执行 N-1、跨视图复用、凸球负向等全部断言，`else` 分支只校验回退元数据，而且 **`PASS: CSM and hybrid ...` 这句只在 if 分支里打印**。所以在一台没有 RT 的 CI 机器上，整套新回归保护不执行，输出仍然只显示 `PASS: frame graph culls ...`，看起来全绿。

本机是 3080 Ti，问题不会暴露；但只要 CI 混入非 RT 设备，这条保护就形同不存在。建议在 else 分支打印明确的 `SKIP: ray-query assertions (device lacks capability)`，或让 `--validation` 下把该分支视为失败要求，由构建方决定。

### N-10 / P3：PROGRESS 缺"验证"一行

M1—M6 每次记录都带构建/ctest/七项 validation/冒烟的结果，这次阴影条目只有四项数据，没有验证段。你消息里已有"Release 构建、10/10 CTest、定向 Vulkan 同步校验、Demo 冒烟"，补成一行即可，否则这份记录无法证明"测过"。

### N-11 / P3：README 接触近景的 p95 复现不出来

p50 对得上（表 0.611 / 0.589，我测 0.599 / 0.583），但静态接触近景的 p95 表里是 0.780 / 0.829，我连测两次是 1.129 / 0.923。亚毫秒区间里 p95 由噪声主导，单次 100 帧采样不足以稳定。既然写了"一次实测"，建议同时注明波动幅度，或对亚毫秒行只报 p50 并说明原因——否则 p95 会被当成门槛读。

### N-12 / P3：AS 显存仍不进 `AssetStats`（沿用 R-S-007）

这一点三版未变。既然第二阶段的"峰值内存有界"是硬要求，至少要在 TECHNICAL_DESIGN 里明说 BLAS / TLAS / scratch 是否计入，避免下阶段实测时才发现统计不全。

### N-13 / P3：负向对照没进仓库

`build-dev/offscreen-negative-control` 与 `build-dev/terminator-negative-control` 是构建目录里的临时产物，不入库。注入的补丁内容只存在于我的复核过程和你的对话里，半年后无法重放。建议把两处注入写成 `tests/negative/` 下的一行 diff 或脚本说明，或在 PROGRESS 里贴出被替换的原行与替换行。

## 3. 结论

**通过，可以提交。** 二审核定的三项全部关闭，证据可独立重算，参考图与当前代码一致，两条新断言都经注入验证为活。

提交前建议顺手处理 N-8、N-9（各一段文字 + 一句打印）；N-10、N-11、N-13 属记录质量，可同批；N-12 留给第二阶段 S2-00 一并定。

时点说明：以上结论针对当前工作树（未提交，22 个修改文件 + `RayShadows.cpp` / `ShadowDemo.*` / `docs/phase2/` / 新参考图）。`docs/phase2/` 是第二阶段计划，与本功能无冲突，提交时不要漏掉。
