cmake_minimum_required(VERSION 3.24)
project(SimRenderer VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# /utf-8 保证源码里的中文注释按 UTF-8 解释；/EHsc 显式开启异常展开语义，
# 不依赖生成器是否把默认值写进 CMAKE_CXX_FLAGS。
if(MSVC)
  add_compile_options(/utf-8 /EHsc)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)
include(FetchContent)
option(RENDERER_EXAMPLE "Build the GLFW / Dear ImGui gallery" ON)
option(RENDERER_INSTALL "Install headers, library, shaders and the CMake package" ON)
FetchContent_Declare(glm URL https://codeload.github.com/g-truc/glm/zip/refs/tags/1.0.1 URL_HASH SHA256=09c5716296787e1f7fcb87b1cbdbf26814ec1288ed6259ccd30d5d9795809fa5)
FetchContent_Declare(stb URL https://codeload.github.com/nothings/stb/zip/f0569113c93ad095470c54bf34a17b36646bbbb5 URL_HASH SHA256=b62be3009cb9d4b6385bd4854bddf72d8e91f4e19d1347d6cf9589b19f70d45d)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm stb)
set(shader_outputs)
foreach(shader geometry.vert geometry.frag shadow.vert fullscreen.vert lighting.frag temporal.frag color.frag
        presentation.frag points.vert)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
    DEPENDS "shaders/${shader}" shaders/frame.glsl VERBATIM)
  list(APPEND shader_outputs "${output}")
endforeach()
# 点云片元着色器编译两遍：颜色通道额外输出 HDR，几何真值通道没有 HDR 附件。
foreach(variant "points;1" "points.depth;0")
  list(GET variant 0 name)
  list(GET variant 1 define)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.frag.spv")
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -DPOINTS_HDR=${define}
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/points.frag" -o "${output}"
    DEPENDS shaders/points.frag shaders/frame.glsl VERBATIM)
  list(APPEND shader_outputs "${output}")
