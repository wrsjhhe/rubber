# 安装与分发的端到端检查：装到一个全新的前缀，再让一个只认识 find_package 的外部项目
# 构建并离屏渲染，最后核对它用到的着色器来自安装前缀而不是源码或构建目录。
# 由 CTest 以脚本模式调用：
#   cmake -DProjectSource=... -DProjectBuild=... -DConfig=Release -DPrefix=...
#         -DGenerator=... -DGeneratorPlatform=... -DConfiguredShaderDir=... -P check.cmake
#
# 写入与清理的边界（R-M6-003）：本脚本的全部写入与递归删除都发生在 Prefix 这棵
# 测试工作目录之内（删除前用解析后的绝对路径校验）。配置时安装前缀
# （ConfiguredShaderDir，即用户机器上可能已有安装的位置）只做只读检查，绝不写入
# 或删除；两个默认查找分支的确定性覆盖改由测试目录内的专属隔离构建完成。
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

function(require_contains path needle description)
  file(READ "${path}" text)
  string(FIND "${text}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${description}: ${path} does not mention ${needle}")
  endif()
endfunction()

# 递归删除只允许作用于测试工作目录之内：先解析绝对路径再校验前缀，防止变量拼接
# 出的路径逃出测试目录（比如误把配置前缀当测试目录清掉）。
function(remove_test_tree path testRoot)
  get_filename_component(resolvedPath "${path}" ABSOLUTE)
  get_filename_component(resolvedRoot "${testRoot}" ABSOLUTE)
  string(FIND "${resolvedPath}" "${resolvedRoot}/" inside)
  if(NOT resolvedPath STREQUAL resolvedRoot AND NOT inside EQUAL 0)
    message(FATAL_ERROR "refusing to remove a path outside the test working directory:\n"
                        "  path: ${path}\n  resolved: ${resolvedPath}\n  root: ${resolvedRoot}")
  endif()
  file(REMOVE_RECURSE "${path}")
endfunction()

# 本脚本独占整棵测试工作目录：开始时清空（含上次运行残留的 consumer/iso 子树），
# 之后所有构建、安装与保留性哨兵都在它之下创建。
set(testRoot "${Prefix}")
set(consumerBuild "${testRoot}/consumer")
set(isoPrefix "${testRoot}/iso")
set(isoBuild "${testRoot}/iso-build")
set(isoConsumer "${testRoot}/iso-consumer")
set(retentionRoot "${testRoot}/retention")
remove_test_tree("${testRoot}" "${testRoot}")

# 1. 安装：只装本次构建真正产出的东西。
run("install" "${CMAKE_COMMAND}" --install "${ProjectBuild}" --config "${Config}" --prefix "${testRoot}")

# 2. 安装树的内容契约：头文件、库、SPIR-V、CMake 包文件，以及随包安装的 glm。
require_file("${testRoot}/include/sim/Renderer.hpp" "public header")
require_file("${testRoot}/include/glm/glm.hpp" "vendored glm header")
require_file("${testRoot}/share/sim/shaders/geometry.vert.spv" "compiled shader")
require_file("${testRoot}/share/sim/shaders/presentation.frag.spv" "compiled shader")
require_file("${testRoot}/include/sim/Processing.hpp" "public header")
file(GLOB configs "${testRoot}/lib/cmake/SimRenderer/SimRendererConfig.cmake")
if(NOT configs)
  file(GLOB configs "${testRoot}/lib64/cmake/SimRenderer/SimRendererConfig.cmake")
endif()
if(NOT configs)
  message(FATAL_ERROR "SimRendererConfig.cmake was not installed under ${testRoot}")
endif()
file(GLOB libraries "${testRoot}/lib/*renderer*" "${testRoot}/lib64/*renderer*")
if(NOT libraries)
  message(FATAL_ERROR "the renderer library was not installed under ${testRoot}")
endif()
file(GLOB installedShaders "${testRoot}/share/sim/shaders/*.spv")
list(LENGTH installedShaders shaderCount)
if(shaderCount LESS 10)
  message(FATAL_ERROR "expected the full pipeline shader set, found ${shaderCount} files")
endif()
file(GLOB globals "${testRoot}/share/sim/shaders/*.glsl")
if(globals)
  message(FATAL_ERROR "GLSL sources must not be installed: ${globals}")
endif()

# 3. 安装出来的包文件不能指回源码或构建目录：外部项目换台机器也要能用。
file(GLOB_RECURSE packFiles "${testRoot}/lib/cmake/SimRenderer/*" "${testRoot}/lib64/cmake/SimRenderer/*")
foreach(file ${packFiles})
  require_text("${file}" "${ProjectSource}" "package file pointing at the source tree")
  require_text("${file}" "${ProjectBuild}" "package file pointing at the build tree")
endforeach()
foreach(file ${packFiles})
  require_text("${file}" "${ProjectSource}/shaders" "package file pointing at the source shaders")
  require_text("${file}" "${ProjectBuild}/shaders" "package file pointing at the build shaders")
