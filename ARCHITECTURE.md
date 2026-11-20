# 仿真渲染器的早期实现说明

本文保留早期实现背景，不作为当前迁移规格。目标设计见 [框架架构](docs/ARCHITECTURE.md)，实际进度见 [PROGRESS](docs/PROGRESS.md)。

引擎以“给定场景快照和相机，生成可解释的观测”为入口。窗口只在 example 中出现。尺寸以米计，世界右手、Y 向上、相机沿局部 −Z 观察，图像原点在左上。深度输出为相机光轴方向的正距离，实例编号零和深度零表示背景。

## 借鉴与取舍

[three.js BufferGeometry](https://threejs.org/docs/pages/BufferGeometry.html) 把几何数据与使用它的对象分开，基本体生成器输出统一顶点和索引。这里采用固定的 position/normal/uv 布局和共享不可变资产，实例持有自己的变换和稳定编号。这样同一段塔身网格可以被多次引用和一次上传。[MeshStandardMaterial](https://threejs.org/docs/pages/MeshStandardMaterial.html) 的金属度、粗糙度和纹理语义适合与建模工具交换。颜色纹理解码 sRGB，法线和金属粗糙度按线性数据采样。

[threepp 多视图接口](https://github.com/markaren/threepp/blob/master/include/threepp/renderers/VulkanRenderer.hpp#L209-L229) 将世界资源与逐视图历史分开。这里每个 View 拥有独立分辨率、相机常量、阴影覆盖、历史图和完成信号，同一 Renderer 的视图共享几何、纹理及管线。场景快照只复制实例的状态和不可变资源引用。调用方可将同一快照提交给多个持久视图。

[UE Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) 让 pass 表达资源用途，再交给公共机制完成资源转换。当前只有固定的几个 pass，使用有序录制和统一图像状态转换。新增 SSAO 或超分辨率时应沿用输入、输出和历史资源边界。待 pass 有多种可选分支、显存复用需求时，再引入图编译、生命周期分析和别名分配。

## 模块与帧生命周期

1. Scene 接收可共享的 Geometry 和 Material。Object3D 节点以稳定 ID 挂接到父节点，节点持有局部变换，删除父节点会删除整个子树。snapshot() 固化世界矩阵、灯光与时间。未来 GDS 适配层只需要产生相同的快照。
2. Renderer 管理设备、资源上传、采样器和图形管线。View 先等待自己上一帧释放可重用资源，再上传当前相机和实例常量。每个提交留有 fence，调用方可轮询。
3. Shadow pass 为方向光拟合四段相机视锥，在深度图集的四个区域画入投影范围内的投影物体。即使物体在相机背后，它仍可能投影到视野中。
4. Geometry pass 输出底色/金属度、世界法线/粗糙度、重投影数据、米制深度、实例 ID 和硬件深度。CPU 依据局部包围盒和当前裁剪矩阵筛选实例。
5. Lighting pass 在屏幕空间重建位置，以微表面 BRDF 计算直接光照，得到线性 HDR。Resolve pass 重投影历史帧、裁剪邻域、混合、色调映射，产生独立的颜色图。TAA 历史按视图和实例编号追踪。
6. 预览可直接采样 GPU 颜色图，传感器按需录制读回。几何真值在像素中心采样，颜色可以使用带抖动投影。镜头畸变、噪声和曝光模型可以在这些数据的基础上扩展。

## 扩展边界

CPU 场景数据可以交给新的导入器；几何一律通过同一索引绘制路径。DLSS 需要额外 SDK、运动向量约定、曝光和历史重置协议。DLSS 作为后续扩展。多相机目前在一条图形队列上异步提交；批量录制、跨视图阴影复用和显存子分配可在性能测量后加入。
