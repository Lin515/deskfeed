cmake_minimum_required(VERSION 3.20)

project(deskfeed LANGUAGES CXX)

# DeskFeed：通用 Windows 桌面信息显示。
#
# 当前阶段：协议/校验/状态机 + 单主进程 + 离线检查器、离线用例、任务栏负载 DLL
# （deskfeed-payload.dll，注入 explorer.exe）、注入器/探针工具与桌面悬浮窗都已实现；
# 真实注入的验收范围与安全边界见 docs/native-safety.md 与 docs/architecture.md 的路线表。
#
# 目标：
#   deskfeed          deskfeed.exe        主进程（单一取数、状态机、命令通道）
#   deskfeed-check    deskfeed-check.exe  离线协议检查器 / HTTP 量具
#   deskfeed-tests    deskfeed-tests.exe  离线用例
#
# x64 only：负载要与 64 位 explorer.exe 同位数，主进程与它保持一致。

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

# 应用图标：资源脚本与 .ico 同在 assets/，rc 按自身所在目录解析 "deskfeed.ico"（即使 cwd 是
# 构建目录也能找到）。OBJECT_DEPENDS 把 ico 补成 .res 的隐式依赖，否则只改图标不会重编资源。
# 只给两个交付 EXE（主进程与注入器）挂图标：诊断工具保持无图标，避免与产品本体混淆。
set(DESKFEED_APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/deskfeed.rc")
set(DESKFEED_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/deskfeed.ico")
set_source_files_properties("${DESKFEED_APP_ICON_RC}"
    PROPERTIES OBJECT_DEPENDS "${DESKFEED_APP_ICON_ICO}")

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
    src/platform/screens.cpp
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
    src/display/taskbar_layout.cpp
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
#
# **两个产物、一份源**：`deskfeed_payload`（生产）与 `deskfeed_payload_test`（自测）编译**同一份源
# 列表、同一 manifest、同一 /guard:cf 与 /MAP**，唯一差异是后者多一个 `DESKFEED_DLL_TEST_API`
# 定义。探针入口、完成事件变异、安装延迟、探针布防与 observer 只在那个定义下存在，所以生产 DLL 里
# 连这些入口都没有（不是"复位成正常"，而是根本不编译）。
set(DESKFEED_PAYLOAD_SOURCES
    src/payload/action_worker.cpp
    src/payload/control_upstream.cpp
    src/payload/dllmain.cpp
    src/payload/feed.cpp
    src/payload/guard_keeper.cpp
    src/payload/hook_post.cpp
    src/payload/host.cpp
    src/payload/reg_hook.cpp
    src/payload/taskbar_xaml.cpp
    src/payload/xaml_tap.cpp
)

# 两个负载目标共用的全部配置：链库、CFG、警告抑制、清单、输出名与 MAP。写成函数是为了让"两边只有
# 一个编译定义之差"这件事**在结构上**成立——任何人给其中一个加东西，都会立刻在另一个上缺失。
function(deskfeed_configure_payload target output_name)
    target_link_libraries(${target} PRIVATE
        deskfeed_display
        deskfeed_platform
        deskfeed_core_text
        deskfeed_control_wire_send
        # comctl32：悬停提示（TOOLTIPS_CLASS）。它是负载自己的窗口，不是 XAML 窗口——任务栏那个
        # XAML 岛只有任务栏那么高，XAML 自己弹出的东西会被岛裁掉。
        comctl32.lib
        # C++/WinRT 的激活（XAML 类型、颜色、画刷）、单元初始化、调度队列。
        windowsapp.lib
        runtimeobject.lib
        coremessaging.lib
    )

    # 负载改写各模块的导入地址表，而那些调用是**指向本模块的间接调用**。开启 Control Flow Guard
    # 的进程会校验每一个间接调用目标，所以本模块必须带 /guard:cf，否则第一次被钩到的调用就会
    # fast-fail 掉 explorer.exe。reg_hook 会把进程与本模块的 CFG 状态写进日志作为前提证据。
    target_compile_options(${target} PRIVATE /guard:cf)
    target_link_options(${target} PRIVATE /guard:cf)
    # 这个警告来自 Windows SDK 自己的 C++/WinRT 投射头（Windows.UI.Xaml.Media.Animation.0.h 把
    # GetCurrentTime 这个类函数宏当带参函数调用），不是本工程的代码。抑制它，好让"本工程零警告"
    # 这条门禁继续可读。
    target_compile_options(${target} PRIVATE /wd4002)

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
    )

    # explorer 里的崩溃必须能归因到确切地址，所以负载总是带 map 文件（两侧各一份，审计各读各的）。
    target_link_options(${target} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/bin/${output_name}.map")

    # 清单作为源文件（见 src/payload/deskfeed-payload.manifest 里的理由）。
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/payload/deskfeed-payload.manifest")
endfunction()

add_library(deskfeed_payload SHARED ${DESKFEED_PAYLOAD_SOURCES})
deskfeed_configure_payload(deskfeed_payload "deskfeed-payload")

# 自测 DLL：唯一差异就是这个定义。它保留探针 ABI 与全部对抗开关；生产 DLL 里没有它们。
add_library(deskfeed_payload_test SHARED ${DESKFEED_PAYLOAD_SOURCES})
deskfeed_configure_payload(deskfeed_payload_test "deskfeed-payload-test")
target_compile_definitions(deskfeed_payload_test PRIVATE DESKFEED_DLL_TEST_API=1)


# 主进程。WIN32 子系统：常驻的桌面程序不该带一个控制台窗口；
# 命令行用法（--help/--inspect/报错）在需要时挂到父进程控制台，见 platform/console.cpp。
#
# `taskbar_supervisor.cpp` 是任务栏组件的**主进程侧编排**（按有界命令行契约异步地请注入器干活），
# 它属于主进程而不是负载。悬浮窗清单必须是**源文件**（与负载同样的做法）：/MANIFESTINPUT 单独用
# 会 LNK1220，/MANIFEST:EMBED 又会和它重复（CVT1100）。链 `deskfeed_widget` 是因为主进程要显示
# 那个窗口本体。
add_executable(deskfeed WIN32
    src/master/main.cpp
    src/master/master.cpp
    src/master/taskbar_supervisor.cpp
    src/widget/deskfeed-widget.manifest
)
target_link_libraries(deskfeed PRIVATE
    deskfeed_ipc
    deskfeed_net
    deskfeed_display
    deskfeed_widget
)
target_sources(deskfeed PRIVATE "${DESKFEED_APP_ICON_RC}")

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
# 用例会真的起 `deskfeed.exe` 子进程（同目录），所以这是一条**真实图边**：只构建 deskfeed-tests
# 时必须把当前源码的主进程一起构建出来，否则 `ctest` 会跑到上一次构建留下的旧 exe 上。
add_dependencies(deskfeed-tests deskfeed)

enable_testing()
add_test(NAME deskfeed-tests COMMAND deskfeed-tests)

# 原生侧的离线回归用例：显示帧消费策略、钩子门的收尾协议（真的起一条消息线程与一个 user32 类的
# 确认窗口，但不注入 explorer）、C++/SEH 边界、导入表改写的"不自我递归 / 模块卸载后仍能恢复"，
# 以及成品负载 DLL 的自动审计。
#
# 它刻意与 tests/tests.cpp 分成两个可执行文件：那一份归主进程/帧/配置的作者，这一份只放任务栏负载
# 这条路上的东西，两边互不踩。`src/payload/{feed,hook_post,reg_hook,action_worker,control_upstream}.cpp`
# 在这里被**再编译一次**（它们不碰 XAML，只依赖 Win32、平台日志与控制通道的编码那一半），所以测试
# 不依赖负载 DLL 的加载；`reg_hook.cpp` 与 `action_worker.cpp` 里的自测探针由 `DESKFEED_NATIVE_TESTS`
# 开关，成品 DLL 里没有它们。
#
# `action_worker.cpp` 必须在场：回收门里"动作工作线程真的没在期限内退出"这条失败只能由**真实的
# 线程句柄超时**咬出来，用例不能用 mock 结果代替。
add_executable(deskfeed-native-tests
    tests/native_tests.cpp
    src/payload/action_worker.cpp
    src/payload/control_upstream.cpp
    src/payload/feed.cpp
    src/payload/hook_post.cpp
    src/payload/reg_hook.cpp
)
# 链 display：glue 用例要在本进程里当一次显示邮箱生产者，负载才会真的挂上邮箱、把动作工作线程
# 起来，然后一条真实的回收门失败才能被咬到。链 platform/control_wire_send 是既有口径。
target_link_libraries(deskfeed-native-tests PRIVATE deskfeed_platform deskfeed_control_wire_send
    deskfeed_display)
target_include_directories(deskfeed-native-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(deskfeed-native-tests PRIVATE DESKFEED_NATIVE_TESTS=1)

# **测试专用** fixture：一个只按名字导入 Reg* 三个入口、不链本工程任何库的小 DLL。用例把它
# LoadLibrary 进来，让 `reg_hook` 真的去补它的导入槽——这样"替身答话"与"模块卸载后再 restore
# 不崩"这两条硬路径才能被真的跑到。不部署、不安装，只是 CTest 的夹具。
add_library(deskfeed-reg-hook-caller SHARED
    tests/reg_hook_caller.cpp
)
# 这个夹具按名字导入 SDK 的注册表入口并**不**链本工程任何库，但编译口径必须与全仓一致：
# 少了 `deskfeed_base` 就没有 /utf-8 与 UNICODE/_UNICODE，源码里的宽字符字面量与 Win32 原型
# 在默认代码页下会报不成对引号/未声明标识符（真实 C2059/C1004）。
target_link_libraries(deskfeed-reg-hook-caller PRIVATE deskfeed_base)
set_target_properties(deskfeed-reg-hook-caller PROPERTIES
    OUTPUT_NAME "deskfeed-reg-hook-caller"
    PREFIX ""
)
add_dependencies(deskfeed-native-tests deskfeed-reg-hook-caller)
# 用例里有一条真的把**成品负载 DLL** LoadLibrary 进本进程的 status 用例，还有走测试 DLL 的 glue
# 用例，所以这两条也是**真实图边**：只构建 deskfeed-native-tests 时，两个 DLL 必须一起构建出来，
# 不能指望上一次构建在 `bin/` 里留下的旧产物。
add_dependencies(deskfeed-native-tests deskfeed_payload deskfeed_payload_test)

add_test(NAME deskfeed-native-tests COMMAND deskfeed-native-tests)

# 主进程侧编排（`TaskbarSupervisor`）的离线用例：`deskfeed-fake-injector` 是一个**不注入任何
# 进程**的假注入器（只按命令行写 argv 日志、按参数延时、返回指定退出码），
# `deskfeed-supervisor-tests` 用它跑完"装载/停止/状态/退避/收尾"那些场景。
# 两个独立可执行文件，各有自己的 main 与用例，不并入 `deskfeed-tests`。
add_executable(deskfeed-fake-injector
    tests/fake_injector.cpp
)
# 与仓库其余部分同一编译口径（/utf-8、UNICODE、/permissive-、基础 Win32 库）：没有
# `deskfeed_base` 时宽字符字面量与 Win32 原型在默认代码页下直接报 C2059/C1004。
target_link_libraries(deskfeed-fake-injector PRIVATE deskfeed_base)
# 主进程用例里有几条会真的起 `deskfeed-fake-injector.exe`（Q5：按 pid 记录假动作、不注入任何进程），
# 所以这是与 `deskfeed` 同性质的一条**真实图边**：只构建 deskfeed-tests 时必须把这个夹具一起构建
# 出来，否则干净克隆上的用例会跑到一个不存在的假注入器上（或上一次构建留下的旧产物上）。
add_dependencies(deskfeed-tests deskfeed-fake-injector)
add_executable(deskfeed-supervisor-tests
    tests/master_supervisor_tests.cpp
    src/master/taskbar_supervisor.cpp
)
target_link_libraries(deskfeed-supervisor-tests PRIVATE deskfeed_platform deskfeed_core_text)
target_include_directories(deskfeed-supervisor-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(deskfeed-supervisor-tests deskfeed-fake-injector)
add_test(NAME deskfeed-supervisor-tests COMMAND deskfeed-supervisor-tests)

# 成品导入表/map/导出名审计：两侧各一条 CTest。生产侧失败 = "负载里混进了不该有的东西"（含出现
# 了任何 DeskFeedProbe* 入口），测试侧失败 = 探针入口整批消失。
add_test(NAME deskfeed-payload-audit
    COMMAND deskfeed-native-tests --audit $<TARGET_FILE:deskfeed_payload>
)
add_test(NAME deskfeed-payload-test-audit
    COMMAND deskfeed-native-tests --audit $<TARGET_FILE:deskfeed_payload_test>
)

# ---------------------------------------------------------------------------
# 工具侧：注入器、自测探针与工具用例（源码在 src/inject、src/probe）。
#
# `deskfeed_inject_support` 是注入器与探针共用的静态库（目标解析、远程调用、PE 导出解析、
# 动作与状态探询）。探针也要链它：`src/probe/main.cpp` 用的是 `inject/target.h` 的
# `resolve_scope`，它是 target.cpp 里的非内联定义，少了这份实现就是 LNK2019。
add_library(deskfeed_inject_support STATIC
    src/inject/actions.cpp
    # per-target 租约 + 入口记录 + 粘性不确定事件（契约与布局的唯一出处是
    # `src/payload/guard_record.h`，这里只放工具侧的获取/写序/恢复实现）。
    src/inject/guard.cpp
    src/inject/payload_status.cpp
    src/inject/pe_export.cpp
    src/inject/remote.cpp
    src/inject/target.cpp
)
target_link_libraries(deskfeed_inject_support PUBLIC deskfeed_display deskfeed_platform)

add_executable(deskfeed-inject
    src/inject/main.cpp
)
target_link_libraries(deskfeed-inject PRIVATE deskfeed_inject_support)
target_sources(deskfeed-inject PRIVATE "${DESKFEED_APP_ICON_RC}")

# 自测探针：自带 Win32 窗口与系统 XAML 岛，绝不注入 explorer。清单作为**源文件**（与负载和
# 悬浮窗窗口用例同样的做法：/MANIFESTINPUT 单独用会 LNK1220，/MANIFEST:EMBED 会与它重复）。
# 探针 EXE 可以链 JSON/core（离线量具要解析严格快照并跑健康状态机），负载 DLL 不行。
add_executable(deskfeed-probe
    src/probe/main.cpp
    src/probe/host.cpp
    src/probe/offline.cpp
    src/probe/mock_producer.cpp
    src/probe/probe_hooks.cpp
    src/probe/deskfeed-probe.manifest
)
target_link_libraries(deskfeed-probe PRIVATE
    deskfeed_inject_support
    deskfeed_display
    deskfeed_core
    deskfeed_platform
    # 探针自己起宿主：系统 XAML 岛、调度队列与 WinRT 单元都在它这边。
    windowsapp.lib
    runtimeobject.lib
    coremessaging.lib
    comctl32.lib
)
# 探针会走真实的负载探针入口与 XAML 路径，与负载一样开 CFG（编译 + 链接）。
target_compile_options(deskfeed-probe PRIVATE /guard:cf)
target_link_options(deskfeed-probe PRIVATE /guard:cf)

# **测试专用**夹具：给"远程调用超时之后仍在写调用方缓冲"这条路径提供真实的延迟写者
# （`tests/remote_delay_writer.cpp`）。必须是**另一个模块**里的导出：只有真的存在一个"先睡、后写"
# 的函数，工具用例才能确定性地制造"调用已超时、写入还没发生"的窗口。它只被工具用例按同目录路径
# LoadLibrary 进子进程，不部署、不安装、不进入任何交付二进制。
#
# 与仓库其余部分同一编译口径（/utf-8、UNICODE、/permissive-、基础 Win32 库）：少了 `deskfeed_base`
# 时宽字符字面量与 Win32 原型在默认代码页下直接报 C2059/C1004。
add_library(deskfeed-test-remote-writer MODULE
    tests/remote_delay_writer.cpp
)
target_link_libraries(deskfeed-test-remote-writer PRIVATE deskfeed_base)
set_target_properties(deskfeed-test-remote-writer PROPERTIES
    OUTPUT_NAME "deskfeed-test-remote-writer"
    PREFIX ""
)

# 工具用例的另外两个**测试专用**夹具（与上面 remote-writer 同一套纪律：不部署、不安装、不进任何
# 交付二进制）：
#   * `deskfeed-test-fixture-payload`（MODULE）扮演被注入的负载：只导出真实的生产 ABI
#     （`DeskFeedStart` / `DeskFeedStop` / `DeskFeedGetStatus`），行为开关全部走环境变量，所以同一份
#     DLL 能演"立刻返回"、"卡在闸门上等放行"、"报未退休"等全部形状；
#   * `deskfeed-tools-fixture-host` 是隐藏的夹具主机：只把那个 DLL 装进自己然后等在那里，给动作路径
#     一个**真实**的远程目标（真实模块表 + 真实 `CreateRemoteThread` + 真实可被结束）。它绝不碰真实
#     explorer.exe：目标恒为用例自己 spawn 的进程。
# 两者都由 `deskfeed-tools-tests.exe` 按"与本可执行文件同目录"拉起（默认输出目录就是 `bin/`），
# 所以必须与本用例一起构建出来。
add_library(deskfeed-test-fixture-payload MODULE
    tests/fixture_payload.cpp
)
target_link_libraries(deskfeed-test-fixture-payload PRIVATE deskfeed_base)
set_target_properties(deskfeed-test-fixture-payload PROPERTIES
    OUTPUT_NAME "deskfeed-test-fixture-payload"
    PREFIX ""
)

add_executable(deskfeed-tools-fixture-host
    tests/tools_fixture_host.cpp
)
target_link_libraries(deskfeed-tools-fixture-host PRIVATE deskfeed_base)

# 工具用例（`deskfeed-tools-tests`）另外两个**测试专用**夹具，与上面同一套纪律：不部署、不安装、
# 不进任何交付二进制。两者都必须真的构建出来——工具用例是按"与本可执行文件同目录"的绝对路径去
# 拉起它们的，缺一个就是硬失败，不是跳过：
#   * `deskfeed-probe-producer-child`（EXE）是"独立帧生产者进程"，给"邮箱属主死掉"那条路径提供
#     一个真实的、由用例自己 spawn 的属主（用真实的 scope 仲裁名先抢名字再发帧）；它链
#     `deskfeed_display`（帧生产者与邮箱）、`deskfeed_core_text`、`deskfeed_base`；
#   * `deskfeed-test-retained-fixture`（MODULE）扮演一个"Stop 说未退休"的负载：只导出真实的生产
#     ABI，DllMain 里只写普通 detach 标记，用来咬住"探针绝不 `FreeLibrary` 一个未退休的模块"。
# 默认输出目录就是 `bin/`，与用例同目录。
add_executable(deskfeed-probe-producer-child
    tests/probe_producer_child.cpp
)
target_link_libraries(deskfeed-probe-producer-child PRIVATE
    deskfeed_display
    deskfeed_core_text
    deskfeed_base
)

add_library(deskfeed-test-retained-fixture MODULE
    tests/probe_retained_fixture.cpp
)
target_link_libraries(deskfeed-test-retained-fixture PRIVATE deskfeed_base)
set_target_properties(deskfeed-test-retained-fixture PROPERTIES
    OUTPUT_NAME "deskfeed-test-retained-fixture"
    PREFIX ""
)

add_executable(deskfeed-tools-tests
    tests/tools_tests.cpp
)
target_link_libraries(deskfeed-tools-tests PRIVATE deskfeed_inject_support)
# 工具用例按"与本可执行文件同目录"找这些夹具（默认输出目录就是 `bin/`），所以必须一起构建出来；
# 少一个工具用例就会在干净克隆上直接硬红。
add_dependencies(deskfeed-tools-tests deskfeed-test-remote-writer
    deskfeed-test-fixture-payload deskfeed-tools-fixture-host
    deskfeed-probe-producer-child deskfeed-test-retained-fixture)
add_test(NAME deskfeed-tools-tests COMMAND deskfeed-tools-tests)

# ---------------------------------------------------------------------------
# 桌面悬浮窗（Phase 3）。
#
# `deskfeed_widget` 是**静态库**：主进程与两个测试目标都链它，绝不把它的实现直接编进负载 DLL
# （负载只链 display/platform/core_text/control_wire_send，见上面的分层）。链 ipc 是因为界面偏好
# 走设置文件那一层；链 display 是因为它渲染的就是显示帧格式化出来的读数。
add_library(deskfeed_widget STATIC
    src/widget/fullscreen.cpp
    src/widget/preferences.cpp
    src/widget/text_metrics.cpp
    src/widget/theme.cpp
    src/widget/window.cpp
)
target_link_libraries(deskfeed_widget PUBLIC
    deskfeed_display
    deskfeed_ipc
    deskfeed_platform
    deskfeed_core
    # 悬浮窗自己的窗口、菜单与悬停提示。
    comctl32.lib
    dwmapi.lib
    gdiplus.lib
)

# 两个悬浮窗用例目标都**直接链静态库**，不重复编译 widget 的源文件：重复编译会带来两份实现，
# 链接期就会撞符号（duplicate symbol）。它们的 include 都从仓库根开始，所以只需要根做包含目录。
add_executable(deskfeed-widget-tests
    tests/widget_tests.cpp
)
target_link_libraries(deskfeed-widget-tests PRIVATE deskfeed_widget)
target_include_directories(deskfeed-widget-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# 隐藏窗口门禁：真的建窗口（`force_hidden = true`，绝不出现在桌面上）与悬停提示，所以清单必须是
# **源文件**（与负载同样的做法）：/MANIFESTINPUT 单独用会 LNK1220，/MANIFEST:EMBED 又会和它重复
# （CVT1100）。作为源文件合进去，trustInfo 与 comctl32 v6 的依赖就都在了。
add_executable(deskfeed-widget-window-tests
    tests/widget_window_tests.cpp
    src/widget/deskfeed-widget.manifest
)
target_link_libraries(deskfeed-widget-window-tests PRIVATE
    deskfeed_widget
    # 悬停提示与主题取色。
    comctl32.lib
    dwmapi.lib
    gdiplus.lib
    user32.lib
)
target_include_directories(deskfeed-widget-window-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME deskfeed-widget-tests COMMAND deskfeed-widget-tests)
add_test(NAME deskfeed-widget-window-tests COMMAND deskfeed-widget-window-tests)
