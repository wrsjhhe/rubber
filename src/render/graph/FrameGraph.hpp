#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sim::vk
{
struct Image;
struct Buffer;
}

// 帧图：pass 只声明读写语义，编译阶段完成裁剪、顺序、屏障与生命周期。
// 声明层的格式/用途与 Vulkan 分配无关，物理资源由执行方在编译后绑定。
namespace sim::render
{
enum class Format : uint8_t
{
    Rgba8Unorm,
    Rgba16Float,
    R32Float,
    R32Uint,
    D32Float
};
enum class Aspect : uint8_t
{
    Color,
    Depth
};
enum class Layout : uint8_t
{
    Undefined,
    ColorAttachment,
    DepthAttachment,
    ShaderRead,
    TransferSrc,
    TransferDst
};
// pass 声明的语义访问；对应的布局、阶段与访问掩码由编译阶段决定。
enum class Access : uint8_t
{
    Sampled,
    UniformRead,
    StorageRead,
    ColorWrite,
    DepthWrite,
    CopySource,
    CopyDestination,
    HostRead
};
enum class ImageUsage : uint8_t
{
    Sampled = 1,
    ColorAttachment = 2,
    DepthAttachment = 4,
    TransferSrc = 8,
    TransferDst = 16
};
enum class BufferUsage : uint8_t
{
    Uniform = 1,
    Storage = 2,
    TransferSrc = 4,
    TransferDst = 8,
    HostRead = 16
};
constexpr ImageUsage operator|(ImageUsage a, ImageUsage b)
{
    return ImageUsage(uint8_t(a) | uint8_t(b));
}
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b)
{
    return BufferUsage(uint8_t(a) | uint8_t(b));
}
constexpr bool has(ImageUsage set, ImageUsage bit)
{
    return (uint8_t(set) & uint8_t(bit)) != 0;
}
constexpr bool has(BufferUsage set, BufferUsage bit)
{
    return (uint8_t(set) & uint8_t(bit)) != 0;
}
struct ImageDesc
{
    Format format = Format::Rgba8Unorm;
    uint32_t width = 0, height = 0;
    uint32_t mipLevels = 1, layers = 1;
    Aspect aspect = Aspect::Color;
    ImageUsage usage = ImageUsage::Sampled;
};
struct BufferDesc
{
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::Storage;
};
// 图像子资源范围；mipCount/layerCount 为零表示到该维度末尾。
struct Subresource
{
    uint32_t mip = 0, mipCount = 1;
    uint32_t layer = 0, layerCount = 1;
};
// 缓冲字节范围；size 为零表示到缓冲末尾。
struct BufferRange
{
    uint64_t offset = 0, size = 0;
};
struct ImageHandle
{
    uint32_t index = UINT32_MAX;
    explicit operator bool() const
    {
        return index != UINT32_MAX;
    }
    bool operator==(const ImageHandle&) const = default;
};
struct BufferHandle
{
    uint32_t index = UINT32_MAX;
    explicit operator bool() const
    {
        return index != UINT32_MAX;
    }
    bool operator==(const BufferHandle&) const = default;
};
struct PassHandle
{
    uint32_t index = UINT32_MAX;
    explicit operator bool() const
    {
        return index != UINT32_MAX;
    }
    bool operator==(const PassHandle&) const = default;
};
// 一条待执行的屏障：编译期算定，执行期只做翻译，不再推导状态。
struct Barrier
{
    bool image = true;
    uint32_t resource = 0;
    Subresource sub;
    BufferRange range;
    Layout from = Layout::Undefined, to = Layout::Undefined;
    VkPipelineStageFlags srcStage = 0, dstStage = 0;
    VkAccessFlags srcAccess = 0, dstAccess = 0;
};
struct CompiledPass
{
    std::string name;
    uint32_t pass = 0;             // 对应的 pass 节点，执行时取录制回调
    std::vector<Barrier> barriers; // 本 pass 开始前必须完成的屏障
};
// 编译后仍存活的瞬时资源，由执行方按声明分配并绑定。
struct Transient
{
    bool image = true;
    uint32_t resource = 0;
    ImageDesc imageDesc;
    BufferDesc bufferDesc;
};
class FrameGraph
{
  public:
    // pass 的声明入口：只写语义，不接触 VkImageLayout 与访问掩码。
    struct Declaration
    {
        void read(ImageHandle, Access = Access::Sampled, Subresource = {});
        void write(ImageHandle, Access, Subresource = {});
        // 读改写同一资源：混合、深度测试后写入、计算着色器的原地更新。
        // 与"同一 pass 先 read 再 write"不同，这是一条合法声明，不构成反馈回路，
        // 但仍然要求先前内容已定义，并保留对上一个写入者的依赖。
        void readWrite(ImageHandle, Access, Subresource = {});
        void read(BufferHandle, Access = Access::StorageRead, BufferRange = {});
        void write(BufferHandle, Access, BufferRange = {});
        // 仅约束顺序，不涉及资源的显式依赖；矛盾时编译报循环。
        void after(PassHandle);
        // 没有导出但必须执行（写回持久历史、外部副作用）。
        void sideEffect();

      private:
        friend class FrameGraph;
        Declaration() = default;
        Declaration(FrameGraph* graph, uint32_t pass) : graph(graph), pass(pass) {}
        FrameGraph* graph = nullptr;
        uint32_t pass = UINT32_MAX;
    };

