#include "render/ShaderDirectory.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace fs = std::filesystem;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void setEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
bool samePath(const fs::path& a, const fs::path& b)
{
    std::error_code first, second;
    const auto left = fs::weakly_canonical(a, first), right = fs::weakly_canonical(b, second);
    if (first || second)
        return a.lexically_normal() == b.lexically_normal();
    return left == right;
}
// 着色器定位的回归：构建树里没有任何"安装前缀"，默认目录因此不存在，必须靠可执行文件
// 位置找到 shaders/。这条路径此前由编译期宏直接指向构建目录，安装后就失效。
int main()
try
{
    // 1. 构建树：默认（无显式目录、无环境变量）应当命中可执行文件相邻的 shaders。
    setEnvironment("SIM_SHADER_DIR", nullptr);
    const fs::path buildShaders{TEST_BUILD_SHADER_DIR};
    const auto resolved = sim::vk::resolveShaderDirectory({});
    expect(samePath(resolved, buildShaders), "Default resolution did not reach the build tree shaders");
    expect(resolved.string().find("..") == std::string::npos, "Resolved shader directory is not normalized");
    expect(!sim::vk::executableDirectory().empty(), "Executable directory is unknown");

    // 2. 显式目录：有效则用它，且尾部 ".." 会被归一化。
    const auto temp = fs::temp_directory_path() / "sim-shader-check";
    fs::remove_all(temp);
    fs::create_directories(temp);
    const fs::path probe{sim::vk::shaderProbe};
    std::ofstream(temp / probe, std::ios::binary) << "spirv";
    expect(samePath(sim::vk::resolveShaderDirectory(temp), temp), "Explicit shader directory was ignored");
    expect(samePath(sim::vk::resolveShaderDirectory(temp / "nested" / ".."), temp),
           "Explicit shader directory was not normalized");

    // 3. 显式目录无效：报错而不是悄悄回落到搜索。
    bool rejected = false;
    try
    {
        sim::vk::resolveShaderDirectory(temp / "missing");
    }
    catch (const std::runtime_error& error)
    {
        rejected = std::string(error.what()).find("does not contain") != std::string::npos;
    }
    expect(rejected, "Explicit shader directory was accepted without the probe");

    // 4. 环境变量：有效则采用，且优先于可执行文件相邻位置。
    setEnvironment("SIM_SHADER_DIR", temp.string().c_str());
    expect(samePath(sim::vk::resolveShaderDirectory({}), temp), "SIM_SHADER_DIR was ignored");
    // 5. 显式目录优先于环境变量。
    expect(samePath(sim::vk::resolveShaderDirectory(buildShaders), buildShaders),
           "Explicit shader directory lost to the environment");

    // 6. 环境变量无效同样报错，并在文本里点名是它。
    setEnvironment("SIM_SHADER_DIR", (temp / "missing").string().c_str());
    rejected = false;
    try
    {
        sim::vk::resolveShaderDirectory({});
    }
    catch (const std::runtime_error& error)
    {
        rejected = std::string(error.what()).find("SIM_SHADER_DIR") != std::string::npos;
    }
    expect(rejected, "Invalid SIM_SHADER_DIR silently fell back to another directory");

    setEnvironment("SIM_SHADER_DIR", nullptr);
    fs::remove_all(temp);
    std::cout << "shader directory: " << resolved.string() << "\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "shader_check failed: " << error.what() << "\n";
    return 1;
}
