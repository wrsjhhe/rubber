cmake_minimum_required(VERSION 3.24)
project(SimRendererPackageCheck LANGUAGES CXX)
# 刻意不设置 CMAKE_CXX_STANDARD：语言标准必须由导出的 sim::renderer 通过
# INTERFACE_COMPILE_FEATURES 传播过来。这里显式设置会把导出 target 缺失
# 使用要求的问题掩盖掉（审核发现 R-M6-001 正是这么漏掉的）。
# 只通过安装前缀找包：不 include 原项目的任何 CMake 文件，也不引用它的源码。
find_package(SimRenderer 0.1 REQUIRED)
add_executable(sim_package_check main.cpp)
# 单配置生成器不自动加 Release 子目录。显式隔开安装树，确保默认搜索用例
# 不会因为 EXE 恰好在安装前缀下一层而命中 ../share/sim/shaders。
set_target_properties(sim_package_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
target_link_libraries(sim_package_check PRIVATE sim::renderer)
get_target_property(rendererType sim::renderer TYPE)
if(NOT rendererType STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "The installed renderer is expected to be a shared library")
endif()
# 隔离安装前缀清掉后，负例仍需加载同一份 DLL/共享库，以确认失败发生在着色器查找。
add_custom_command(TARGET sim_package_check POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:sim::renderer>" "$<TARGET_FILE_DIR:sim_package_check>"
  VERBATIM)
if(APPLE)
  set_target_properties(sim_package_check PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(sim_package_check PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
if(MSVC)
  target_compile_options(sim_package_check PRIVATE /utf-8 /EHsc)
endif()