    ImageHandle importImage(std::string name, const ImageDesc&, Layout initial, bool initialized);
    BufferHandle importBuffer(std::string name, const BufferDesc&, bool initialized);
    // 瞬时资源：内容不跨帧，执行方按声明分配。
    ImageHandle createImage(std::string name, const ImageDesc&);
    BufferHandle createBuffer(std::string name, const BufferDesc&);
    // 导出：内容交给消费者，声明的最终布局在执行末尾生效，同时作为裁剪根。
    void exportImage(ImageHandle, Layout final);
    void exportBuffer(BufferHandle);
    // 恢复：帧末把持久资源放回声明布局，不作为裁剪根。用于跨帧复用的视图资源，
    // 保证下一帧导入时的初始状态与声明一致。
    void restoreImage(ImageHandle, Layout final);
    // 预声明 pass 句柄，允许对之后加入的 pass 声明顺序约束。
    PassHandle reserve(std::string_view name);
    void addPass(PassHandle, std::function<void(Declaration&)> declare, std::function<void(VkCommandBuffer)> body);
    void bind(ImageHandle, vk::Image&);
    void bind(BufferHandle, vk::Buffer&);
    // 裁剪、排序、诊断与屏障推导；不接触设备。
    void compile();
    void execute(VkCommandBuffer);
    void reset();

    const std::vector<std::string>& activePasses() const
    {
        return active;
    }
    // 编译后某个 pass 是否仍然存活（会被执行）。调度侧据此判断跨帧副作用（例如历史写回）
    // 是否真的发生，而不是按"本次请求需要颜色"这类意图标志推断。
    bool executed(PassHandle) const;
    const std::vector<CompiledPass>& passes() const
    {
        return compiled;
    }
    const std::vector<Barrier>& finalBarriers() const
    {
        return final;
    }
    const std::vector<Transient>& transients() const
    {
        return transient;
    }
    uint32_t barrierCount() const;
    const std::vector<std::string>& names() const
    {
        return passNames;
    }

  private:
    struct Cell
    {
        Layout layout = Layout::Undefined;
        VkPipelineStageFlags stage = 0;
        VkAccessFlags access = 0;
        int32_t writer = -1; // 声明期的最近写入者，-1 表示没有（导入内容或未定义）
        bool defined = false;
        bool lastWrite = false;
        bool used = false;            // 本帧被活跃 pass 访问过；导出/恢复只处理这类子资源
        std::vector<uint32_t> readers; // 自上次写入以来的读取者，用于 WAR
    };
    // 缓冲的一段字节区间及其状态；段边界与声明过的范围对齐，段之间不重叠。
    struct Segment
    {
        uint64_t begin = 0, end = 0;
        Cell state;
    };
    struct Use
    {
        bool image = true;
        uint32_t resource = 0;
        Subresource sub;
        BufferRange range;
        bool write = false;
        bool readsBack = false; // 读改写：既消费先前内容又覆盖它
        Access access = Access::Sampled;
        bool reads() const
        {
            return !write || readsBack;
        }
        // 本次访问的精确子资源：图像为 cell 下标，缓冲为字节区间。声明期算定，plan 直接复用。
        std::vector<uint32_t> cells;
        std::vector<std::pair<uint64_t, uint64_t>> pieces;
        std::vector<int32_t> writers;  // 相交子资源上声明期的全部写入者（去重）
        std::vector<uint32_t> readers; // 写入前相交子资源上的全部读取者（去重），用于 WAR
    };
    struct PassNode
    {
        std::string name;
        bool sideEffect = false;
        bool added = false;
        std::function<void(Declaration&)> declare;
        std::function<void(VkCommandBuffer)> body;
        std::vector<Use> uses;
        std::vector<uint32_t> after;
    };
    struct ImageResource
    {
        std::string name;
        ImageDesc desc;
        bool imported = false;
        bool initialized = false;
        Layout importLayout = Layout::Undefined;
        Layout exportLayout = Layout::Undefined;
        bool exported = false;
        Layout restoreLayout = Layout::Undefined;
        bool restored = false;
        vk::Image* physical = nullptr;
        std::vector<Cell> cells;
    };
    struct BufferResource
    {
        std::string name;
        BufferDesc desc;
        bool imported = false;
        bool initialized = false;
        bool exported = false;
        vk::Buffer* physical = nullptr;
        // 缓冲按字节区间切段跟踪：每段有自己的写入者、读者与"是否已定义"。
        // 只覆盖声明过的区间，未声明过的字节按导入声明推导默认状态。
        std::vector<Segment> segments;
    };

    // 图像子资源的精确下标集（layer × mip 双重展开，不做首末压平）。
    std::vector<uint32_t> cellsOf(const ImageResource&, const Subresource&) const;
    // 把 [begin,end) 对齐成段边界，缺失部分按默认状态补齐，返回恰好覆盖该区间的字节区间。
    std::vector<std::pair<uint64_t, uint64_t>> segmentsOf(BufferResource&, uint64_t begin, uint64_t end);
    Segment* findSegment(BufferResource&, uint64_t begin, uint64_t end);
    void mergeSegments(BufferResource&);

    Use& addUse(Declaration&, bool image, uint32_t resource, const Subresource&, const BufferRange&, bool write,
                Access, bool readsBack = false);
    void validate(const PassNode&, const Use&) const;
    std::vector<bool> cull();
    std::vector<uint32_t> order(const std::vector<bool>&);
    void plan(const std::vector<uint32_t>&);
    void record(VkCommandBuffer, const Barrier&);

    std::vector<PassNode> nodes;
    std::vector<std::string> passNames;
    std::vector<ImageResource> images;
    std::vector<BufferResource> buffers;
    std::vector<std::string> active;
    std::vector<bool> livePasses; // 编译时的裁剪结果，按 pass 序号索引
    std::vector<CompiledPass> compiled;
    std::vector<Barrier> final;
    std::vector<Transient> transient;
};
}
