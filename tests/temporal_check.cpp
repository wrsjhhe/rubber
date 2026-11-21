// 时序解析回归：静止收敛、运动轮廓质量与真值稳定性。
// 度量使用 Resolved（时序解析后的线性颜色）：Beauty 经过曝光与色调映射，不再是覆盖率的
// 线性函数；调试图自 M5 起独立于颜色链路，也不再享有 TAA 累积。
// 误差只统计参考块本身不均匀的像素，即真正的轮廓像素；整块同色的像素对二者都没有
// 抗锯齿信息，其差异只来自历史槽的半浮点累积舍入。
#include "sim/Observation.hpp"
#include "sim/Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
constexpr int width = 180;
constexpr int height = 120;
constexpr int scale = 4;
constexpr sim::Channel resolvedChannel = sim::Channel::Resolved;

float halfToFloat(uint16_t bits)
{
    uint32_t sign = uint32_t(bits & 0x8000u) << 16;
    uint32_t exponent = (bits >> 10) & 0x1fu;
    uint32_t mantissa = bits & 0x3ffu;
    uint32_t result;
    if (exponent == 0)
    {
        if (!mantissa)
            result = sign;
        else
        {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400u))
            {
                mantissa <<= 1;
                --exponent;
            }
            result = sign | (exponent << 23) | ((mantissa & 0x3ffu) << 13);
        }
    }
    else if (exponent == 31)
        result = sign | 0x7f800000u | (mantissa << 13);
    else
        result = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &result, 4);
    return value;
}
// 线性红通道：物体正面是平面且材质单一，覆盖率的差异在这个通道上直接可测。
double linearRed(const sim::OutputData& image, int x, int y)
{
    uint16_t bits;
    std::memcpy(&bits, image.bytes.data() + size_t(y * image.info.width + x) * 8, 2);
    return halfToFloat(bits);
}
// 参考图的子像素平均值直接反映边缘覆盖率。返回轮廓像素上的绝对误差之和。
double referenceError(const sim::OutputData& image, const sim::OutputData& reference, int* counted = nullptr)
{
    double error = 0;
    int pixels = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            double sum = 0, lo = 1e30, hi = -1e30;
            for (int j = 0; j < scale; ++j)
                for (int i = 0; i < scale; ++i)
                {
                    double value = linearRed(reference, x * scale + i, y * scale + j);
                    sum += value;
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
            if (hi - lo <= 0.02)
                continue;
            error += std::abs(linearRed(image, x, y) - sum / (scale * scale));
            ++pixels;
        }
    }
    if (counted)
        *counted = pixels;
    return error;
}
const std::vector<sim::OutputDesc>& channels()
{
    static const std::vector<sim::OutputDesc> all{{sim::Channel::Resolved, sim::Delivery::Cpu},
                                                  {sim::Channel::MetricDepth, sim::Delivery::Cpu},
                                                  {sim::Channel::InstanceId, sim::Delivery::Cpu}};
    return all;
}
sim::OutputData readResolved(const sim::Observation& observation)
{
    return observation.read(resolvedChannel);
}
bool sameColor(const sim::OutputData& a, const sim::OutputData& b)
{
    for (size_t i = 0; i < a.bytes.size(); i += 8)
        if (std::memcmp(a.bytes.data() + i, b.bytes.data() + i, 6))
            return false;
    return true;
}
}
int main(int argc, char** argv)
try
{
    sim::DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    sim::Renderer renderer(device);
    auto view = renderer.createView(width, height, 64);
    auto referenceView = renderer.createView(width * scale, height * scale, 64);
    sim::PerspectiveCamera camera;
    camera.position = {0, 0, 4};

    sim::Scene scene;
    auto mesh = scene.add(sim::BoxGeometry(2.7f, 0.17f, 0.1f), std::make_shared<sim::MeshStandardMaterial>(),
                          glm::rotate(glm::mat4(1), 0.39f, {0, 0, 1}));
    auto snapshot = scene.snapshot(0);
    sim::RenderOptions raw;
    raw.shadows = raw.taa = false;

    sim::OutputData reference, aliased, aliasedDepth, aliasedIds;
    {
        auto high = renderer.observe({snapshot, camera, *referenceView, channels(), raw});
        reference = readResolved(*high.observation);
        auto single = renderer.observe({snapshot, camera, *view, channels(), raw});
        aliased = readResolved(*single.observation);
        aliasedDepth = single.observation->read(sim::Channel::MetricDepth);
        aliasedIds = single.observation->read(sim::Channel::InstanceId);
    }

    auto taa = raw;
    taa.taa = true;
    sim::OutputData converged, convergedDepth, convergedIds;
    for (int i = 0; i < 34; ++i)
    {
        snapshot.time = i / 60.0;
        auto frame = renderer.observe({snapshot, camera, *view, channels(), taa});
        auto current = readResolved(*frame.observation);
        if (i == 15)
        {
            converged = current;
            convergedDepth = frame.observation->read(sim::Channel::MetricDepth);
            convergedIds = frame.observation->read(sim::Channel::InstanceId);
        }
        // 只比较 RGB：Resolved 的 A 分量携带该帧的米制深度（时序拒绝依赖它），
        // 它在轮廓像素上按当前抖动位置取值，本来就会变。
        if (i > 15 && !sameColor(current, converged))
            throw std::runtime_error("Stationary silhouette still flickers");
    }
    int silhouette = 0;
    double rawError = referenceError(aliased, reference, &silhouette);
    double taaError = referenceError(converged, reference);
    std::cout << "Static absolute error on " << silhouette << " silhouette pixels: raw=" << rawError
              << ", TAA=" << taaError << '\n';
    if (silhouette < 100)
        throw std::runtime_error("Reference has too few silhouette pixels");
    if (rawError <= 0 || taaError >= rawError * 0.45)
        throw std::runtime_error("Insufficient subpixel coverage on the diagonal edge");
    if (aliasedIds.bytes != convergedIds.bytes || aliasedDepth.bytes != convergedDepth.bytes)
        throw std::runtime_error("Temporal filtering changed sensor ground truth");

    // 平移相机后对照同一时刻的独立高分辨率视图，避免通过冻结历史掩盖运动或产生长拖尾。
    double movingRaw = 0, movingTaa = 0;
    auto singleView = renderer.createView(width, height, 64);
    for (int i = 0; i < 12; ++i)
    {
        camera.position.y += 0.038f;
        camera.target.y += 0.038f;
        snapshot.time += 1.0 / 60;
        auto moving = renderer.observe({snapshot, camera, *view, channels(), taa});
        auto referenceFrame = renderer.observe({snapshot, camera, *referenceView, channels(), raw});
        auto referenceImage = readResolved(*referenceFrame.observation);
        movingTaa += referenceError(readResolved(*moving.observation), referenceImage);
        // 用参考视图投影到像素中心的独立单样本绘制作为比较基准。
        auto alone = renderer.observe({snapshot, camera, *singleView, channels(), raw});
        movingRaw += referenceError(readResolved(*alone.observation), referenceImage);
    }
    std::cout << "Moving silhouette error: raw=" << movingRaw << ", TAA=" << movingTaa << '\n';
    if (movingTaa >= movingRaw)
        throw std::runtime_error("Moving edge quality regressed");

    scene.remove(mesh);
    auto empty = renderer.observe({scene.snapshot(2), camera, *view, channels(), taa});
    auto emptyIds = empty.observation->read(sim::Channel::InstanceId);
    for (size_t i = 0; i < emptyIds.bytes.size(); i += 4)
    {
        uint32_t label;
        std::memcpy(&label, emptyIds.bytes.data() + i, 4);
        if (label != 0)
            throw std::runtime_error("Removed object left a ghost");
    }
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
