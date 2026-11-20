# Aperture · 仿真渲染器

一个可独立运行的 C++20 / Vulkan 1.3 离屏延迟渲染引擎。场景接口、相机和传感器输出位于 `include/sim`，Vulkan 实现在 `src`，GLFW 和 Dear ImGui 只在 `example` 中使用。开发与审核入口见 [FRAMEWORK](FRAMEWORK.md)，功能、技术、架构和进度分别维护。

![引擎实机渲染的相机展厅](example/preview/courtyard.png)

## 构建与运行

Windows 参考环境为 Visual Studio 2022 x64、CMake 3.24 及以上、Vulkan SDK 1.3 及以上（包含 glslc）。设备需要支持动态渲染、至少五个颜色附件和带计算能力的图形队列。以下命令在 renderer 目录运行。首次全新构建会下载校验过的固定版本 GLM、stb、GLFW、Dear ImGui。现有 build 已配置为使用本机下载的源码。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\gallery.exe
```

- `gallery --model path/to/asset.glb` 将静态不透明 glTF 模型放到展台上，也支持含外部缓冲和图片的 .gltf。
- `gallery --capture image.png` 无窗口渲染并导出 PNG。可与 `--model` 组合。
- `gallery --benchmark` 实测 1、4、8 个视图的批次耗时。每轮预热后统计 120 个批次，包含快照、录制、提交及最后一批 GPU 完成等待。
- `gallery --smoke --validation` 创建隐藏窗口、切换场景、调整窗口尺寸，运行 12 帧后退出。`sensor_check --validation` 验证传感器数据与剔除。validation 模式需要 SDK 验证层。
- 仅编译引擎可传入 `-DRENDERER_EXAMPLE=OFF -DBUILD_TESTING=OFF`。嵌入其他程序时可用 `DeviceOptions::shaderDirectory` 指向生成的 SPIR-V 目录。当前 gallery 直接使用构建目录的着色器和源码目录的示例资产，移动可执行文件时需一并考虑这两个路径。

## 特性清单

| 范围 | 当前实现 |
|---|---|
| 场景与资产 | 有父子关系的 Object3D、局部到世界变换、稳定实例 ID、场景快照、跨实例及视图共享网格和贴图 |
| 几何 | Box、Sphere、Cylinder、Cone、Capsule、点集 Convex、索引 TriangleMesh。各生成器输出统一 BufferGeometry |
| 材质 | GGX 金属粗糙度模型、底色/法线/金属粗糙度纹理、sRGB 解码、mipmap、线性 HDR、色调映射。环境项使用解析天空近似 |
| 光照 | 一盏方向光，最多 16 盏平方反比衰减点光。方向光采用四级 CSM、光空间 texel 对齐和 3×3 PCF |
| 剔除 | CPU 包围盒视锥剔除 |
| TAA | 静止时对 16 个 Halton 子像素样本取线性 HDR 平均，前景与天空边缘均按覆盖率积累。运动时使用无抖动速度、前景速度扩展、逐点深度检验、三次重建和邻域限幅。每个视图独立保存历史 |
| 传感器 | 按像素指定 fx/fy/cx/cy，GPU 附件访问，可选异步 RGBA8、米制 Z 和 uint32 实例 ID 批量读回，帧携带仿真时间 |
| 导入与示例 | 静态 glTF/GLB 节点、索引、UV0、核心 PBR 贴图，外部或 GLB 内嵌 PNG/JPEG。交互展厅、七种几何体、轨道相机、光照及 G-buffer 预览 |

## 最小离屏采集

```cpp
#include <sim/Renderer.hpp>

sim::Renderer renderer;
sim::Scene scene;
auto material = std::make_shared<sim::MeshStandardMaterial>();
auto id = scene.add(sim::BoxGeometry(), material);
auto view = renderer.createView(640, 480);

sim::PerspectiveCamera camera;
camera.position = {0, 0, 5};
camera.fx = camera.fy = 500;
camera.cx = 320;
camera.cy = 240;

sim::RenderOptions capture;
capture.readback = true;
auto snapshot = scene.snapshot(1.25);
renderer.render(snapshot, camera, *view, capture);
auto frame = view->read();
```

这个例子中心像素的深度为 4.5 米，实例编号为 id。深度是相机坐标系中沿光轴的正距离，背景的深度和 ID 均为零。每个 View 同时容纳一个未完成提交，可在读回前先提交其他视图。read() 等待本视图并返回 CPU 自有副本；再次提交同一视图会覆盖此前的 GPU 输出。切换相机、跳转仿真或替换场景时设置 resetHistory。

## GPU 数据契约

`View::imageView(Channel)` 暴露由视图持有的 VkImageView。附件在 render 提交完成后可按 SHADER_READ_ONLY_OPTIMAL 读取。外部消费者应使用相同设备，协调提交与资源生命周期。当前 Renderer 的录制、提交和缓存接口按单线程调用设计；场景快照可由调用方在仿真步边界生成后移交给渲染线程。仅由 GPU 消费时可打开 stableGeometry，获得像素中心的几何通道。TAA 与几何真值同时启用时，为保证采样位置会额外录制一次无抖动的几何 pass。

| 通道 | 格式与语义 |
|---|---|
| Color | RGBA8 UNORM，字节值已转 sRGB。演示默认输出 Beauty，可切换为诊断图 |
| AlbedoMetal | RGBA8 UNORM，线性底色 RGB 和金属度 A |
| NormalRough | RGBA16F，世界法线 XYZ 和粗糙度 W |
| Motion | RGBA16F，XY 为上一帧 UV 减当前 UV，两个位置均在无抖动投影下计算。Z 为上一帧相机深度，W 标记存在匹配的旧实例 |
| MetricDepth / InstanceId | R32F 米制光轴深度 / R32_UINT 稳定标签。整数纹理使用最近点采样 |
| HardwareDepth / Hdr | D32F 硬件深度，范围 [0,1] / RGBA16F 光照结果 |

## 当前边界

主要验证范围为 Windows、NVIDIA RTX 3080 Ti 和不透明静态三角网格，支持实例运动。图像畸变、按曝光时间积分的运动模糊、透明材质、骨骼动画、HDRI 预滤波 IBL、DLSS 和多 GPU 尚待扩展。TAA 负责抗锯齿。glTF 导入器读取静态姿态和上述核心材质子集，贴图使用 UV0 和重复采样；暂未覆盖完整的 glTF 材质扩展与采样器语义。

材质、贴图及网格在发布到场景后按不可变资产使用，更新贴图时创建新材质引用。切换大型场景后可用 clearAssetCache() 切换缓存代际；仍被视图引用的旧资产延迟释放。首帧资产上传同步进行。后续多相机优化可以从批量上传、显存子分配和跨视图命令录制入手。

示例模型的作者、许可和标志说明见 [资产来源](example/assets/ATTRIBUTION.md)。
