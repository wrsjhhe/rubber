#pragma once
#include "../../render/Pipeline.hpp"
#include "sim/Scene.hpp"
#include <glm/glm.hpp>

// 点云的共享 GPU 数据：一个快照版本一份，位置已在准备阶段变换到世界空间，
// 因此同一份数据可被任意数量的相机复用，绘制时不再需要逐相机上传。
namespace sim::vk
{
// 与 points.vert 中的 Point 布局一致。
struct PointGpu
{
    glm::vec4 positionRadius;
    glm::vec4 colorIntensity;
    glm::uvec4 label;
};
static_assert(sizeof(PointGpu) == 48);
// 点云实例标签与网格实例标签使用互不重叠的取值空间：最高位为 1 表示点云。
// 消费者因此可用 InstanceId 判断像素属于网格还是点云，而不必额外通道。
constexpr uint32_t pointLabelBit = 0x8000'0000u;
constexpr uint32_t pointLabel(uint32_t objectId)
{
    return pointLabelBit | (objectId & 0x7FFF'FFFFu);
}
class RenderPoints
{
  public:
    RenderPoints(Device&, const SceneSnapshot&, uint64_t version);
    ~RenderPoints();
    const Buf& data() const
    {
        return buffer;
    }
    uint32_t count() const
    {
        return pointCount;
    }
    // 首次上传完成的时间线值，零表示没有待完成上传。
    uint64_t uploads() const
    {
        return uploadValue;
    }
    uint64_t version() const
    {
        return number;
    }
    bool matches(const SceneSnapshot&) const;

  private:
    uint64_t number = 0, uploadValue = 0;
    uint32_t pointCount = 0;
    // 快照里的点云引用与变换，用于判断键命中的缓存是否仍与内容一致。
    std::vector<PointObject> cached;
    Buf buffer;
    // 暂存副本必须活到复制完成，否则设备侧拷贝会读到已释放的内存。
    std::unique_ptr<UploadBatch> batch;
};
}
