# M6 独立审核

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
