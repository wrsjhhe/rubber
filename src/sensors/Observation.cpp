#include "sim/Observation.hpp"
#include "sim/Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <stdexcept>

namespace sim
{
namespace
{
void require(bool ok, const char* message)
{
    if (!ok)
        throw std::invalid_argument(message);
}
const ChannelSemantic& lookup(Channel channel)
{
    // 通道语义是公开契约：名字同时用于处理链的重名检查，改动会直接让派生输出无法声明。
    static const ChannelSemantic table[] = {
        {Channel::Color, "Color", PixelFormat::Rgba8Unorm, "sRGB 字节",
         "Beauty：曝光、色调映射与 sRGB 编码后的颜色；背景是天空", 0, false, false},
        {Channel::AlbedoMetal, "AlbedoMetal", PixelFormat::Rgba8Unorm, "线性底色 / 金属度",
         "G-buffer：RGB 线性底色，A 金属度", 0, false, true},
        {Channel::NormalRough, "NormalRough", PixelFormat::Rgba16Float, "世界法线 / 粗糙度",
         "G-buffer：XYZ 世界空间法线，W 粗糙度", 0, false, true},
        {Channel::Motion, "Motion", PixelFormat::Rgba16Float, "UV / 相机深度 / 匹配标记",
         "G-buffer：XY 旧 UV 减当前 UV（无抖动），Z 旧相机深度，W 旧实例匹配", 0, false, true},
        {Channel::MetricDepth, "MetricDepth", PixelFormat::R32Float, "米",
         "光轴正距离（非欧氏距离），与网格和点云同一公式 1/gl_FragCoord.w；背景为零", 0, false, true},
        {Channel::InstanceId, "InstanceId", PixelFormat::R32Uint, "稳定标签",
         "稳定非零标签，背景为零，禁止线性插值；点云占用最高位", 0, true, true},
        {Channel::HardwareDepth, "HardwareDepth", PixelFormat::R32Float, "NDC 深度 [0,1]",
         "深度附件，非线性且不能当米制深度使用", 1, true, false},
        {Channel::Hdr, "Hdr", PixelFormat::Rgba16Float, "线性辐亮度",
         "光照结果，尚未做时序解析、曝光与色调映射；背景是天空", 0, false, false},
        {Channel::Resolved, "Resolved", PixelFormat::Rgba16Float, "线性辐亮度",
         "时序解析后的线性颜色（TAA 结果），未经曝光与色调映射；背景是天空", 0, false, false},
        {Channel::Debug, "Debug", PixelFormat::Rgba8Unorm, "sRGB 字节",
         "调试呈现：法线、米制深度、实例标签或级联分区；与 Beauty 各自独立", 0, true, false},
    };
    for (const auto& entry : table)
        if (entry.channel == channel)
            return entry;
    throw std::invalid_argument("Unknown channel");
}
}
SensorCalibration SensorCalibration::of(const PerspectiveCamera& camera, uint32_t width, uint32_t height)
{
    require(width && height, "Invalid observation size");
    require(camera.nearPlane > 0 && camera.farPlane > camera.nearPlane, "Invalid camera frustum");
    SensorCalibration calibration;
    calibration.width = width;
    calibration.height = height;
    calibration.nearPlane = camera.nearPlane;
    calibration.farPlane = camera.farPlane;
    if (camera.fx != 0 || camera.fy != 0)
    {
        require(camera.fx > 0 && camera.fy > 0, "Invalid camera intrinsics");
        calibration.fx = camera.fx;
        calibration.fy = camera.fy;
        calibration.cx = camera.cx;
        calibration.cy = camera.cy;
    }
    else
    {
        // 与 PerspectiveCamera 的默认路径一致：垂直视场角与高度决定像素焦距，方形像素。
        require(camera.verticalFov > 0 && camera.verticalFov < 179, "Invalid camera field of view");
        calibration.fy = .5f * float(height) / std::tan(glm::radians(camera.verticalFov) * .5f);
        calibration.fx = calibration.fy;
        calibration.cx = .5f * float(width);
        calibration.cy = .5f * float(height);
    }
    return calibration;
}
glm::mat4 SensorCalibration::projection() const
{
    require(width && height && fx > 0 && fy > 0 && nearPlane > 0 && farPlane > nearPlane,
            "Invalid calibration");
    // 先按视场角构造，再用内参覆盖四个元素：与 PerspectiveCamera::projection 的
    // 内参分支逐位一致，近远平面项也来自同一次构造。
    auto m = glm::perspectiveRH_ZO(2 * std::atan(.5f * float(height) / fy), float(width) / float(height),
                                   nearPlane, farPlane);
    m[1][1] *= -1;
    m[0][0] = 2 * fx / float(width);
    m[1][1] = -2 * fy / float(height);
    m[2][0] = 1 - 2 * cx / float(width);
    m[2][1] = 1 - 2 * cy / float(height);
    return m;
}
Observation::Observation(std::shared_ptr<FrameResult> result, SensorCalibration calibration, SampleTime time,
                         std::vector<OutputInfo> outputs)
    : frame(std::move(result)), calib(calibration), sample(time), infos(std::move(outputs))
{
    require(frame != nullptr, "Observation requires a frame result");
    require(calib.width && calib.height && calib.fx > 0 && calib.fy > 0, "Invalid calibration");
    require(std::isfinite(sample.time), "Invalid sample time");
    require(std::isfinite(sample.duration) && sample.duration >= 0, "Invalid exposure duration");
    // 观测声明的是它真正采样过的时刻：与结果的帧时间不一致就拒绝，不做静默修正。
    require(sample.time == frame->time(), "Sample time does not match the frame");
}
double Observation::time() const
{
    return frame->time();
}
bool Observation::has(Channel channel) const
{
    for (const auto& info : infos)
        if (info.requested.channel == channel)
            return true;
    return false;
}
OutputData Observation::read(Channel channel) const
{
    if (!has(channel))
        throw std::invalid_argument("Channel was not requested in this observation");
    return frame->read(channel);
}
const ChannelSemantic& Observation::semantic(Channel channel)
{
    return lookup(channel);
}
ObservationResult Renderer::observe(const RenderRequest& request)
{
    SampleTime sample{request.scene.time, 0};
    if (request.sampleTime)
    {
        // 校验放在提交之前：否则一次非法观测会先占掉一个结果槽。
        sample = *request.sampleTime;
        require(std::isfinite(sample.time) && sample.time == request.scene.time,
                "Sample time does not match the snapshot time");
        require(std::isfinite(sample.duration) && sample.duration >= 0, "Invalid exposure duration");
    }
    auto submission = submit(request);
    if (submission.status != SubmitStatus::Submitted || !submission.frame)
        return {};
    auto calibration = SensorCalibration::of(request.camera, request.view.width(), request.view.height());
    return {SubmitStatus::Submitted,
            std::make_shared<Observation>(submission.frame, calibration, sample, submission.frame->outputs())};
}
}
