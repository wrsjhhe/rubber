# M5 独立审核

## 最终复审：428d264（文档 3f82621，2026-11-21）

结论：**M5 在当前约定范围内通过验收，可进入 M6。** R-M5-001/002/003 全部关闭。本节取代下方历史需修改结论。

核对：lighting 独立于颜色输出，几何-only 调试路径会导入并恢复 HDR，模块根据 lighting 声明光照/点云附件；DerivedPixels 只公开只读描述和固定大小可写 span，64 位行跨度计算及返回前布局复核封闭原绕过路径；Observation 的输出描述取自 FrameResult，标定尺寸及有限性接受检查。真实焦距/主点由外部包装者负责的限制已明确。

独立验证：Release 构建、八项 CTest、七项设备测试 --validation、Gallery smoke 全部成功，控制台未见校验错误。TAA 新指标为静止 4.78522/1.23697、运动 105.933/62.5146，与交接一致。本轮未独立重做缺陷注入、截图差异统计及性能测量；无 debug messenger，不扩大为完整验证消息捕获结论。

非阻塞说明：几何-only + debug 虽然不导入/执行 display/history 的图路径，但仍调用 ensureDebug/ensureColor，后者实际分配 display 和 history。因此“没有分配显示图与历史槽”的交接表述不准确；目前应表述为“不执行时序/颜色变换”，物理资源按需拆分留待优化。普通不带 debug 的几何请求不受此说明影响。

处理链 CPU-only、曝光积分/畸变/噪声未实现、外部不受检像素越界写等已知边界继续保留。本次只修改审核文档，未改产品代码。

---

版本：3924da5（文档 80a8613）；日期：2026-11-20。结论：需修改，M5 暂不验收。本轮未修改产品代码；以下问题为代码路径检查发现，尚未添加专项复现测试。

## 独立验证

Release 构建、八项 CTest、七项设备测试 --validation、Gallery smoke --validation 全部成功，控制台未见验证错误。TAA 新指标：328 个轮廓像素静止 4.78522/1.23697，运动 105.933/62.5146，与交接一致。指标已改变，不与旧版原始数值直接比较。本轮未重做截图差异、缺陷注入和性能测量；无 debug messenger，验证消息可见性限制仍存在。

## R-M5-001 / P1：几何-only 请求选择调试通道会引用未导入 HDR

Renderer::submit 只根据输出通道计算 color；若 outputs 只有 MetricDepth/InstanceId，options.debugChannel=1..4，则 color=false。renderFrame 仍创建 debugImage 并添加带 sideEffect 的 presentation，但 hdrImage 只在 color=true 时导入，d.read(hdrImage) 会抛 unknown image。ensureDebug 分配物理图像不能代替图资源导入。

这是公开有效选项组合，且当前约定是选择调试通道便生成视图调试图。修复应统一规划：启用 presentation 时引入所需 HDR/光照依赖，或者明确规定只有请求 Debug 才生成（同时处理旧 render 窗口兼容）。补几何-only + 每种 debugChannel 的 submit/observe、CPU/GPU 交付测试，不能只测 Color+Debug 或 Debug-only。

## R-M5-002 / P2：派生输出描述仍可在处理阶段绕过声明校验

DerivedSink::write 返回可变 DerivedOutput&，其 desc.name/format/width/height、rowBytes 和 bytes 全部可改。阶段可先声明合法名称，再在 process 中把 desc.name 改为 Color 或其他阶段名称，或者改变尺寸却不调整数据。run 返回前没有重新校验，因而“全链唯一且不冒充输入通道”的承诺不成立。原始 FrameResult 虽未被物理覆盖，但派生结果契约和查找会出现歧义。

修复：只向阶段提供固定描述对应的可写像素范围，描述/布局保持只读；或返回前严格校验名字、格式、尺寸、行跨度及数据长度均符合声明。补处理阶段改名、重复名、修改布局/长度的拒绝测试。分配时同时检查 width*bytesPerPixel 的 uint32 溢出，避免巨大宽度得到零或过小缓冲。

## R-M5-003 / P2：公开 Observation 构造允许元数据与帧不一致

构造函数接受调用方提供的 SensorCalibration 和 vector<OutputInfo>，只检查部分标定字段和时间，不检查 infos 与 frame->outputs() 一致，也不检查标定分辨率等于帧输出尺寸。可用 64×48 的结果构造 width=640 的观测，或声明不存在的通道；outputs()/has() 与实际 read() 返回内容不一致，违背“元数据与像素属于同一次采样”的目标。

修复：输出描述直接取 FrameResult；构造入口限制为 Renderer 或验证完整一致性，校验标定尺寸及有效有限内参/近远平面。若允许外部包装而无法验证真实相机内参，应明确调用方责任，至少拒绝可验证的尺寸/通道矛盾。补错误尺寸、伪造通道/格式、非法标定测试。

修复并补测试后复审，再决定进入 M6。
