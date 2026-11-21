cmake_minimum_required(VERSION 3.24)
project(SimRendererPackageCheck LANGUAGES CXX)
# 刻意不设置 CMAKE_CXX_STANDARD：语言标准必须由导出的 sim::renderer 通过
# INTERFACE_COMPILE_FEATURES 传播过来。这里显式设置会把导出 target 缺失
# 使用要求的问题掩盖掉（审核发现 R-M6-001 正是这么漏掉的）。
# 只通过安装前缀找包：不 include 原项目的任何 CMake 文件，也不引用它的源码。
find_package(SimRenderer 0.1 REQUIRED)
add_executable(sim_package_check main.cpp)
target_link_libraries(sim_package_check PRIVATE sim::renderer)
if(MSVC)
  target_compile_options(sim_package_check PRIVATE /utf-8 /EHsc)
endif()
