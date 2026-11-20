#pragma once
#include "Output.hpp"
#include "Scene.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace sim
{
// 传感器标定：像素内参与视锥范围。约定与 PerspectiveCamera 一致——fx/fy 以像素为单位，
// 主点以图像左上角为原点。projection() 由标定反算投影矩阵，渲染器实际使用的也是同一组
// 参数，因此"标定"和"出图"不是两份可以各自漂移的数据。
struct SensorCalibration
{
    uint32_t width = 0, height = 0;
    float fx = 0, fy = 0, cx = 0, cy = 0;
    float nearPlane = 0, farPlane = 1;
    // 相机给了内参就用内参，否则由垂直视场角与分辨率换算（方形像素）。
    static SensorCalibration of(const PerspectiveCamera&, uint32_t width, uint32_t height);
    // 与 PerspectiveCamera::projection 使用同一套构造路径：内参写入 P00/P11/P20/P21，
    // 近远平面写入其余元素。
    glm::mat4 projection() const;
    bool operator==(const SensorCalibration&) const = default;
};
// 采样语义：时刻必须等于快照时间（观测描述的正是那一帧）。duration 是曝光时长，0 表示
// 瞬时采样；当前渲染器按瞬时采样出图，曝光积分属于 F12，时间语义先作为观测元数据固定。
struct SampleTime
{
    double time = 0;
    double duration = 0;
    double end() const
    {
        return time + duration;
    }
};
// 通道语义：字节形式之外的"这个通道是什么"。groundTruth 为真的通道只描述场景本身，
// 不随曝光、抗锯齿抖动或调试呈现改变；nearest 为真的通道禁止线性插值。
struct ChannelSemantic
{
    Channel channel;
    const char* name;
    PixelFormat format;
    const char* unit;
    const char* definition;
    // 背景取值；不是常量时为零（由定义说明）。
    double background;
    bool nearest;
    bool groundTruth;
};
// 观测：一次提交的结果 + 标定 + 采样时间。结果本身只读，观测持有它，
// 因此元数据与像素在读回时始终属于同一次采样。
class Observation
{
  public:
    // 采样时刻与结果的帧时间不一致时拒绝构造：观测不能声明一个自己没采样的时刻。
    Observation(std::shared_ptr<FrameResult>, SensorCalibration, SampleTime, std::vector<OutputInfo>);
    const SensorCalibration& calibration() const
    {
        return calib;
    }
    const SampleTime& sampleTime() const
    {
        return sample;
    }
    double time() const;
    const std::vector<OutputInfo>& outputs() const
    {
        return infos;
    }
    bool has(Channel) const;
    // 与 FrameResult::read 一致：未请求的通道显式报错，不做静默补零。
    OutputData read(Channel) const;
    const std::shared_ptr<FrameResult>& result() const
    {
        return frame;
    }
    static const ChannelSemantic& semantic(Channel);

  private:
    std::shared_ptr<FrameResult> frame;
    SensorCalibration calib;
    SampleTime sample;
    std::vector<OutputInfo> infos;
};
struct ObservationResult
{
    SubmitStatus status = SubmitStatus::Busy;
    std::shared_ptr<Observation> observation;
};
}
