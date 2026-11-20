// 点云模块验证：与网格共用同一张深度、强度与半径原样保留、双相机下前后遮挡互换、
// 深度与实例标签语义确定，并确认点云以普通模块接入调度（pass 清单只多不少）。
#include "sim/Renderer.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace sim;

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
// 点云实例标签用最高位与网格区分，低 31 位是点云节点的稳定 ID。
constexpr uint32_t labelBit = 0x8000'0000u;
uint32_t labelOf(uint32_t node)
{
    return labelBit | node;
}
uint32_t centerIndex(const Frame& frame)
{
    return (frame.height / 2) * frame.width + frame.width / 2;
}
float luminance(const Frame& frame, uint32_t index)
{
    auto* p = frame.rgba.data() + size_t(index) * 4;
    return (p[0] + p[1] + p[2]) / 3.0f;
}
uint32_t labelledPixels(const Frame& frame, uint32_t label)
{
    return uint32_t(std::count(frame.ids.begin(), frame.ids.end(), label));
}
// 单点云：一份共享资产同时被多个视图使用，测试据此确认它没有逐视图状态。
PointCloud onePoint(glm::vec3 position, float radius, float intensity, glm::vec3 color = glm::vec3(0.25f))
{
    std::vector<PointVertex> points{{position, intensity, color, radius}};
    return std::make_shared<BufferPointCloud>(points);
}
Frame renderReadback(Renderer& renderer, const SceneSnapshot& snapshot, const PerspectiveCamera& camera, View& view,
                     RenderOptions options = {})
{
    options.readback = true;
    renderer.render(snapshot, camera, view, options);
    return view.read();
}
int main(int argc, char** argv)
try
{
    DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    Renderer renderer(device);
    auto material = std::make_shared<MeshStandardMaterial>();

    // 资产的强度与半径必须原样保留：绘制阶段按世界单位换算出像素半径、按强度缩放辐亮度，
    // 两者都不该在构造时被归一化或丢弃。
    auto cloud = onePoint({0, 0, 1.5f}, 0.3f, 4.0f);
    expect(cloud->points.size() == 1, "Point cloud lost its point");
    expect(cloud->points[0].intensity == 4.0f, "Point cloud lost the intensity");
    expect(cloud->points[0].radius == 0.3f, "Point cloud lost the radius");
    expect(cloud->maxRadius == 0.3f, "Point cloud lost the maximum radius");
    // 包围盒按最大半径外扩：剔除与阴影范围都必须覆盖点的可见范围，而不只是中心。
    expect(glm::length(cloud->minimum - glm::vec3(-.3f, -.3f, 1.2f)) < 1e-5f, "Bounding box ignored the radius");
    expect(glm::length(cloud->maximum - glm::vec3(.3f, .3f, 1.8f)) < 1e-5f, "Bounding box ignored the radius");

    // 双相机共用同一份快照：点在前时由点写入深度与标签，从背面看时由网格写回，
    // 说明两者的遮挡关系完全来自同一张深度图，而不是任何模块自带的排序或裁剪。
    Scene scene;
    auto meshNode = scene.add(BoxGeometry(), material);
    auto pointNode = scene.addPoints(cloud);
    auto snapshot = scene.snapshot(1);
    auto front = renderer.createView(64, 48, 64);
    auto back = renderer.createView(64, 48, 64);
    PerspectiveCamera towardOrigin;
    towardOrigin.position = {0, 0, 5};
    PerspectiveCamera fromBehind;
    fromBehind.position = {0, 0, -5};

    auto nearFrame = renderReadback(renderer, snapshot, towardOrigin, *front);
    auto center = centerIndex(nearFrame);
    // 点位于 z=1.5，相机在 z=5：点比箱体正面（z=0.5）更近，必须由点占据该像素。
    expect(nearFrame.ids[center] == labelOf(pointNode), "Point in front did not own the pixel");
    expect(std::abs(nearFrame.depth[center] - 3.5f) < .02f, "Point in front kept the mesh depth");
    expect(front->visiblePoints() == 1, "Point cloud was not submitted");
    expect(front->visibleMeshes() == 1, "Mesh count changed by the point cloud");

    auto farFrame = renderReadback(renderer, snapshot, fromBehind, *back);
    // 从背面看时箱体反而更近：同一份点数据被深度测试正确遮挡，标签回落为网格 ID。
    expect(farFrame.ids[center] == meshNode, "Occluded point still wrote its label");
    expect((farFrame.ids[center] & labelBit) == 0, "Mesh label collided with the point label bit");
    expect(std::abs(farFrame.depth[center] - 4.5f) < .02f, "Occluded point still wrote its depth");
    expect(back->visiblePoints() == 1, "Point cloud was culled by the CPU");

    // 点云不依赖网格存在：调度只按模块分发，没有"必须先有三角网格"的分支。
    Scene pointsOnly;
    auto soloNode = pointsOnly.addPoints(onePoint({0, 0, 0}, 0.3f, 2.0f));
    auto solo = pointsOnly.snapshot(2);
    auto soloView = renderer.createView(64, 48, 64);
    auto soloFrame = renderReadback(renderer, solo, towardOrigin, *soloView);
    expect(soloView->visibleMeshes() == 0 && soloView->visiblePoints() == 1, "Points-only scene mis-dispatched");
    expect(soloFrame.ids[centerIndex(soloFrame)] == labelOf(soloNode), "Points-only scene lost the label");
    expect(std::abs(soloFrame.depth[centerIndex(soloFrame)] - 5) < .02f, "Points-only scene lost the depth");

    // 点云作为普通模块接入：活跃清单恰好比同一场景去掉点云时多出两个 pass，
    // 位置也符合"光照之后、时序解析之前"和"与稳定几何同段"的约定。
    Scene meshOnly;
    meshOnly.add(BoxGeometry(), material);
    auto bare = renderer.createView(64, 48, 64);
    renderReadback(renderer, meshOnly.snapshot(3), towardOrigin, *bare);
    auto barePasses = bare->activePasses();
    auto full = renderer.createView(64, 48, 64);
    renderReadback(renderer, scene.snapshot(4), towardOrigin, *full);
    auto fullPasses = full->activePasses();
    auto indexOf = [](const std::vector<std::string>& names, const std::string& name)
    {
        auto it = std::find(names.begin(), names.end(), name);
        return it == names.end() ? names.size() : size_t(it - names.begin());
    };
    expect(indexOf(fullPasses, "points") < fullPasses.size(), "Point pass was not executed");
    expect(indexOf(fullPasses, "points.stable") < fullPasses.size(), "Stable point pass was not executed");
    auto stripped = fullPasses;
    std::erase(stripped, "points");
    std::erase(stripped, "points.stable");
    expect(stripped == barePasses, "Point cloud changed the mesh pass list");
    expect(indexOf(fullPasses, "points") > indexOf(fullPasses, "lighting"), "Point pass ran before lighting");
    expect(indexOf(fullPasses, "points") < indexOf(fullPasses, "temporal"), "Point pass ran after resolve");
    expect(indexOf(fullPasses, "points.stable") > indexOf(fullPasses, "geometry.stable"),
           "Stable point pass ran before the stable geometry");

    // 半径以世界单位保留：像素覆盖随分辨率平方增长，同一份数据在不同分辨率下保持物理尺寸。
    auto small = renderer.createView(64, 48, 64);
    auto large = renderer.createView(128, 96, 64);
    RenderOptions flat;
    flat.taa = false;
    flat.shadows = false;
    Scene wideScene;
    auto wide = wideScene.addPoints(onePoint({0, 0, 0}, 0.6f, 1.0f));
    auto wideSnapshot = wideScene.snapshot(5);
    auto smallFrame = renderReadback(renderer, wideSnapshot, towardOrigin, *small, flat);
    auto largeFrame = renderReadback(renderer, wideSnapshot, towardOrigin, *large, flat);
    auto smallArea = float(labelledPixels(smallFrame, labelOf(wide)));
    auto largeArea = float(labelledPixels(largeFrame, labelOf(wide)));
    expect(smallArea >= 1 && largeArea >= 1, "Point radius collapsed below a pixel");
    auto ratio = largeArea / smallArea;
    expect(ratio > 3.2f && ratio < 4.8f, "Pixel coverage did not follow the resolution");

    // 强度只缩放辐亮度，半径只决定覆盖：同一份几何下改强度会变亮而面积不变。
    Scene dim;
    dim.addPoints(onePoint({0, 0, 0}, 0.6f, 1.0f));
    Scene bright;
    bright.addPoints(onePoint({0, 0, 0}, 0.6f, 4.0f));
    auto dimView = renderer.createView(64, 48, 64);
    auto brightView = renderer.createView(64, 48, 64);
    auto dimFrame = renderReadback(renderer, dim.snapshot(6), towardOrigin, *dimView, flat);
    auto brightFrame = renderReadback(renderer, bright.snapshot(7), towardOrigin, *brightView, flat);
    expect(luminance(brightFrame, centerIndex(brightFrame)) > luminance(dimFrame, centerIndex(dimFrame)) + 8,
           "Intensity did not scale the radiance");
    expect(labelledPixels(dimFrame, labelOf(1)) == labelledPixels(brightFrame, labelOf(1)),
           "Intensity changed the footprint");

    // 半径变化必须改变覆盖面积，且覆盖面积始终由世界半径决定。
    Scene narrow;
    narrow.addPoints(onePoint({0, 0, 0}, 0.15f, 1.0f));
    auto narrowView = renderer.createView(64, 48, 64);
    auto narrowFrame = renderReadback(renderer, narrow.snapshot(8), towardOrigin, *narrowView, flat);
    expect(labelledPixels(narrowFrame, labelOf(1)) * 4 < labelledPixels(dimFrame, labelOf(1)),
           "Radius did not change the footprint");

    std::cout << "PASS: point clouds share the mesh depth buffer and keep intensity and radius\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
