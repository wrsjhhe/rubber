# 安装与分发的端到端检查：装到一个全新的前缀，再让一个只认识 find_package 的外部项目
# 构建并离屏渲染，最后核对它用到的着色器来自安装前缀而不是源码或构建目录。
# 由 CTest 以脚本模式调用：
#   cmake -DProjectSource=... -DProjectBuild=... -DConfig=Release -DPrefix=...
#         -DGenerator=... -DGeneratorPlatform=... -DConfiguredShaderDir=... -P check.cmake
cmake_minimum_required(VERSION 3.24)

foreach(required ProjectSource ProjectBuild Prefix Generator ConfiguredShaderDir)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "check.cmake requires -D${required}=...")
  endif()
endforeach()
# 单配置生成器在未指定构建类型时会让 $<CONFIG> 展开为空：装 Release 是唯一有意义的默认。
if("${Config}" STREQUAL "")
  set(Config Release)
endif()

function(run description)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${description} failed (${code}):\n${ARGN}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
  endif()
endfunction()

function(require_file path description)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${description} is missing from the install tree: ${path}")
  endif()
endfunction()

function(require_text path needle description)
  file(READ "${path}" text)
  string(FIND "${text}" "${needle}" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "${description}: ${path} references ${needle}")
  endif()
endfunction()

set(consumerBuild "${Prefix}-consumer")
file(REMOVE_RECURSE "${Prefix}" "${consumerBuild}")

# 1. 安装：只装本次构建真正产出的东西。
run("install" "${CMAKE_COMMAND}" --install "${ProjectBuild}" --config "${Config}" --prefix "${Prefix}")

# 2. 安装树的内容契约：头文件、库、SPIR-V、CMake 包文件，以及随包安装的 glm。
require_file("${Prefix}/include/sim/Renderer.hpp" "public header")
require_file("${Prefix}/include/glm/glm.hpp" "vendored glm header")
require_file("${Prefix}/share/sim/shaders/geometry.vert.spv" "compiled shader")
require_file("${Prefix}/share/sim/shaders/presentation.frag.spv" "compiled shader")
require_file("${Prefix}/include/sim/Processing.hpp" "public header")
file(GLOB configs "${Prefix}/lib/cmake/SimRenderer/SimRendererConfig.cmake")
if(NOT configs)
  file(GLOB configs "${Prefix}/lib64/cmake/SimRenderer/SimRendererConfig.cmake")
endif()
if(NOT configs)
  message(FATAL_ERROR "SimRendererConfig.cmake was not installed under ${Prefix}")
endif()
file(GLOB libraries "${Prefix}/lib/*renderer*" "${Prefix}/lib64/*renderer*")
if(NOT libraries)
  message(FATAL_ERROR "the renderer library was not installed under ${Prefix}")
endif()
file(GLOB installedShaders "${Prefix}/share/sim/shaders/*.spv")
list(LENGTH installedShaders shaderCount)
if(shaderCount LESS 10)
  message(FATAL_ERROR "expected the full pipeline shader set, found ${shaderCount} files")
endif()
file(GLOB globals "${Prefix}/share/sim/shaders/*.glsl")
if(globals)
  message(FATAL_ERROR "GLSL sources must not be installed: ${globals}")
endif()

# 3. 安装出来的包文件不能指回源码或构建目录：外部项目换台机器也要能用。
file(GLOB_RECURSE packFiles "${Prefix}/lib/cmake/SimRenderer/*" "${Prefix}/lib64/cmake/SimRenderer/*")
foreach(file ${packFiles})
  require_text("${file}" "${ProjectSource}" "package file pointing at the source tree")
  require_text("${file}" "${ProjectBuild}" "package file pointing at the build tree")
endforeach()
foreach(file ${packFiles})
  require_text("${file}" "${ProjectSource}/shaders" "package file pointing at the source shaders")
  require_text("${file}" "${ProjectBuild}/shaders" "package file pointing at the build shaders")
endforeach()

# 4. 外部项目：只给安装前缀，不 include 原项目的任何 CMake 文件。
set(configureArguments -S "${ProjectSource}/tests/package" -B "${consumerBuild}"
                        "-DCMAKE_PREFIX_PATH=${Prefix}" "-DCMAKE_BUILD_TYPE=${Config}")
if(Generator)
  list(APPEND configureArguments -G "${Generator}")
endif()
if(GeneratorPlatform)
  list(APPEND configureArguments -A "${GeneratorPlatform}")
endif()
run("external configure" "${CMAKE_COMMAND}" ${configureArguments})
run("external build" "${CMAKE_COMMAND}" --build "${consumerBuild}" --config "${Config}")

file(GLOB_RECURSE applications "${consumerBuild}/*sim_package_check.exe" "${consumerBuild}/*sim_package_check")
list(FILTER applications EXCLUDE REGEX "CMakeFiles")
list(LENGTH applications count)
if(count EQUAL 0)
  message(FATAL_ERROR "external project did not produce sim_package_check")
endif()
list(GET applications 0 application)

# 5. 不设置任何覆盖时，二进制自带的默认目录（配置时的安装前缀）与本次前缀不同，必须
#    明确失败并列出候选路径——而不是悄悄用源码或构建目录里的着色器。
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=SIM_SHADER_DIR "${application}"
                WORKING_DIRECTORY "${consumerBuild}"
                RESULT_VARIABLE failureCode OUTPUT_VARIABLE failureOut ERROR_VARIABLE failureErr)
if(failureCode EQUAL 0)
  message(FATAL_ERROR "external project ran without finding installed shaders; the default lookup is too loose")
endif()
set(failureText "${failureOut}\n${failureErr}")
string(FIND "${failureText}" "Shader directory not found" diagnostics)
if(diagnostics EQUAL -1)
  message(FATAL_ERROR "missing shaders produced no diagnostic:\n${failureText}")
endif()
string(FIND "${failureText}" "${ConfiguredShaderDir}" configured)
if(configured EQUAL -1)
  message(FATAL_ERROR "the diagnostic does not name the configured install directory ${ConfiguredShaderDir}:\n${failureText}")
endif()

# 6. 指到安装前缀后，应当离屏渲染成功，且用的就是前缀下的 SPIR-V。
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SIM_SHADER_DIR=${Prefix}/share/sim/shaders" "${application}"
                WORKING_DIRECTORY "${consumerBuild}"
                RESULT_VARIABLE runCode OUTPUT_VARIABLE runOut ERROR_VARIABLE runErr)
if(NOT runCode EQUAL 0)
  message(FATAL_ERROR "external project failed to render (${runCode}):\n--- stdout ---\n${runOut}\n--- stderr ---\n${runErr}")
endif()
string(FIND "${runOut}" "shader-directory: ${Prefix}/share/sim/shaders" shaderOrigin)
if(shaderOrigin EQUAL -1)
  message(FATAL_ERROR "external project did not use the installed shaders:\n${runOut}")
endif()
string(FIND "${runOut}" "center-depth: 4.5" depth)
if(depth EQUAL -1)
  message(FATAL_ERROR "external project rendered the wrong geometry:\n${runOut}")
endif()
foreach(leak "${ProjectSource}/shaders" "${ProjectBuild}/shaders")
  string(FIND "${runOut}\n${runErr}" "${leak}" leaked)
  if(NOT leaked EQUAL -1)
    message(FATAL_ERROR "external project touched ${leak}:\n${runOut}\n${runErr}")
  endif()
endforeach()

message(STATUS "install tree: ${Prefix}")
message(STATUS "external consumer: ${runOut}")
