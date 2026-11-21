# M6 独立审核

## 最终复审（2026-11-21 / 67e11ec，文档 e9edd86）

**M6 验收通过。R-M6-001、R-M6-002、R-M6-003 关闭。** 本轮只更新审核与进度文档，未修改产品或测试代码。

- R-M6-001：PUBLIC cxx_std_20 随目标导出，外部消费者没有自设标准；独立生成的 consumer/sim_package_check.vcxproj 含 LanguageStandard=stdcpp20，实际构建通过。
- R-M6-002：真实配置目录按 probe 存在与否检查成功/失败，不再无条件要求失败。隔离构建使用自己的配置前缀，实际完成安装命中正例与移除隔离安装后的负例。
- R-M6-003：真实 ConfiguredShaderDir 仅用于探测和结果检查，不再作为安装或删除目标。CTest 的 Prefix 固定为构建目录下的 package-check，隔离构建、安装和递归删除均位于该测试目录；原有覆盖和删除真实安装的路径已移除。remove_test_tree 做绝对路径的词法边界检查；此结论针对 CTest 提供的普通专属目录，不将它宣称为抵御任意手动 Prefix 或目录链接的通用删除工具。

### 独立验证

Release 全目标构建通过；九项普通 CTest 通过。package_check 首次因 codeload.github.com 域名解析失败停在 glm 下载，联网重跑通过（56.56 秒），所以十项均已取得通过结果，但并非首次整套运行 10/10。安装测试实际完成外部消费者构建、离屏运行及隔离配置的正反分支。

七项设备测试 --validation 与 gallery --smoke --validation 均退出 0，控制台未见校验错误。temporal_check：328 个轮廓像素，raw=4.78522、TAA=1.23697，运动 105.933/62.5146。本轮未独立重复截图比较、449 文件真实安装树哈希对比或开发者报告的缺陷注入；这些仍属于交接证据。

### 非阻塞补强：保留性测试与证据措辞

check.cmake 的 5c/5d 仅用 require_file 验证哨兵仍存在，没有读取内容或比较哈希；user-files 哨兵目录也没有作为配置前缀参与受测流程。严格显式 shader 路径失败主要覆盖运行时解析器，不能独自证明包测试的真实配置前缀分支不会改写用户文件。因此当前自动化用例不能表述为三类场景均“逐字节保留”。建议后续对目录清单与文件内容做前后对比，并让用户文件场景实际经过待验证的只读分支。此项不阻塞本轮：危险写入路径已从脚本移除，但保留性回归检测强度仍需提升。

---

## 工作树复审（2026-11-21，基于 a6f3ac7 的未提交修改）

R-M6-001：PUBLIC cxx_std_20 已加入，消费者已移除自设标准，代码修复正确。R-M6-002 的存在/缺失分支方向正确，但当前安装/清理引入以下阻塞问题，暂不验收。

### R-M6-003 / P1：以单个探测文件缺失推断安装目录所有权

tests/package/check.cmake 在 geometry.vert.spv 不存在时，就向 ConfiguredPrefix 执行完整安装，并在成功后设置 ownsConfiguredInstall。若前缀原本存在，清理会递归删除 ConfiguredShaderDir。该目录可能原本已包含其他文件或不完整/其他版本安装，探测文件缺失不代表目录是测试新建的。完整安装还可能覆盖既有 include/lib，删除 shader 并不能恢复用户原状态。若正例验证中途 FATAL_ERROR，清理也不会执行。

要求：不要在用户配置前缀中自动安装再删除。该前缀只做只读的存在/缺失验证；需要两个分支都覆盖时，另用测试专属隔离构建与全新前缀，明确限制所有写入/递归清理路径在测试工作目录内并检查解析后的绝对路径。不得根据单个文件缺失授予目录所有权。补已有 shader 目录但无 probe、已有前缀含用户文件、安装/验证失败三类保留性测试。

本轮为避免现有安装被改写，不运行 package_check；构建及其余回归单独验证。此处暂停的是存在实际删除风险的测试，不是对正常修复开发另设审批。

---

日期：2026-11-21；实现 acb4eb4，文档 a6f3ac7。结论：需修改，M6 暂不验收。本轮未修改产品代码。

## 独立验证

build-dev Release 构建及十项 CTest 全通过，包含 shader_check、package_check。后者实际完成新前缀安装、外部项目配置/构建与指定安装 shader 路径后的离屏运行。本轮未独立重复 build-m6 无环境变量安装路径、设备测试的 --validation、截图比较或缺陷注入。

## R-M6-001 / P2：导出 target 没有传播 C++20 要求

CMakeLists.txt 只有目录级 CMAKE_CXX_STANDARD=20，没有 target_compile_features(renderer PUBLIC cxx_std_20)。安装的 SimRendererTargets.cmake 因而没有 INTERFACE_COMPILE_FEATURES。消费者只 find_package 并链接 sim::renderer 时，仍可能使用默认 C++14/17，无法编译公开头中的默认比较运算符、std::span 等 C++20 接口。

现有 tests/package/CMakeLists.txt 自己设置 CMAKE_CXX_STANDARD=20，掩盖了导出 target 缺失的使用要求。修复应把 C++20 作为 PUBLIC 编译特性随 target 导出；外部测试移除自行设置标准，验证只链接目标即可获得所需语言标准。此项根据生成包和公开头检查确认，本轮未额外编译默认标准失败用例。

## R-M6-002 / P2：package_check 把合法的已安装默认目录当成失败

tests/package/check.cmake 第五步无条件要求 unset SIM_SHADER_DIR 后运行必须失败。但解析器明确允许配置时安装目录；用户先正常 cmake --install、再跑 CTest 时，该目录已有合法 shader，外部应用成功才是正确行为，测试却报 default lookup is too loose。不同测试前缀并不能证明配置前缀不存在。

修复：缺失目录负例应使用隔离配置且保证该配置的默认目录不存在，或根据配置目录实际状态区分合法安装命中与源码/构建回落；不能删除用户已有安装以制造负例。补“先安装到配置前缀，再运行 package_check”场景，同时保留默认目录回退到构建树的缺陷检测。

以上两项修复后复审。--prefix 重定位需显式 shader 路径、静态库和平台验证限制已在交接中说明，本轮不额外扩大范围。
