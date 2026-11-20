# M3 独立审核

审核日期：2026-11-20；实现版本 996a779，文档版本 51490c7。结论：**需修改，M3 暂不验收**。以下为代码路径确认的问题，尚未添加专项自动化复现；本轮未修改产品代码。

## 独立功能测试

build-dev Release 构建通过，六项 CTest 全通过。sensor_check、temporal_check、request_check、asset_check、pass_check 分别以 --validation 运行成功；gallery --smoke --validation 成功，控制台未见校验错误。未安装 debug messenger，不宣称捕获全部验证消息。

TAA 静止误差 raw=28.623 / TAA=6.28194，运动 347.217 / 293.813，与交接记录一致。本轮未复做截图差异统计及性能测量。现有主路径测试通过不代表下述通用帧图契约已经满足。

## R-M3-001 / P1：缓冲没有按区间跟踪初始化和生产者

FrameGraph::addUse 对 BufferRange 只做边界检查，依赖全部记录在单个 res.state；written 合并成最小至最大的一段，会把中间未写的空洞算入。cull 导出缓冲只激活最后一个 writer，plan 的缓冲分支又没有未初始化读取检查，也未更新 defined。

例如 A 写 [0,16)，B 写 [16,32)，随后导出整个缓冲，A 可被裁掉；或创建未初始化缓冲后直接 StorageRead 并将读取 pass 标为 sideEffect，compile 不会报未初始化错误。读取有空洞的区域也不能被可靠诊断。这不是保守屏障优化问题，而是输出缺失/未定义数据问题。

修复要求：分割并跟踪缓冲区间的 writer/readers/defined/stage/access；导出激活所有必要区间生产者，未初始化读取逐区间拒绝。补测试：完全未写、部分未写、两段写后整体读/导出、非重叠访问不产生错误冲突、跨段 RAW/WAR/WAW。

## R-M3-002 / P1：多 layer 的 mip 子范围被错误展开

cellRange 把首末 layer/mip 映射为连续 [first,last]，addUse/plan 遍历整个区间。若 mipLevels=4，声明 mip=1,mipCount=1,layer=0,layerCount=2，实际应只访问 cell 1、5，当前却访问 1..5，额外修改 layer0 的 mip2/3 及 layer1 的 mip0 的状态、生产者和屏障。

这会把未写 mip 误标为已初始化，产生虚假依赖，且实际发出非声明范围的布局转换。修复为 layer×mip 双重遍历或精确 cell 集，补至少两层四 mip 的非全范围测试，检查无关子资源不被初始化/转换。

## R-M3-003 / P1：多子资源写入丢失 WAW/WAR 边

addUse 的图像循环用 use.readers = cell.readers 覆盖之前 cell 的 reader 集；order 的写入分支仅查看 use.writers.front()。因此跨多个 mip/layer 的写入只保留首个 writer 和最后一个 cell 的 readers。

当不同子资源有不同生产者/读取者且这些 pass 都活跃，结合 reserve 顺序或 after 约束，后续覆盖写可能被排到遗漏的读取者/写入者之前。单队列屏障只能执行已排定的次序，不能修复错误的数据版本顺序。

修复要求：收集所有相交子资源的 writer 和 reader，去重后建立完整 WAW/WAR 边。补非顺序 reserve、显式 after 与多子资源覆盖写组合，验证所有旧读完成后才覆写。

## 复核要求

修复以上问题并新增能在当前实现上失败的 graph_check 用例，再重复六项回归及相关 validation。暂不进入依赖通用帧图正确性的 M4。建议进度文档同时整理 M2 的状态表为已验收，避免历史“待二次复审”行被误读为当前状态。