endforeach()

# 3b. 导出的 target 必须把 C++20 作为 PUBLIC 编译特性传播（R-M6-001）：消费者不设
#     CMAKE_CXX_STANDARD，只链接 sim::renderer 就要得到正确的语言标准。
file(GLOB targetFiles "${testRoot}/lib/cmake/SimRenderer/SimRendererTargets.cmake"
                      "${testRoot}/lib64/cmake/SimRenderer/SimRendererTargets.cmake")
if(NOT targetFiles)
  message(FATAL_ERROR "SimRendererTargets.cmake was not installed under ${testRoot}")
endif()
foreach(file ${targetFiles})
  require_contains("${file}" "cxx_std_20" "exported target must propagate the C++20 compile feature")
endforeach()

function(find_application buildDir outVar)
  file(GLOB_RECURSE found "${buildDir}/*sim_package_check.exe" "${buildDir}/*sim_package_check")
  list(FILTER found EXCLUDE REGEX "CMakeFiles")
  list(LENGTH found count)
  if(count EQUAL 0)
    message(FATAL_ERROR "external project did not produce sim_package_check under ${buildDir}")
  endif()
  list(GET found 0 application)
  set("${outVar}" "${application}" PARENT_SCOPE)
endfunction()

function(add_external_project sourceDir buildDir prefixPath outVar)
  set(configureArguments -S "${sourceDir}" -B "${buildDir}"
                         "-DCMAKE_PREFIX_PATH=${prefixPath}" "-DCMAKE_BUILD_TYPE=${Config}")
  if(Generator)
    list(APPEND configureArguments -G "${Generator}")
  endif()
  if(GeneratorPlatform)
    list(APPEND configureArguments -A "${GeneratorPlatform}")
  endif()
  run("external configure (${buildDir})" "${CMAKE_COMMAND}" ${configureArguments})
  run("external build (${buildDir})" "${CMAKE_COMMAND}" --build "${buildDir}" --config "${Config}")
  find_application("${buildDir}" application)
  set("${outVar}" "${application}" PARENT_SCOPE)
endfunction()

function(check_default_run application consumerBuild configuredDir)
  # 合法安装命中：默认目录里有 SPIR-V，不带环境变量必须跑通，且报告的路径必须是
  # 配置目录本身（而不是源码/构建目录的回落）。
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=SIM_SHADER_DIR "${application}"
                  WORKING_DIRECTORY "${consumerBuild}"
                  RESULT_VARIABLE defaultCode OUTPUT_VARIABLE defaultOut ERROR_VARIABLE defaultErr)
  if(NOT defaultCode EQUAL 0)
    message(FATAL_ERROR "the configured install directory has shaders but the external project failed "
                        "without SIM_SHADER_DIR (${defaultCode}):\n${defaultOut}\n${defaultErr}")
  endif()
  string(FIND "${defaultOut}" "shader-directory: ${configuredDir}" defaultOrigin)
  if(defaultOrigin EQUAL -1)
    message(FATAL_ERROR "external project did not resolve the default lookup to the configured install "
                        "directory ${configuredDir}:\n${defaultOut}")
  endif()
  foreach(leak "${ProjectSource}/shaders" "${ProjectBuild}/shaders")
    string(FIND "${defaultOut}\n${defaultErr}" "${leak}" leaked)
    if(NOT leaked EQUAL -1)
      message(FATAL_ERROR "external project's default lookup touched ${leak}:\n${defaultOut}\n${defaultErr}")
    endif()
  endforeach()
endfunction()

function(expect_default_failure application consumerBuild configuredDir requireNamedCandidate)
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
  if(requireNamedCandidate)
    string(FIND "${failureText}" "${configuredDir}" configured)
    if(configured EQUAL -1)
      message(FATAL_ERROR "the diagnostic does not name the configured install directory ${configuredDir}:\n${failureText}")
    endif()
  endif()
endfunction()

# 4. 外部项目：只给安装前缀，不 include 原项目的任何 CMake 文件。
add_external_project("${ProjectSource}/tests/package" "${consumerBuild}" "${testRoot}" application)

# 5. 真实配置前缀（编译进二进制的默认目录）只做只读检查（R-M6-003）：
#    - 前缀里已有合法安装（用户 cmake --install 过）：不带任何环境变量也必须成功，
#      且报告的就是配置目录——把"合法安装命中"判成"回落太松"是错的（R-M6-002）。
#    - 前缀里没有安装，或目录里没有 probe 文件（可能是别的版本、别的用途的目录）：
#      必须明确失败并列出候选路径——而不是悄悄用源码或构建目录里的着色器。
#    两种情况都不向该前缀写入或删除任何内容。
set(configuredProbe "${ConfiguredShaderDir}/geometry.vert.spv")
if(EXISTS "${configuredProbe}")
  check_default_run("${application}" "${consumerBuild}" "${ConfiguredShaderDir}")
