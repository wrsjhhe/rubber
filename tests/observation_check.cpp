// 观测与呈现验证：标定与投影一致、采样时间与快照一致、Beauty 与调试图同帧互不污染、
// 几何真值不随曝光/调试/TAA 变化，以及处理链只能生成新输出而不能改写真值。
#include "sim/Observation.hpp"
#include "sim/Processing.hpp"
#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <iostream>
#include <memory>
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
uint32_t centerOf(const OutputData& data)
{
    return (data.info.height / 2) * data.info.width + data.info.width / 2;
}
float readFloat(const OutputData& data, uint32_t index)
{
    float value;
    std::memcpy(&value, data.bytes.data() + size_t(index) * 4, 4);
    return value;
}
uint32_t readLabel(const OutputData& data, uint32_t index)
{
    uint32_t value;
    std::memcpy(&value, data.bytes.data() + size_t(index) * 4, 4);
    return value;
}
// 派生输出：由米制深度算逆深度。真实传感器阶段（畸变、噪声）形状与此相同。
class InverseDepthStage final : public SensorStage
{
  public:
    std::string_view name() const override
    {
        return "inverse-depth";
    }
    std::vector<DerivedDesc> outputs(const Observation& input) const override
    {
        auto& calibration = input.calibration();
        return {{"inverse-depth", PixelFormat::R32Float, calibration.width, calibration.height}};
    }
    void process(DerivedSink& sink) const override
    {
        auto& output = sink.write("inverse-depth");
        auto depth = sink.input().read(Channel::MetricDepth);
        for (uint32_t i = 0; i < depth.info.width * depth.info.height; ++i)
        {
            float value = readFloat(depth, i);
            float inverse = value > 0 ? 1.f / value : 0.f;
            std::memcpy(output.bytes.data() + size_t(i) * 4, &inverse, 4);
        }
    }
};
// 试图把派生输出命名成真值通道：处理链必须整体拒绝。
class ShadowingStage final : public SensorStage
{
  public:
    std::string_view name() const override
    {
        return "shadowing";
    }
    std::vector<DerivedDesc> outputs(const Observation& input) const override
    {
        auto& calibration = input.calibration();
        return {{"MetricDepth", PixelFormat::R32Float, calibration.width, calibration.height}};
    }
    void process(DerivedSink&) const override
    {
    }
};
// 不声明就写别人的名字：写入句柄必须拒绝。
class SloppyStage final : public SensorStage
{
  public:
    std::string_view name() const override
    {
        return "sloppy";
    }
    std::vector<DerivedDesc> outputs(const Observation& input) const override
    {
        auto& calibration = input.calibration();
        return {{"sloppy", PixelFormat::R32Float, calibration.width, calibration.height}};
    }
    void process(DerivedSink& sink) const override
    {
        sink.write("MetricDepth");
    }
};
int main(int argc, char** argv)
try
{
    DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    Renderer renderer(device);
    Scene scene;
    auto material = std::make_shared<MeshStandardMaterial>();
    material->color = {.75f, .06f, .04f};
    auto boxId = scene.add(BoxGeometry(), material);
    // 第二个箱体带斜角：它的轮廓与深度随亚像素抖动变化，真值是否与抖动无关才有可测的差异。
    scene.add(BoxGeometry(), material,
              glm::rotate(glm::translate(glm::mat4(1), {1.2f, .3f, 0}), .55f, glm::vec3(0, 1, 0)));
    auto snapshot = scene.snapshot(1.25);
    PerspectiveCamera camera;
    camera.position = {0, 0, 5};
    RenderOptions flat;
    flat.taa = false;

    // 1. 标定与时间：观测携带的标定能反算出渲染器用的同一份投影，采样时刻就是快照时间。
    auto view = renderer.createView(96, 64, 64);
    {
        auto observed = renderer.observe({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}, flat});
        expect(observed.status == SubmitStatus::Submitted && observed.observation, "Observation was rejected");
        auto& observation = *observed.observation;
        expect(observation.time() == 1.25, "Observation time differs from the snapshot");
        expect(observation.sampleTime().time == 1.25 && observation.sampleTime().duration == 0,
               "Default sample time is not the instantaneous snapshot time");
        auto calibration = observation.calibration();
        expect(calibration.width == 96 && calibration.height == 64 && calibration.nearPlane == camera.nearPlane &&
                   calibration.farPlane == camera.farPlane,
               "Calibration lost the sensor extent or the frustum");
        expect(std::abs(calibration.fx - calibration.fy) < 1e-4f && calibration.cx == 48 && calibration.cy == 32,
               "Default calibration is not a centered square-pixel camera");
        auto expected = camera.projection(96, 64);
        auto rebuilt = calibration.projection();
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                expect(std::abs(rebuilt[column][row] - expected[column][row]) < 1e-5f,
                       "Calibration projection differs from the renderer projection");
        // 渲染器的两轴焦距与标定同源：|P00| * w / 2、|P11| * h / 2。
        expect(std::abs(calibration.fx - std::abs(expected[0][0]) * 48.f) < 1e-3f &&
                   std::abs(calibration.fy - std::abs(expected[1][1]) * 32.f) < 1e-3f,
               "Calibration focal length drifted from the projection");

        // 相机给定内参时原样保留，反算依旧与渲染一致；calibration 不依赖请求了哪些通道。
        PerspectiveCamera lens = camera;
        lens.fx = 90;
        lens.fy = 120;
        lens.cx = 52;
        lens.cy = 30;
        auto intrinsic = renderer.observe({snapshot, lens, *view, {{Channel::MetricDepth, Delivery::Cpu}}, flat});
        auto fixed = intrinsic.observation->calibration();
        expect(fixed.fx == 90 && fixed.fy == 120 && fixed.cx == 52 && fixed.cy == 30,
               "Camera intrinsics were not preserved");
        auto lensProjection = fixed.projection();
        auto cameraProjection = lens.projection(96, 64);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                expect(std::abs(lensProjection[column][row] - cameraProjection[column][row]) < 1e-6f,
                       "Intrinsic calibration does not rebuild the camera projection");
        expect(!intrinsic.observation->has(Channel::Color), "Observation reported an unrequested channel");

        // 曝光区间作为元数据保留；采样时刻必须是真的那一帧。
        RenderRequest timed{snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}, flat};
        timed.sampleTime = SampleTime{1.25, 0.004};
        auto exposed = renderer.observe(timed);
        expect(exposed.status == SubmitStatus::Submitted, "Exposure interval was rejected");
        expect(exposed.observation->sampleTime().duration == 0.004 &&
                   std::abs(exposed.observation->sampleTime().end() - 1.254) < 1e-12,
               "Exposure interval was not preserved");
        bool rejected = false;
        auto wrong = timed;
        wrong.sampleTime = SampleTime{1.0, 0.004};
        try
        {
            renderer.observe(wrong);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        expect(rejected, "Observation accepted a sample time that was never sampled");
        rejected = false;
        wrong.sampleTime = SampleTime{1.25, -0.01};
        try
        {
            renderer.observe(wrong);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        expect(rejected, "Observation accepted a negative exposure duration");
    }

    // 2. 呈现隔离：同一帧既出 Beauty 也出调试图，两份互不覆盖，历史里也只有 Beauty。
    auto plainView = renderer.createView(96, 64, 64);
    auto debugView = renderer.createView(96, 64, 64);
    auto reference = renderer.observe({snapshot, camera, *plainView,
                                       {{Channel::Color, Delivery::Cpu}, {Channel::Hdr, Delivery::Cpu}}, flat});
    auto beauty = reference.observation->read(Channel::Color);
    auto hdr = reference.observation->read(Channel::Hdr);
    RenderOptions normals = flat;
    normals.debugChannel = 1;
    auto combined = renderer.observe({snapshot, camera, *debugView,
                                      {{Channel::Color, Delivery::Cpu},
                                       {Channel::Hdr, Delivery::Cpu},
                                       {Channel::Debug, Delivery::Cpu}},
                                      normals});
    auto debugBeauty = combined.observation->read(Channel::Color);
    auto debugHdr = combined.observation->read(Channel::Hdr);
    auto normalOutput = combined.observation->read(Channel::Debug);
    expect(debugBeauty.bytes == beauty.bytes, "Debug view changed the beauty color of the same frame");
    expect(debugHdr.bytes == hdr.bytes, "Debug view leaked into the HDR result");
    expect(normalOutput.bytes != debugBeauty.bytes, "Debug output duplicated the beauty color");
    // 法线通道：箱体正面是 +Z，编码为 (0.5, 0.5, 1)，背景为零。
    auto center = centerOf(normalOutput);
    auto* pixel = normalOutput.bytes.data() + size_t(center) * 4;
    expect(pixel[2] > 200 && std::abs(int(pixel[0]) - int(pixel[1])) < 6,
           "Normal debug channel did not show the front face");
    expect(normalOutput.bytes[0] == 0 && normalOutput.bytes[1] == 0 && normalOutput.bytes[2] == 0,
           "Debug background is not black");
    // 只请求 Debug 与 Color 时 Beauty 仍是同一份；深度通道与级联通道同样不污染 Beauty。
    for (int channel : {2, 4})
    {
        auto channelView = renderer.createView(96, 64, 64);
        RenderOptions options = flat;
        options.debugChannel = channel;
        auto observed = renderer.observe({snapshot, camera, *channelView,
                                          {{Channel::Debug, Delivery::Cpu}, {Channel::Color, Delivery::Cpu}},
                                          options});
        auto debug = observed.observation->read(Channel::Debug);
        expect(observed.observation->read(Channel::Color).bytes == beauty.bytes,
               "Debug channel 2/4 changed the beauty color");
        expect(debug.bytes != beauty.bytes, "Debug channel 2/4 produced the beauty image");
        expect(debug.bytes[size_t(centerOf(debug)) * 4] > 0, "Debug channel 2/4 lost the object");
        if (channel == 2)
            expect(debug.bytes[0] == 0 && debug.bytes[1] == 0 && debug.bytes[2] == 0,
                   "Depth debug background is not zero");
    }
    // 没有选择调试通道时请求 Debug 通道必须显式报错，而不是静默给 Beauty。
    bool rejected = false;
    try
    {
        renderer.submit({snapshot, camera, *debugView, {{Channel::Debug, Delivery::Cpu}}});
    }
    catch (const std::exception& error)
    {
        // 报错原因必须是"没有选择调试通道"，而不是后续某处空句柄崩溃。
        rejected = std::string(error.what()).find("debug channel") != std::string::npos;
    }
    expect(rejected, "Debug channel was accepted without a debug channel selection");

    // 3. 互操作边界：调试图同样以输出身份交付给 GPU 消费者，窗口采样同一份视图。
    auto gpuView = renderer.createView(96, 64, 64);
    auto gpu = renderer.observe({snapshot, camera, *gpuView, {{Channel::Debug, Delivery::Gpu}}, normals});
    expect(gpu.status == SubmitStatus::Submitted, "GPU debug delivery was rejected");
    auto native = VulkanInterop::output(*gpu.observation->result(), Channel::Debug);
    expect(native.image && native.view && native.completion, "Debug channel has no GPU output");
    expect(native.format == VK_FORMAT_R8G8B8A8_UNORM, "Debug output format is not displayable");
    expect(gpuView->debugView() != VK_NULL_HANDLE, "View has no debug presentation image");
    bool missing = false;
    try
    {
        VulkanInterop::output(*gpu.observation->result(), Channel::Color);
    }
    catch (const std::invalid_argument&)
    {
        missing = true;
    }
    expect(missing, "Unrequested channel produced a GPU output");
    gpu.observation.reset();

    // 4. 真值独立：曝光、调试通道与 TAA 抖动都不改变几何真值，只改变颜色。
    auto truthView = renderer.createView(96, 64, 64);
    auto variantView = renderer.createView(96, 64, 64);
    auto stillView = renderer.createView(96, 64, 64);
    std::vector<OutputDesc> truthOutputs{{Channel::Color, Delivery::Cpu},
                                         {Channel::MetricDepth, Delivery::Cpu},
                                         {Channel::InstanceId, Delivery::Cpu}};
    RenderOptions normal;
    // 连拍几帧让抖动相位离开首帧：首帧的抖动偏移恰好为零，只用首帧无法证明真值不受抖动影响。
    auto warm = [&](View& view, const std::vector<OutputDesc>& outputs, const RenderOptions& options)
    {
        ObservationResult last;
        for (int i = 0; i < 3; ++i)
            last = renderer.observe({snapshot, camera, view, outputs, options});
        return last;
    };
    auto truth = warm(*truthView, truthOutputs, normal);
    RenderOptions altered = normal;
    altered.exposure = 4;
    altered.debugChannel = 3;
    auto debugged = warm(*variantView,
                         {{Channel::Color, Delivery::Cpu},
                          {Channel::MetricDepth, Delivery::Cpu},
                          {Channel::InstanceId, Delivery::Cpu},
                          {Channel::Debug, Delivery::Cpu}},
                         altered);
    auto still = warm(*stillView, truthOutputs, flat);
    auto depthA = truth.observation->read(Channel::MetricDepth);
    auto depthB = debugged.observation->read(Channel::MetricDepth);
    auto labelsA = truth.observation->read(Channel::InstanceId);
    auto labelsB = debugged.observation->read(Channel::InstanceId);
    expect(depthA.bytes == depthB.bytes, "Ground truth depth changed with exposure and debug output");
    expect(labelsA.bytes == labelsB.bytes, "Ground truth labels changed with exposure and debug output");
    expect(truth.observation->read(Channel::Color).bytes != debugged.observation->read(Channel::Color).bytes,
           "Exposure had no effect on the color channel");
    expect(still.observation->read(Channel::MetricDepth).bytes == depthA.bytes, "TAA jitter contaminated depth");
    expect(still.observation->read(Channel::InstanceId).bytes == labelsA.bytes, "TAA jitter contaminated labels");
    expect(std::abs(readFloat(depthA, centerOf(depthA)) - 4.5f) < .001f, "Ground truth depth is not metric");
    expect(readLabel(labelsA, centerOf(labelsA)) == boxId, "Ground truth label is not the mesh identity");

    // 5. 处理链：只能新增输出，不能改写真值通道。
    auto chainObservation = truth.observation;
    ProcessingChain chain("depth");
    chain.add(std::make_unique<InverseDepthStage>());
    expect(chain.size() == 1 && chain.name() == "depth", "Chain registration failed");
    auto processed = chain.run(chainObservation);
    expect(processed.source == chainObservation, "Processed observation lost its source");
    auto* inverse = processed.find("inverse-depth");
    expect(inverse != nullptr && inverse->rowBytes == 96 * 4 && inverse->bytes.size() == 96 * 64 * 4,
           "Derived output has the wrong extent");
    auto inverseDepth = [&]()
    {
        float value;
        std::memcpy(&value, inverse->bytes.data() + size_t(centerOf(depthA)) * 4, 4);
        return value;
    }();
    expect(std::abs(inverseDepth - 1.f / readFloat(depthA, centerOf(depthA))) < 1e-5f,
           "Derived output does not follow its input channel");
    expect(chainObservation->read(Channel::MetricDepth).bytes == depthA.bytes,
           "Processing chain changed the ground truth");
    expect(processed.find("MetricDepth") == nullptr, "Derived outputs can shadow a truth channel");

    ProcessingChain shadowing("shadowing");
    shadowing.add(std::make_unique<ShadowingStage>());
    rejected = false;
    try
    {
        shadowing.run(chainObservation);
    }
    catch (const std::invalid_argument& error)
    {
        rejected = std::string(error.what()).find("shadow") != std::string::npos;
    }
    expect(rejected, "Chain allowed a derived output to shadow a truth channel");

    ProcessingChain sloppy("sloppy");
    sloppy.add(std::make_unique<SloppyStage>());
    rejected = false;
    try
    {
        sloppy.run(chainObservation);
    }
    catch (const std::invalid_argument& error)
    {
        rejected = std::string(error.what()).find("undeclared") != std::string::npos;
    }
    expect(rejected, "Stage wrote an output it never declared");

    // 通道语义是公开契约，处理链的重名检查直接依赖它。
    expect(Observation::semantic(Channel::MetricDepth).groundTruth &&
               std::string_view(Observation::semantic(Channel::MetricDepth).unit) == "米",
           "Metric depth semantics lost its unit or truth status");
    expect(Observation::semantic(Channel::InstanceId).nearest &&
               !Observation::semantic(Channel::Debug).groundTruth &&
               !Observation::semantic(Channel::Color).groundTruth,
           "Channel semantics lost the nearest or truth distinction");
    expect(std::string_view(Observation::semantic(Channel::Debug).name) == "Debug" &&
               std::string_view(Observation::semantic(Channel::Color).name) == "Color",
           "Channel names drifted from the processing chain contract");

    std::cout << "PASS: observation calibration, sample time, presentation isolation, truth independence and "
                 "processing chain\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
