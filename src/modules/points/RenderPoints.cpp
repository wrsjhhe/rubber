#include "RenderPoints.hpp"
#include "../../backend/Device.hpp"
#include <stdexcept>
#include <vector>

namespace sim::vk
{
RenderPoints::RenderPoints(Device& d, const SceneSnapshot& snapshot, uint64_t version) : number(version)
{
    std::vector<PointGpu> points;
    size_t total = 0;
    for (auto& object : snapshot.points)
    {
        if (!object.cloud)
            throw std::invalid_argument("Invalid snapshot point cloud");
        total += object.cloud->points.size();
    }
    if (!total)
        return;
    if (total * sizeof(PointGpu) > d.properties.limits.maxStorageBufferRange)
        throw std::length_error("Point buffer exceeds device limit");
    points.reserve(total);
    for (auto& object : snapshot.points)
    {
        auto label = pointLabel(object.id);
        // 世界变换在准备阶段一次完成：同一点云被多个节点引用时按节点展开，
        // 相机侧不再需要变换矩阵，绘制只按世界坐标生成公告板。
        for (auto& p : object.cloud->points)
        {
            auto position = object.world * glm::vec4(p.position, 1);
            points.push_back({{position.x, position.y, position.z, p.radius},
                              {p.color, p.intensity},
                              {label, 0, 0, 0}});
        }
    }
    batch = std::make_unique<UploadBatch>(d);
    buffer = batch->upload(points.data(), points.size() * sizeof(PointGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    pointCount = uint32_t(points.size());
    cached = snapshot.points;
    uploadValue = batch->submit();
}
RenderPoints::~RenderPoints()
{
    if (batch && !batch->complete())
        batch->wait();
}
bool RenderPoints::matches(const SceneSnapshot& scene) const
{
    if (scene.points.size() != cached.size())
        return false;
    for (size_t i = 0; i < cached.size(); ++i)
        if (scene.points[i].id != cached[i].id || scene.points[i].cloud != cached[i].cloud ||
            scene.points[i].world != cached[i].world)
            return false;
    return true;
}
}