endforeach()
set(ray_shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/lighting.ray.frag.spv")
add_custom_command(OUTPUT "${ray_shader}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
  COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -DRAY_QUERY=1
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lighting.frag" -o "${ray_shader}"
  DEPENDS shaders/lighting.frag shaders/frame.glsl VERBATIM)
list(APPEND shader_outputs "${ray_shader}")
add_custom_target(renderer_shaders DEPENDS ${shader_outputs})
# glm 是公共头文件里的类型（glm::mat4），因此作为接口目标随包一起安装：
# 消费者只要 find_package 就能编译，不必另找一份可能版本不同的 glm。
add_library(sim_glm INTERFACE)
add_library(sim::glm ALIAS sim_glm)
target_include_directories(sim_glm INTERFACE
  $<BUILD_INTERFACE:${glm_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(renderer
  src/scene/Scene.cpp src/assets/Gltf.cpp src/backend/Device.cpp
  src/render/Pipeline.cpp src/render/RenderScene.cpp src/render/RayShadows.cpp src/render/graph/FrameGraph.cpp
  src/render/passes/ShadowPass.cpp src/render/passes/GeometryPass.cpp src/render/passes/LightingPass.cpp
  src/render/passes/TemporalPass.cpp src/render/passes/ColorPass.cpp src/render/passes/ReadbackPass.cpp
  src/render/passes/PointsPass.cpp
  src/modules/mesh/MeshModule.cpp src/modules/points/PointsModule.cpp src/modules/points/RenderPoints.cpp
  src/sensors/Observation.cpp src/sensors/Processing.cpp src/presentation/Presentation.cpp
  src/render/ShaderDirectory.cpp src/render/Renderer.cpp)
add_library(sim::renderer ALIAS renderer)
add_dependencies(renderer renderer_shaders)
target_include_directories(renderer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${stb_SOURCE_DIR} third_party/cgltf)
target_link_libraries(renderer PUBLIC Vulkan::Vulkan sim_glm)
# 公开头直接暴露 C++20 接口（std::optional、std::span、观测/处理链的返回类型）。
# 语言要求必须作为 PUBLIC 编译特性随导出 target 传播：消费者只 find_package + 链接
# sim::renderer 就得到正确的标准，不依赖消费者自己设置 CMAKE_CXX_STANDARD。
target_compile_features(renderer PUBLIC cxx_std_20)
# 二进制里的默认着色器目录指向安装位置，而不是源码或构建目录：安装后的库应当在自己的
# 安装前缀下找到 SPIR-V。构建树里的可执行文件由 exe 相对路径命中（见 ShaderDirectory.cpp），
# 因此同一份二进制在两种布局下都工作，且不含构建目录路径。
set(RENDERER_SHADER_INSTALL_DIR "${CMAKE_INSTALL_FULL_DATADIR}/sim/shaders")
target_compile_definitions(renderer
  PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS
  PRIVATE "RENDERER_SHADER_DIR=\"${RENDERER_SHADER_INSTALL_DIR}\"")
include(CTest)
if(BUILD_TESTING)
  add_executable(sensor_check tests/sensor_check.cpp)
  target_link_libraries(sensor_check PRIVATE renderer)
  add_test(NAME sensor_check COMMAND sensor_check)
  add_executable(temporal_check tests/temporal_check.cpp)
  target_link_libraries(temporal_check PRIVATE renderer)
  add_test(NAME temporal_check COMMAND temporal_check)
  add_executable(request_check tests/request_check.cpp)
  target_link_libraries(request_check PRIVATE renderer)
  add_test(NAME request_check COMMAND request_check)
  add_executable(asset_check tests/asset_check.cpp)
  target_link_libraries(asset_check PRIVATE renderer)
  add_test(NAME asset_check COMMAND asset_check)
  add_executable(pass_check tests/pass_check.cpp)
  target_link_libraries(pass_check PRIVATE renderer)
  add_test(NAME pass_check COMMAND pass_check)
  add_executable(points_check tests/points_check.cpp)
  target_link_libraries(points_check PRIVATE renderer)
  add_test(NAME points_check COMMAND points_check)
  add_executable(graph_check tests/graph_check.cpp)
  target_link_libraries(graph_check PRIVATE renderer)
  target_include_directories(graph_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME graph_check COMMAND graph_check)
  add_executable(observation_check tests/observation_check.cpp)
  target_link_libraries(observation_check PRIVATE renderer)
  add_test(NAME observation_check COMMAND observation_check)
  # 着色器定位不需要 GPU：专门验证"构建树按可执行文件位置、安装树按安装前缀"两条路径，
  # 以及显式目录/环境变量的优先级与失败时的诊断文本。
  add_executable(shader_check tests/shader_check.cpp)
  target_link_libraries(shader_check PRIVATE renderer)
  target_include_directories(shader_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(shader_check PRIVATE "TEST_BUILD_SHADER_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/shaders\"")
  add_test(NAME shader_check COMMAND shader_check)
  # 安装 + 导出包 + 外部项目 find_package 构建并离屏运行的端到端检查。
  add_test(NAME package_check
    COMMAND ${CMAKE_COMMAND}
      "-DProjectSource=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DProjectBuild=${CMAKE_CURRENT_BINARY_DIR}"
      "-DConfig=$<CONFIG>"
      "-DPrefix=${CMAKE_CURRENT_BINARY_DIR}/package-check"
      "-DGenerator=${CMAKE_GENERATOR}"
      "-DGeneratorPlatform=${CMAKE_GENERATOR_PLATFORM}"
      "-DConfiguredShaderDir=${RENDERER_SHADER_INSTALL_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package/check.cmake")
  # 含一次隔离构建（引擎本体 + 两个外部消费者），给足时间。
  set_tests_properties(package_check PROPERTIES TIMEOUT 1800)
endif()
if(RENDERER_INSTALL)
  install(TARGETS renderer sim_glm EXPORT SimRendererTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/sim DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY ${glm_SOURCE_DIR}/glm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")
  # 只装本轮真正产出的 SPIR-V：构建目录里可能留着改名前的旧文件，按目录整装会把它们带出去。
  install(FILES ${shader_outputs} DESTINATION ${CMAKE_INSTALL_DATADIR}/sim/shaders)
  install(EXPORT SimRendererTargets NAMESPACE sim::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimRenderer)
  configure_package_config_file(cmake/SimRendererConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SimRendererConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimRenderer
    PATH_VARS CMAKE_INSTALL_DATADIR)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/SimRendererConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SimRendererConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/SimRendererConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimRenderer)
endif()
if(RENDERER_EXAMPLE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # GLFW 只是示例的窗口后端，不随本包分发：否则它的头文件与 cmake 配置会混进安装前缀。
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw URL https://codeload.github.com/glfw/glfw/zip/refs/tags/3.4 URL_HASH SHA256=a133ddc3d3c66143eba9035621db8e0bcf34dba1ee9514a9e23e96afd39fd57a)
  FetchContent_Declare(imgui URL https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.91.9b URL_HASH SHA256=fd37507c8476a6d14cc7c4b352401f31bcbd0f0d995d35390811e968c466f46e)
  FetchContent_MakeAvailable(glfw imgui)
  add_executable(gallery example/main.cpp example/Gallery.cpp example/ShadowDemo.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(gallery PRIVATE "${stb_SOURCE_DIR}" ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_definitions(gallery PRIVATE GLFW_INCLUDE_NONE GALLERY_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/example/assets")
  target_link_libraries(gallery PRIVATE renderer glfw)
endif()
