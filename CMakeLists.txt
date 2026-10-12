cmake_minimum_required(VERSION 3.20)

project(deskfeed LANGUAGES CXX)

# DeskFeed：通用 Windows 桌面信息显示。
#
# 当前阶段（Phase 1）：协议/校验/状态机 + 单主进程 + 离线检查器 + 离线用例。
# 任务栏负载 DLL（deskfeed-payload.dll）与桌面悬浮窗属于 Phase 2/3，尚未实现；
# 见 docs/architecture.md 的路线表。
#
# 目标：
#   deskfeed          deskfeed.exe        主进程（单一取数、状态机、命令通道）
#   deskfeed-check    deskfeed-check.exe  离线协议检查器 / HTTP 量具
#   deskfeed-tests    deskfeed-tests.exe  离线用例
#
# x64 only：Phase 2 的负载要与 64 位 explorer.exe 同位数，主进程与它保持一致。

if(NOT WIN32)
    message(FATAL_ERROR "DeskFeed is Windows-only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "DeskFeed requires MSVC (C++/WinRT headers and the Windows SDK ship with the Visual Studio toolchain).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DeskFeed must be built for x64 so that it can match 64-bit explorer.exe.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态 CRT：Phase 2 的负载要跑在 explorer.exe 里，不能指望那边解析得到同版本的 VC 运行库；
# 主进程与工具跟它保持一致，避免同一份工程两种运行库口径。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 全部目标共用的编译口径与 Win32 基础库。
#
# 这里**刻意不含** winhttp.lib 与 dbghelp.lib：它们分别代表“对外的网络”与“符号下载”这两种能力，
# 只有真正需要的目标才链（见下面的 deskfeed_winhttp / deskfeed_dbghelp）。理由不是洁癖：Phase 2 的
# 负载 DLL 要跑在 explorer.exe 里，而“这个模块没有任何外部网络”这条断言如果只靠“没写调用”来解释，
# 迟早会被一次无意的继承打破。把它放在目标边界上，比交给链接器按需拉取可靠。
add_library(deskfeed_base INTERFACE)
target_include_directories(deskfeed_base INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(deskfeed_base INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(deskfeed_base INTERFACE /utf-8 /W3 /permissive-)
target_link_libraries(deskfeed_base INTERFACE
    user32.lib
    shell32.lib
    ole32.lib
    advapi32.lib
)

# WinHTTP：只有取数层用（src/net/http_client.cpp）。
add_library(deskfeed_winhttp INTERFACE)
target_link_libraries(deskfeed_winhttp INTERFACE deskfeed_base winhttp.lib)

# DbgHelp：只有符号解析那一侧用（src/platform/{pdb,symbols,taskbar_symbols}.cpp）。它会从
# Microsoft 符号服务器取 PDB，这件事只发生在注入器/命令行工具里，负载一行都不碰。
add_library(deskfeed_dbghelp INTERFACE)
target_link_libraries(deskfeed_dbghelp INTERFACE deskfeed_base dbghelp.lib)

# UTF-8 与 UTF-16 的互转，和 core/utf8.cpp 里的**校验器**刻意分成两个目标。
#
# 理由在负载一侧：任务栏负载 DLL 要跑在 explorer.exe 里，它的审计要求是"这个模块里没有 JSON、
# 没有快照、没有协议校验器"。邮箱的消费者只需要把 Win32 的错误文本换成 UTF-8，渲染侧只需要把
# 协议文本换成宽字符，所以负载只链 `deskfeed_core_text`；`utf8_validate` / `is_blank` /
# `has_line_break` 这些校验器留在 core/utf8.cpp，一个都不会被拉进 explorer。
# 声明仍在 core/utf8.h，现有调用方（主进程、IPC、网络层、显示层）看不到任何变化。
add_library(deskfeed_core_text STATIC
    src/core/utf8_convert.cpp
)
target_link_libraries(deskfeed_core_text PUBLIC deskfeed_base)

# 纯逻辑层：协议解析与校验、健康/到期状态机、调度、UTF-8 判据。没有窗口，没有网络，可单独测。
add_library(deskfeed_core STATIC
    src/core/health.cpp
    src/core/json.cpp
    src/core/schedule.cpp
    src/core/snapshot.cpp
    src/core/utf8.cpp
)
target_link_libraries(deskfeed_core PUBLIC deskfeed_base deskfeed_core_text)

# 平台层：日志、Win32 帮手、用户范围路径与安全描述符、控制台输出、符号解析（Phase 2 用）。
add_library(deskfeed_platform STATIC
    src/platform/console.cpp
    src/platform/log.cpp
    src/platform/paths.cpp
    src/platform/user_scope.cpp
    src/platform/win32_util.cpp
)
target_link_libraries(deskfeed_platform PUBLIC deskfeed_base)

# 任务栏符号解析（Phase 2 的平台知识，与运行时无关，单独成库便于独立替换）。
add_library(deskfeed_symbols STATIC
    src/platform/pdb.cpp
    src/platform/symbols.cpp
    src/platform/taskbar_symbols.cpp
)
target_link_libraries(deskfeed_symbols PUBLIC deskfeed_platform deskfeed_dbghelp)

# 网络层：唯一的取数入口与地址处理。
add_library(deskfeed_net STATIC
    src/net/http_client.cpp
    src/net/url.cpp
)
target_link_libraries(deskfeed_net PUBLIC deskfeed_core deskfeed_platform deskfeed_winhttp)

# 启动/控制通道的线格式。刻意拆成"只编码"与"编码 + 解码"两个目标：
# 任务栏负载 DLL 只会发一个动作给主进程，它只链 `deskfeed_control_wire_send`——解码器里的
# `utf8_validate` 与接收端对象（`ControlServer` / `InstanceMutex`）都不进 explorer。
add_library(deskfeed_control_wire_send STATIC
    src/ipc/control_wire_encode.cpp
)
target_link_libraries(deskfeed_control_wire_send PUBLIC deskfeed_base deskfeed_core_text)

add_library(deskfeed_control_wire STATIC
    src/ipc/control_wire_decode.cpp
)
target_link_libraries(deskfeed_control_wire PUBLIC deskfeed_control_wire_send deskfeed_core deskfeed_platform)

# 进程间与配置层：命令行、控制通道、设置落盘、进程绑定。
add_library(deskfeed_ipc STATIC
    src/ipc/binding.cpp
    src/ipc/command.cpp
    src/ipc/control_channel.cpp
    src/ipc/settings.cpp
)
target_link_libraries(deskfeed_ipc PUBLIC deskfeed_core deskfeed_platform deskfeed_control_wire)

# 显示帧与显示邮箱（Phase 2 的 seam）：主进程是唯一生产者，任务栏负载 DLL 只读最新一帧。
# 分成独立目标，是为了让负载 DLL 能只链这一层与平台层，不把 JSON 解析器与协议校验器
# 一起塞进 explorer.exe。
add_library(deskfeed_display STATIC
    src/display/frame.cpp
    src/display/mailbox.cpp
    src/display/reading.cpp
)
target_link_libraries(deskfeed_display PUBLIC deskfeed_core deskfeed_platform)

# 任务栏负载 DLL：注入 explorer.exe，把主进程发布的最新显示帧渲染进任务栏的 XAML 树。
#
# 链的东西是刻意的、也是可审计的：
#   deskfeed_display          帧线格式、显示邮箱消费者、纯读数格式化器
#   deskfeed_platform         日志、用户范围路径、Win32 帮手
#   deskfeed_core_text        只有 UTF-8/UTF-16 转换（不是 core 的校验器）
#   deskfeed_control_wire_send 只有控制通道的编码那一半
# 没有 deskfeed_net、没有 deskfeed_ipc（接收端）、没有 deskfeed_symbols/dbghelp。静态库只按需拉取
# 目标文件，所以"最终 DLL 里没有 JSON / 快照 / 协议校验器 / 符号 / HTTP"这件事由**导入表与 map
# 文件**验收（`deskfeed-native-tests --audit <dll>` 就是这条审计的自动门禁）。
add_library(deskfeed_payload SHARED
    src/payload/action_worker.cpp
    src/payload/control_upstream.cpp
    src/payload/dllmain.cpp
    src/payload/feed.cpp
    src/payload/hook_post.cpp
    src/payload/host.cpp
    src/payload/reg_hook.cpp
    src/payload/taskbar_xaml.cpp
    src/payload/xaml_tap.cpp
)
target_link_libraries(deskfeed_payload PRIVATE
    deskfeed_display
    deskfeed_platform
    deskfeed_core_text
    deskfeed_control_wire_send
    # comctl32：悬停提示（TOOLTIPS_CLASS）。它是负载自己的窗口，不是 XAML 窗口——任务栏那个 XAML
    # 岛只有任务栏那么高，XAML 自己弹出的东西会被岛裁掉。
    comctl32.lib
    # C++/WinRT 的激活（XAML 类型、颜色、画刷）、单元初始化、调度队列。
    windowsapp.lib
    runtimeobject.lib
    coremessaging.lib
)

# 负载改写各模块的导入地址表，而那些调用是**指向本模块的间接调用**。开启 Control Flow Guard 的
# 进程会校验每一个间接调用目标，所以本模块必须带 /guard:cf，否则第一次被钩到的调用就会
# fast-fail 掉 explorer.exe。reg_hook 会把进程与本模块的 CFG 状态写进日志作为前提证据。
target_compile_options(deskfeed_payload PRIVATE /guard:cf)
target_link_options(deskfeed_payload PRIVATE /guard:cf)
# 这个警告来自 Windows SDK 自己的 C++/WinRT 投射头（Windows.UI.Xaml.Media.Animation.0.h 把
# GetCurrentTime 这个类函数宏当带参函数调用），不是本工程的代码。抑制它，好让"本工程零警告"这条
# 门禁继续可读。
target_compile_options(deskfeed_payload PRIVATE /wd4002)

set_target_properties(deskfeed_payload PROPERTIES
    OUTPUT_NAME "deskfeed-payload"
    PREFIX ""
)

# explorer 里的崩溃必须能归因到确切地址，所以负载总是带 map 文件。
target_link_options(deskfeed_payload PRIVATE "/MAP:${CMAKE_BINARY_DIR}/bin/deskfeed-payload.map")

# 清单作为源文件（见 src/payload/deskfeed-payload.manifest 里的理由）。
target_sources(deskfeed_payload PRIVATE
    src/payload/deskfeed-payload.manifest
)

# 主进程。WIN32 子系统：常驻的桌面程序不该带一个控制台窗口；
# 命令行用法（--help/--inspect/报错）在需要时挂到父进程控制台，见 platform/console.cpp。
add_executable(deskfeed WIN32
    src/master/main.cpp
    src/master/master.cpp
)
target_link_libraries(deskfeed PRIVATE deskfeed_ipc deskfeed_net deskfeed_display)

# 离线检查器与量具。
add_executable(deskfeed-check
    src/tools/check/main.cpp
)
target_link_libraries(deskfeed-check PRIVATE deskfeed_net)

# 离线用例。
add_executable(deskfeed-tests
    tests/tests.cpp
)
target_link_libraries(deskfeed-tests PRIVATE deskfeed_ipc deskfeed_net deskfeed_display ws2_32.lib)
target_include_directories(deskfeed-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME deskfeed-tests COMMAND deskfeed-tests)

# 原生侧的离线回归用例：显示帧消费策略、钩子门的收尾协议（真的起一条消息线程与一个 user32 类的
# 确认窗口，但不注入 explorer）、C++/SEH 边界，以及成品负载 DLL 的自动审计。
#
# 它刻意与 tests/tests.cpp 分成两个可执行文件：那一份归主进程/帧/配置的作者，这一份只放任务栏负载
# 这条路上的东西，两边互不踩。`src/payload/{feed,hook_post}.cpp` 在这里被**再编译一次**（它们不碰
# XAML，只依赖 Win32 与平台日志），所以测试不依赖负载 DLL 的加载。
add_executable(deskfeed-native-tests
    tests/native_tests.cpp
    src/payload/feed.cpp
    src/payload/hook_post.cpp
)
target_link_libraries(deskfeed-native-tests PRIVATE deskfeed_platform)
target_include_directories(deskfeed-native-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME deskfeed-native-tests COMMAND deskfeed-native-tests)

# 成品导入表/map 审计：单独一条 CTest，失败就是"负载里混进了不该有的东西"。
add_test(NAME deskfeed-payload-audit
    COMMAND deskfeed-native-tests --audit $<TARGET_FILE:deskfeed_payload>
)
