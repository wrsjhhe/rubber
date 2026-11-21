#include "ShaderDirectory.hpp"
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
namespace sim::vk
{
namespace
{
std::filesystem::path modulePath()
{
#if defined(_WIN32)
    wchar_t buffer[1024];
    const DWORD size = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (size == 0 || size >= DWORD(std::size(buffer)))
        return {};
    return std::filesystem::path(std::wstring(buffer, size));
#elif defined(__linux__)
    char buffer[4096];
    const auto size = ::readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (size <= 0)
        return {};
    return std::filesystem::path(std::string(buffer, size_t(size)));
#else
    return {};
#endif
}
bool hasShaders(const std::filesystem::path& dir)
{
    if (dir.empty())
        return false;
    std::error_code ignored;
    return std::filesystem::is_regular_file(dir / shaderProbe, ignored);
}
std::filesystem::path normalize(const std::filesystem::path& p)
{
    return p.lexically_normal();
}
void requireShaders(const std::filesystem::path& dir, const char* source)
{
    if (!hasShaders(dir))
        throw std::runtime_error(std::string("Shader directory from ") + source + " does not contain " +
                                 std::string(shaderProbe) + ": " + dir.generic_string());
}
}
std::filesystem::path executableDirectory()
{
    const auto module = modulePath();
    if (module.empty())
        return {};
    return module.parent_path();
}
std::filesystem::path resolveShaderDirectory(const std::filesystem::path& explicitDirectory)
{
    // 显式目录与环境变量代表调用方的明确意图：无效就失败，不退回搜索。
    if (!explicitDirectory.empty())
    {
        requireShaders(explicitDirectory, "DeviceOptions::shaderDirectory");
        return normalize(explicitDirectory);
    }
    if (const char* env = std::getenv("SIM_SHADER_DIR"); env && *env)
    {
        requireShaders(env, "SIM_SHADER_DIR");
        return normalize(env);
    }
    std::vector<std::filesystem::path> candidates;
    if (const auto exe = executableDirectory(); !exe.empty())
    {
        candidates.push_back(exe / "shaders");
        candidates.push_back(exe / ".." / "share" / "sim" / "shaders");
        candidates.push_back(exe / ".." / "shaders");
        candidates.push_back(exe / "share" / "sim" / "shaders");
    }
    candidates.emplace_back(RENDERER_SHADER_DIR);
    std::string tried;
    for (const auto& candidate : candidates)
    {
        const auto path = normalize(candidate);
        if (hasShaders(path))
            return path;
        // 通用分隔符：诊断文本要在不同平台上被同一套脚本核对。
        tried += "\n  " + path.generic_string();
    }
    throw std::runtime_error("Shader directory not found (" + std::string(shaderProbe) + "). Tried:" + tried +
                             "\nSet SIM_SHADER_DIR, pass DeviceOptions::shaderDirectory, or install the shaders "
                             "into the prefix the library was configured for.");
}
}
