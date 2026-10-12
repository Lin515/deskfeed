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

# 纯逻辑层：协议解析与校验、健康/到期状态机、调度、UTF-8 判据。没有窗口，没有网络，可单独测。
add_library(deskfeed_core STATIC
    src/core/health.cpp
    src/core/json.cpp
    src/core/schedule.cpp
    src/core/snapshot.cpp
    src/core/utf8.cpp
)
target_link_libraries(deskfeed_core PUBLIC deskfeed_base)

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

# 进程间与配置层：命令行、控制通道、设置落盘、进程绑定。
add_library(deskfeed_ipc STATIC
    src/ipc/binding.cpp
    src/ipc/command.cpp
    src/ipc/control_channel.cpp
    src/ipc/settings.cpp
)
target_link_libraries(deskfeed_ipc PUBLIC deskfeed_core deskfeed_platform)

# 显示帧与显示邮箱（Phase 2 的 seam）：主进程是唯一生产者，任务栏负载 DLL 只读最新一帧。
# 分成独立目标，是为了让负载 DLL 能只链这一层与平台层，不把 JSON 解析器与协议校验器
# 一起塞进 explorer.exe。
add_library(deskfeed_display STATIC
    src/display/frame.cpp
    src/display/mailbox.cpp
)
target_link_libraries(deskfeed_display PUBLIC deskfeed_core deskfeed_platform)

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
