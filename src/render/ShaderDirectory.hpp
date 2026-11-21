#pragma once
#include <filesystem>
#include <string_view>
namespace sim::vk
{
// 着色器定位：同一份二进制既要在构建树里跑（可执行文件上一级或同级的 shaders/），
// 也要在安装前缀里跑（<prefix>/share/sim/shaders），且不能把源码或构建目录写死进去。
// 探测文件用来判断一个候选目录是不是真的装着这套管线要用的 SPIR-V。
inline constexpr std::string_view shaderProbe = "geometry.vert.spv";

// 当前进程可执行文件所在目录；取不到时返回空路径。
std::filesystem::path executableDirectory();

// 解析顺序：
//   1. 调用方显式给的目录（DeviceOptions::shaderDirectory）——给错就报错，不回落；
//   2. 环境变量 SIM_SHADER_DIR——同上，设了就必须有效，避免悄悄换一套着色器；
//   3. 可执行文件相邻的常见布局：<exe>/shaders、<exe>/../share/sim/shaders、
//      <exe>/../shaders、<exe>/share/sim/shaders——覆盖单配置构建树、多配置构建树，
//      以及应用与安装前缀同布局的部署；
//   4. 编译期的安装目录（配置本项目时的安装前缀）。
// 前两者是"意图"（严格），后两者是"搜索"（取第一个含探测文件的）。全部落空时抛出
// 列出所有候选路径的错误，而不是静默使用一份来路不明的着色器。
std::filesystem::path resolveShaderDirectory(const std::filesystem::path& explicitDirectory);
}
