#pragma once
#include "Scene.hpp"
namespace sim
{
// 导入默认场景的静态三角形，保留节点变换与共享资产。
Scene loadGltf(const std::filesystem::path&);
}