else()
  expect_default_failure("${application}" "${consumerBuild}" "${ConfiguredShaderDir}" TRUE)
endif()

# 5b. 两个默认查找分支的确定性覆盖用专属隔离构建完成：全新前缀是测试目录内的子目录，
#     本次运行亲手创建、验证通过后才清理。正例（安装命中）→ 清理 → 负例（缺失必须
#     失败），一个运行内两个分支都必然执行。
run("isolated configure" "${CMAKE_COMMAND}" -S "${ProjectSource}" -B "${isoBuild}"
        "-DCMAKE_INSTALL_PREFIX=${isoPrefix}" -DRENDERER_EXAMPLE=OFF -DBUILD_TESTING=OFF)
run("isolated build" "${CMAKE_COMMAND}" --build "${isoBuild}" --config "${Config}")
run("isolated install" "${CMAKE_COMMAND}" --install "${isoBuild}" --config "${Config}" --prefix "${isoPrefix}")
set(isoConfiguredDir "${isoPrefix}/share/sim/shaders")
require_file("${isoConfiguredDir}/geometry.vert.spv" "isolated install shader")
add_external_project("${ProjectSource}/tests/package" "${isoConsumer}" "${isoPrefix}" isoApplication)
check_default_run("${isoApplication}" "${isoConsumer}" "${isoConfiguredDir}")

# 5c. 保留性测试（R-M6-003）：不是本次创建的内容必须原样保留。
#     - 验证失败不触发清理：显式路径无效导致运行失败后，隔离安装必须原样保留
#       （清理只发生在正例验证成功之后；脚本中途 FATAL 也不清理，内容留给下次
#       运行的受控清理）。
#     - 已有 shader 目录但没有 probe（别的版本/别的用途）、已有前缀含用户文件：
#       状态检查走只读分支，见 5d。
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SIM_SHADER_DIR=${testRoot}/no-such-shaders" "${isoApplication}"
                WORKING_DIRECTORY "${isoConsumer}"
                RESULT_VARIABLE failureCode OUTPUT_VARIABLE failureOut ERROR_VARIABLE failureErr)
if(failureCode EQUAL 0)
  message(FATAL_ERROR "an invalid explicit shader directory did not fail:\n${failureOut}")
endif()
require_file("${isoConfiguredDir}/geometry.vert.spv" "isolated install retained after a failed verification")

# 清理只在正例验证成功之后执行，且只针对测试目录内的隔离前缀。
remove_test_tree("${isoPrefix}" "${testRoot}")
# 恢复"默认目录不存在"的状态后，负例同样必须成立：回落到源码/构建目录是缺陷，保留检测。
expect_default_failure("${isoApplication}" "${isoConsumer}" "${isoConfiguredDir}" TRUE)

# 5d. 只读状态检查不会动用户内容：构造两个"看似可判定"的目录，检查后逐字节核对。
#     - shaders-no-probe：有别的 .spv 但没有 geometry.vert.spv → 不能当作本次创建的
#       安装去写或删。
#     - user-files：前缀里有用户自己的文件，配置目录整个缺失 → 同样不能动。
file(MAKE_DIRECTORY "${retentionRoot}/shaders-no-probe" "${retentionRoot}/user-files")
file(WRITE "${retentionRoot}/shaders-no-probe/legacy.vert.spv" "legacy shader bytes")
file(WRITE "${retentionRoot}/user-files/user-data.txt" "user content\n")
# 用严格显式路径触发失败（确定性不依赖真实配置前缀的状态），确认脚本对这些目录零写入。
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SIM_SHADER_DIR=${retentionRoot}/shaders-no-probe" "${application}"
                WORKING_DIRECTORY "${consumerBuild}"
                RESULT_VARIABLE failureCode OUTPUT_VARIABLE failureOut ERROR_VARIABLE failureErr)
if(failureCode EQUAL 0)
  message(FATAL_ERROR "a shader directory without the probe file was accepted:\n${failureOut}")
endif()
require_file("${retentionRoot}/shaders-no-probe/legacy.vert.spv" "pre-existing shader without probe retained")
require_file("${retentionRoot}/user-files/user-data.txt" "pre-existing user file retained")

# 6. 指到安装前缀后，应当离屏渲染成功，且用的就是前缀下的 SPIR-V。
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SIM_SHADER_DIR=${testRoot}/share/sim/shaders" "${application}"
                WORKING_DIRECTORY "${consumerBuild}"
                RESULT_VARIABLE runCode OUTPUT_VARIABLE runOut ERROR_VARIABLE runErr)
if(NOT runCode EQUAL 0)
  message(FATAL_ERROR "external project failed to render (${runCode}):\n--- stdout ---\n${runOut}\n--- stderr ---\n${runErr}")
endif()
string(FIND "${runOut}" "shader-directory: ${testRoot}/share/sim/shaders" shaderOrigin)
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

message(STATUS "install tree: ${testRoot}")
message(STATUS "external consumer: ${runOut}")
