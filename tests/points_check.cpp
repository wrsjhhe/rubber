// 点云模块验证：与网格共用同一张深度、强度与半径原样保留、双相机下前后遮挡互换、
// 深度与实例标签语义确定，并确认点云以普通模块接入调度（pass 清单只多不少）。
// 另覆盖点尺寸只依赖纯投影（俯仰/翻滚/双轴内参）、静止收敛后点云变化使 TAA 历史失效，
// 以及历史状态的提交条件（被裁掉时序解析的仅 Hdr 帧不推进颜色历史，网格与点云同理）。
#include "sim/Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
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
// 标签像素的包围盒：用于确认点足迹的宽高比，而不是只比面积。
struct Footprint
{
    uint32_t left = 0, right = 0, top = 0, bottom = 0, width = 0, height = 0;
};
Footprint footprint(const Frame& frame, uint32_t label)
{
    Footprint box;
    bool any = false;
    box.left = frame.width;
    box.top = frame.height;
    for (uint32_t y = 0; y < frame.height; ++y)
        for (uint32_t x = 0; x < frame.width; ++x)
            if (frame.ids[size_t(y) * frame.width + x] == label)
            {
                any = true;
                box.left = std::min(box.left, x);
                box.right = std::max(box.right, x);
                box.top = std::min(box.top, y);
                box.bottom = std::max(box.bottom, y);
            }
    if (!any)
        return {};
    box.width = box.right - box.left + 1;
    box.height = box.bottom - box.top + 1;
    return box;
}
// 红减蓝：点用高饱和红，天空偏蓝，这个差值能稳定区分"点在不在"。
float redness(const Frame& frame, uint32_t index)
{
    auto* p = frame.rgba.data() + size_t(index) * 4;
    return float(p[0]) - float(p[2]);
}
// 窗口内最红的像素：点移动后的位置允许几个像素的取整误差。
float brightestRedNear(const Frame& frame, uint32_t x, uint32_t y, uint32_t radius)
{
    float best = -255;
    for (uint32_t row = y - std::min(y, radius); row <= std::min(frame.height - 1, y + radius); ++row)
        for (uint32_t column = x - std::min(x, radius); column <= std::min(frame.width - 1, x + radius); ++column)
            best = std::max(best, redness(frame, row * frame.width + column));
    return best;
}
bool closeEnough(float actual, float expected, float tolerance)
{
    return std::abs(actual - expected) <= tolerance * std::max(actual, expected);
}
bool hasPass(const std::vector<std::string>& passes, const char* name)
{
    return std::find(passes.begin(), passes.end(), name) != passes.end();
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

    // 点尺寸只由纯投影决定：同一距离下俯仰与翻滚都不该改变覆盖面积。
    // 用 vp 的元素当焦距会把这些姿态混进半径，俯仰 45 度面积减半、翻滚 60 度再减半。
    Scene poseScene;
    auto poseNode = poseScene.addPoints(onePoint({0, 0, 0}, 0.5f, 1.0f));
    auto poseSnapshot = poseScene.snapshot(9);
    PerspectiveCamera straight;
    straight.position = {0, 0, 5};
    straight.verticalFov = 60;
    PerspectiveCamera pitched = straight;
    auto pitch = glm::radians(45.f);
    pitched.position = {0, 5 * std::sin(pitch), 5 * std::cos(pitch)};
    PerspectiveCamera rolled = straight;
    auto roll = glm::radians(60.f);
    rolled.up = {std::sin(roll), std::cos(roll), 0};
    auto poseView = renderer.createView(96, 72, 64);
    auto straightFrame = renderReadback(renderer, poseSnapshot, straight, *poseView, flat);
    auto pitchView = renderer.createView(96, 72, 64);
    auto pitchedFrame = renderReadback(renderer, poseSnapshot, pitched, *pitchView, flat);
    auto rollView = renderer.createView(96, 72, 64);
    auto rolledFrame = renderReadback(renderer, poseSnapshot, rolled, *rollView, flat);
    auto straightArea = float(labelledPixels(straightFrame, labelOf(poseNode)));
    expect(straightArea > 20, "Reference point was too small to measure");
    expect(closeEnough(float(labelledPixels(pitchedFrame, labelOf(poseNode))), straightArea, .1f),
           "Pitch changed the point size");
    expect(closeEnough(float(labelledPixels(rolledFrame, labelOf(poseNode))), straightArea, .1f),
           "Roll changed the point size");

    // 两轴内参不同时，足迹必须是椭圆而不是正方形：分别换算 x/y 焦距才谈得上标定相机。
    PerspectiveCamera calibrated;
    calibrated.position = {0, 0, 5};
    calibrated.fx = 240;
    calibrated.fy = 160;
    calibrated.cx = 64;
    calibrated.cy = 48;
    auto calibratedView = renderer.createView(128, 96, 64);
    auto calibratedFrame = renderReadback(renderer, poseSnapshot, calibrated, *calibratedView, flat);
    auto box = footprint(calibratedFrame, labelOf(poseNode));
    expect(box.width > 0 && box.height > 0, "Calibrated camera lost the point");
    expect(float(box.width) > float(box.height) * 1.3f, "Point footprint ignored the per-axis focal lengths");

    // 静止收敛后点云变化必须让 TAA 历史失效：静止分支按像素平均历史，不看运动匹配，
    // 光把 Motion 写零并不能阻止旧点云被一直复用。点用高饱和红，背景是偏蓝的天空，
    // 因此"红减蓝"能稳定区分点在不在，而不受天空梯度影响。
    auto signalPoint = [](float intensity)
    {
        return onePoint({0, 0, 0}, 0.5f, intensity, {1.f, .1f, .1f});
    };
    Scene still;
    auto stillNode = still.addPoints(signalPoint(4));
    auto stillA = still.snapshot(10);
    PerspectiveCamera staticCamera;
    staticCamera.position = {0, 0, 5};
    auto historyView = renderer.createView(64, 48, 64);
    Frame converged;
    for (int i = 0; i < 18; ++i)
        converged = renderReadback(renderer, stillA, staticCamera, *historyView);
    expect(historyView->stationary(), "Static point cloud never converged");
    auto referenceRed = redness(converged, centerIndex(converged));
    auto referenceBright = luminance(converged, centerIndex(converged));
    auto backdropRed = redness(converged, 4 + 4 * converged.width);
    expect(referenceRed > backdropRed + 40, "Point did not stand out from the background");

    // 移动：中心必须回落到背景，新位置必须出现点。
    still.get(stillNode).local = glm::translate(glm::mat4(1), {2, 0, 0});
    auto stillB = still.snapshot(11);
    auto afterMove = renderReadback(renderer, stillB, staticCamera, *historyView);
    expect(!historyView->stationary(), "Moved point cloud was treated as stationary");
    expect(redness(afterMove, centerIndex(afterMove)) < backdropRed + 20,
           "TAA kept the old point after it moved");
    expect(brightestRedNear(afterMove, afterMove.width / 2 + 21, afterMove.height / 2, 4) >
               referenceRed * .8f,
           "TAA lost the point at its new position");

    // 删除：整体移除后旧画面不能留在历史里。
    still.remove(stillNode);
    auto stillC = still.snapshot(12);
    auto afterDelete = renderReadback(renderer, stillC, staticCamera, *historyView);
    expect(!historyView->stationary(), "Removed point cloud was treated as stationary");
    expect(redness(afterDelete, centerIndex(afterDelete)) < backdropRed + 20,
           "TAA kept a deleted point cloud");

    // 两台相机各有自己的历史基准：A 先看到变化不能让 B 误判为静止，否则 B 会一直显示旧点云。
    Scene pairScene;
    auto pairNode = pairScene.addPoints(signalPoint(4));
    auto pairA = pairScene.snapshot(20);
    auto left = renderer.createView(64, 48, 64);
    auto right = renderer.createView(64, 48, 64);
    for (int i = 0; i < 18; ++i)
        renderReadback(renderer, pairA, staticCamera, *left);
    for (int i = 0; i < 18; ++i)
        renderReadback(renderer, pairA, staticCamera, *right);
    expect(left->stationary() && right->stationary(), "Two-camera convergence failed");
    auto shared = renderReadback(renderer, pairA, staticCamera, *left);
    auto sharedBright = luminance(shared, centerIndex(shared));
    pairScene.get(pairNode).local = glm::translate(glm::mat4(1), {2, 0, 0});
    auto pairB = pairScene.snapshot(21);
    renderReadback(renderer, pairB, staticCamera, *left);
    auto rightFrame = renderReadback(renderer, pairB, staticCamera, *right);
    expect(!right->stationary(), "Second camera reused the first camera's history baseline");
    expect(redness(rightFrame, centerIndex(rightFrame)) < backdropRed + 20,
           "Second camera kept the stale point");

    // 强度变化也是内容变化：静止分支会一直保持旧亮度，必须重新解析。
    pairScene.get(pairNode).local = glm::mat4(1);
    pairScene.get(pairNode).cloud = signalPoint(1);
    auto pairC = pairScene.snapshot(22);
    auto dimmer = renderReadback(renderer, pairC, staticCamera, *right);
    expect(!right->stationary(), "Changed point intensity was treated as stationary");
    expect(luminance(dimmer, centerIndex(dimmer)) < sharedBright * .85f,
           "TAA kept the old intensity after the asset changed");

    // 换色：颜色与强度同在一份共享缓冲里，静止历史同样必须失效。
    pairScene.get(pairNode).cloud = onePoint({0, 0, 0}, 0.5f, 4.0f, {.1f, 1.f, .1f});
    auto pairD = pairScene.snapshot(23);
    auto recolored = renderReadback(renderer, pairD, staticCamera, *right);
    expect(!right->stationary(), "Changed point colour was treated as stationary");
    auto* pixel = recolored.rgba.data() + size_t(centerIndex(recolored)) * 4;
    expect(pixel[1] > pixel[0] + 40, "TAA kept the old colour after the asset changed");

    // 几何真值交付不推进颜色历史基准：几何帧里点云已变化时，下一帧颜色仍须刷新。
    Scene geometryOnly;
    auto geometryNode = geometryOnly.addPoints(signalPoint(4));
    auto geometryA = geometryOnly.snapshot(30);
    auto geometryView = renderer.createView(64, 48, 64);
    Frame geometryConverged;
    for (int i = 0; i < 18; ++i)
        geometryConverged = renderReadback(renderer, geometryA, staticCamera, *geometryView);
    auto geometryRed = redness(geometryConverged, centerIndex(geometryConverged));
    geometryOnly.get(geometryNode).local = glm::translate(glm::mat4(1), {2, 0, 0});
    auto geometryB = geometryOnly.snapshot(31);
    auto only = renderer.submit(
        {geometryB, staticCamera, *geometryView, {{Channel::InstanceId, Delivery::Cpu}}});
    expect(only.status == SubmitStatus::Submitted, "Geometry-only submit was rejected");
    only.frame->read(Channel::InstanceId);
    only.frame.reset();
    auto refreshed = renderReadback(renderer, geometryB, staticCamera, *geometryView);
    expect(!geometryView->stationary(), "Geometry-only frame advanced the color history baseline");
    expect(redness(refreshed, centerIndex(refreshed)) < backdropRed + 20,
           "Color history survived a geometry-only frame");
    expect(geometryRed > backdropRed + 40, "Geometry-only reference frame was invalid");

    // 仅导出 Hdr 的帧同样不得推进颜色历史：它没有 color 的导出根，未启用 TAA 时也没有
    // 副作用根，时序解析会被裁掉，颜色历史仍是旧内容。基线若提前推进，下一帧要么误判
    // 静止而冻结旧图，要么平白丢掉已收敛的历史。
    Scene hdrScene;
    auto hdrNode = hdrScene.addPoints(signalPoint(4));
    auto hdrSnapshot = hdrScene.snapshot(40);
    auto hdrView = renderer.createView(64, 48, 64);
    Frame hdrConverged;
    for (int i = 0; i < 18; ++i)
        hdrConverged = renderReadback(renderer, hdrSnapshot, staticCamera, *hdrView);
    expect(hdrView->stationary(), "Hdr reference frame never converged");
    auto hdrBackdrop = redness(hdrConverged, 4 + 4 * hdrConverged.width);
    auto hdrRed = redness(hdrConverged, centerIndex(hdrConverged));

    // 内容没变时只请求 Hdr：这一帧不写颜色历史，随后仍应判为静止（历史里正是这份内容）。
    RenderOptions noTaa;
    noTaa.taa = false;
    auto hdrSame = renderer.submit({hdrSnapshot, staticCamera, *hdrView, {{Channel::Hdr, Delivery::Gpu}}, noTaa});
    expect(hdrSame.status == SubmitStatus::Submitted, "Hdr-only submit was rejected");
    expect(!hasPass(hdrView->activePasses(), "temporal"),
           "Hdr-only frame did not prune the resolve pass, this case is not covered");
    expect(!hdrView->stationary(), "A frame that never wrote history reported itself stationary");
    hdrSame.frame.reset();
    auto afterHdr = renderReadback(renderer, hdrSnapshot, staticCamera, *hdrView);
    expect(hdrView->stationary(), "Hdr-only frame disturbed the color history");
    expect(redness(afterHdr, centerIndex(afterHdr)) > hdrBackdrop + 40, "Resumed history lost the point");

    // 内容真的变化后只请求 Hdr：历史仍是旧内容，随后的 Color 必须刷新而不能复用旧画面。
    hdrScene.get(hdrNode).local = glm::translate(glm::mat4(1), {2, 0, 0});
    auto hdrMoved = hdrScene.snapshot(41);
    auto hdrOnly = renderer.submit({hdrMoved, staticCamera, *hdrView, {{Channel::Hdr, Delivery::Gpu}}, noTaa});
    expect(hdrOnly.status == SubmitStatus::Submitted, "Hdr-only submit was rejected");
    expect(!hasPass(hdrView->activePasses(), "temporal"), "Hdr-only frame ran the resolve pass");
    hdrOnly.frame.reset();
    auto hdrRefreshed = renderReadback(renderer, hdrMoved, staticCamera, *hdrView);
    expect(!hdrView->stationary(), "Hdr-only frame advanced the color history baseline");
    expect(redness(hdrRefreshed, centerIndex(hdrRefreshed)) < hdrBackdrop + 20,
           "Color reused the history that the Hdr frame never wrote");
    expect(brightestRedNear(hdrRefreshed, hdrRefreshed.width / 2 + 21, hdrRefreshed.height / 2, 4) > hdrRed * .8f,
           "Color did not show the moved point");

    // 连续穿插 Hdr-only 帧不该扰动颜色链路：内容与历史都没变，颜色帧仍静止、画面不变。
    auto settledRed = redness(hdrRefreshed, centerIndex(hdrRefreshed));
    for (int i = 0; i < 3; ++i)
    {
        auto extra = renderer.submit({hdrMoved, staticCamera, *hdrView, {{Channel::Hdr, Delivery::Gpu}}, noTaa});
        expect(extra.status == SubmitStatus::Submitted, "Interleaved Hdr-only submit was rejected");
        extra.frame.reset();
        auto colour = renderReadback(renderer, hdrMoved, staticCamera, *hdrView);
        expect(hdrView->stationary(), "Interleaved Hdr-only frame disturbed the color history");
        expect(std::abs(redness(colour, centerIndex(colour)) - settledRed) < 10,
               "Interleaved Hdr-only frame changed the colour output");
    }
    // 启用 TAA 的 Hdr 请求因历史写回副作用保留时序解析，此时历史确实被写入，属于正常路径。
    auto taaHdr = renderer.submit({hdrMoved, staticCamera, *hdrView, {{Channel::Hdr, Delivery::Gpu}}});
    expect(taaHdr.status == SubmitStatus::Submitted, "Taa Hdr-only submit was rejected");
    expect(hasPass(hdrView->activePasses(), "temporal"), "Taa Hdr-only frame pruned the history side effect");
    taaHdr.frame.reset();

    // 网格侧同样如此：姿态基准只在真正写入颜色历史后提交，否则 Hdr 帧会让下一颜色帧
    // 把新姿态当成"未变"而冻结旧画面。
    Scene mover;
    auto moverNode = mover.add(BoxGeometry(), material);
    auto moverStill = mover.snapshot(50);
    auto moverView = renderer.createView(64, 48, 64);
    Frame moverConverged;
    for (int i = 0; i < 18; ++i)
        moverConverged = renderReadback(renderer, moverStill, staticCamera, *moverView);
    expect(moverView->stationary(), "Mesh reference frame never converged");
    auto meshCenter = luminance(moverConverged, centerIndex(moverConverged));
    mover.get(moverNode).local = glm::translate(glm::mat4(1), {2, 0, 0});
    auto moverMoved = mover.snapshot(51);
    auto meshHdr = renderer.submit({moverMoved, staticCamera, *moverView, {{Channel::Hdr, Delivery::Gpu}}, noTaa});
    expect(meshHdr.status == SubmitStatus::Submitted, "Mesh Hdr-only submit was rejected");
    meshHdr.frame.reset();
    auto movedFrame = renderReadback(renderer, moverMoved, staticCamera, *moverView);
    expect(!moverView->stationary(), "Hdr-only frame advanced the mesh history baseline");
    expect(movedFrame.ids[centerIndex(movedFrame)] == 0, "Mesh did not leave the centre");
    expect(std::abs(luminance(movedFrame, centerIndex(movedFrame)) - meshCenter) > 10,
           "TAA froze the old mesh after a Hdr-only frame");

    std::cout << "PASS: point clouds share the mesh depth buffer and keep intensity and radius\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
