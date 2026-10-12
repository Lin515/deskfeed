#include "probe/host.h"

#include "platform/log.h"
#include "platform/win32_util.h"
#include "probe/observer_pod.h"
#include "probe/probe_hooks.h"

#include <windows.h>

#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <dispatcherqueue.h>

// windows.h 里 16 位时代留下的 GetCurrentTime 宏与 C++/WinRT 投影同名，先撤掉它。
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <vector>

namespace deskfeed::probe {
namespace {

namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;

constexpr wchar_t kWindowClass[] = L"DeskFeedProbeHost";
constexpr UINT kRunMessage = WM_APP + 1;
// UI 线程"停止取消息"：在窗口过程里睡，就一定不会检索消息 —— 这正是对抗"任务栏线程被挂住"的
// 形状（钩子记录留在链上，收尾拿不到完成证明）。判断必须在 PeekMessage **之前**生效。
constexpr UINT kHangMessage = WM_APP + 2;

// 负载给自测宿主用的一组导出（冻结 ABI，见 src/payload/dllmain.cpp）。
struct ProbeExports {
    int(__stdcall* start)() = nullptr;
    int(__stdcall* sites)() = nullptr;
    int(__stdcall* watch)() = nullptr;
    int(__stdcall* run)(int) = nullptr;
    void(__stdcall* set_host)(HWND) = nullptr;
    void(__stdcall* set_slow_apply)(unsigned) = nullptr;
    int(__stdcall* post_apply)(unsigned, unsigned) = nullptr;
    int(__stdcall* retire)(unsigned) = nullptr;
    int(__stdcall* hook)(int) = nullptr;
    int(__stdcall* block_visible)() = nullptr;
    int(__stdcall* teardown)(unsigned) = nullptr;
    void(__stdcall* stop)() = nullptr;
    int(__stdcall* dangling)() = nullptr;
    // 生产入口与（待负载作者冻结的）probe 专用入口。
    DWORD(WINAPI* feed_start)(void*) = nullptr;
    DWORD(WINAPI* feed_stop)(void*) = nullptr;
    void(__stdcall* set_tray_window)(HWND) = nullptr;
    void(__stdcall* set_install_delay)(unsigned) = nullptr;
    void(__stdcall* set_observer)(HookPhasePod*) = nullptr;
    void(__stdcall* set_completion_mutation)(unsigned) = nullptr;
};

HostOptions g_options;
ProbeExports g_probe;
HMODULE g_module = nullptr;
HWND g_window = nullptr;
HWND g_ack = nullptr;
DWORD g_ui_thread = 0;
HANDLE g_ui_done = nullptr;
bool g_feed_retired = false;
bool g_failed = false;
bool g_dump_only = false;
HookPhasePod g_phase{};

struct ExtraCore {
    HANDLE thread = nullptr;
    HANDLE ready = nullptr;
    bool ok = false;
};
std::vector<ExtraCore*> g_extra_cores;

std::wstring directory_of(std::wstring const& path) {
    std::wstring::size_type const slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// ---- XAML 岛的宿主 -------------------------------------------------------

// 与 shell 同形状的最小树：根 Grid 里一个名为 SystemTrayFrameGrid 的 Grid，里面已有一个子元素，
// 用来验证"插一列并把既有子元素右移"。
wuxc::Grid build_content() {
    wuxc::Grid tray;
    tray.Name(L"SystemTrayFrameGrid");
    wuxc::TextBlock label;
    label.Text(L"tray");
    tray.Children().Append(label);

    wuxc::Grid root;
    root.Name(L"RootGrid");
    root.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(
        winrt::Windows::UI::ColorHelper::FromArgb(255, 28, 28, 28)));
    root.Children().Append(tray);
    return root;
}

// 额外核心的树用**不同的名字**：三棵一模一样的树都会上报 SystemTrayFrameGrid，走查的最后一个
// 写入者属于谁不确定，解析那一半就无从核对；换名字只是让这个宿主对自己在量什么保持诚实。
wuxc::Grid build_extra_content() {
    wuxc::TextBlock label;
    label.Text(L"tray");
    wuxc::Grid panel;
    panel.Name(L"ExtraCorePanel");
    panel.Children().Append(label);
    wuxc::Grid root;
    root.Name(L"ExtraCoreRootGrid");
    root.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(
        winrt::Windows::UI::ColorHelper::FromArgb(255, 28, 28, 28)));
    root.Children().Append(panel);
    return root;
}

DWORD WINAPI extra_core_entry(LPVOID parameter) {
    auto* core = static_cast<ExtraCore*>(parameter);
    bool signalled = false;
    auto signal_ready = [core, &signalled]() {
        if (!signalled) {
            signalled = true;
            ::SetEvent(core->ready);
        }
    };
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        DispatcherQueueOptions queue_options{};
        queue_options.dwSize = sizeof(queue_options);
        queue_options.threadType = DQTYPE_THREAD_CURRENT;
        queue_options.apartmentType = DQTAT_COM_NONE;
        winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> controller;
        winrt::check_hresult(::CreateDispatcherQueueController(
            queue_options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(controller.put())));

        HWND window = ::CreateWindowExW(0, kWindowClass, L"deskfeed probe extra core",
                                        WS_OVERLAPPEDWINDOW, -32000, -32000, 320, 120, nullptr,
                                        nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (window == nullptr) {
            signal_ready();
            return 0;
        }
        auto manager = wuxh::WindowsXamlManager::InitializeForCurrentThread();
        wuxh::DesktopWindowXamlSource source;
        winrt::com_ptr<IDesktopWindowXamlSourceNative> native;
        HRESULT attached = reinterpret_cast<::IUnknown*>(winrt::get_abi(source))->QueryInterface(
            __uuidof(IDesktopWindowXamlSourceNative), native.put_void());
        if (SUCCEEDED(attached)) {
            attached = native->AttachToWindow(window);
        }
        if (FAILED(attached)) {
            signal_ready();
            return 0;
        }
        source.Content(build_extra_content());
        core->ok = true;
        signal_ready();

        MSG message{};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    } catch (...) {
        // 探针绝不允许异常逃出线程入口。
    }
    signal_ready();
    return 0;
}

bool create_extra_cores(unsigned target) {
    for (unsigned index = 1; index < target; ++index) {
        auto* core = new (std::nothrow) ExtraCore();
        if (core == nullptr) {
            return false;
        }
        core->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        core->thread = ::CreateThread(nullptr, 0, extra_core_entry, core, 0, nullptr);
        if (core->ready == nullptr || core->thread == nullptr) {
            return false;
        }
        g_extra_cores.push_back(core);
        if (::WaitForSingleObject(core->ready, 15000) != WAIT_OBJECT_0 || !core->ok) {
            return false;
        }
    }
    return true;
}

// ---- 对抗与验收 ---------------------------------------------------------

bool resolve_exports(HMODULE module) {
    g_probe.start = reinterpret_cast<int(__stdcall*)()>(::GetProcAddress(module, "DeskFeedProbeStart"));
    g_probe.sites = reinterpret_cast<int(__stdcall*)()>(::GetProcAddress(module, "DeskFeedProbeSites"));
    g_probe.watch = reinterpret_cast<int(__stdcall*)()>(::GetProcAddress(module, "DeskFeedProbeWatch"));
    g_probe.run = reinterpret_cast<int(__stdcall*)(int)>(::GetProcAddress(module, "DeskFeedProbeRun"));
    g_probe.set_host = reinterpret_cast<void(__stdcall*)(HWND)>(
        ::GetProcAddress(module, "DeskFeedProbeSetHost"));
    g_probe.set_slow_apply = reinterpret_cast<void(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeSetSlowApply"));
    g_probe.post_apply = reinterpret_cast<int(__stdcall*)(unsigned, unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbePostApply"));
    g_probe.retire = reinterpret_cast<int(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeRetire"));
    g_probe.hook = reinterpret_cast<int(__stdcall*)(int)>(::GetProcAddress(module, "DeskFeedProbeHook"));
    g_probe.block_visible = reinterpret_cast<int(__stdcall*)()>(
        ::GetProcAddress(module, "DeskFeedProbeBlockVisible"));
    g_probe.teardown = reinterpret_cast<int(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeTeardown"));
    g_probe.stop = reinterpret_cast<void(__stdcall*)()>(::GetProcAddress(module, "DeskFeedProbeStop"));
    g_probe.dangling = reinterpret_cast<int(__stdcall*)()>(
        ::GetProcAddress(module, "DeskFeedProbeDangling"));
    g_probe.feed_start = reinterpret_cast<DWORD(WINAPI*)(void*)>(
        ::GetProcAddress(module, "DeskFeedStart"));
    g_probe.feed_stop = reinterpret_cast<DWORD(WINAPI*)(void*)>(
        ::GetProcAddress(module, "DeskFeedStop"));
    g_probe.set_tray_window = reinterpret_cast<void(__stdcall*)(HWND)>(
        ::GetProcAddress(module, "DeskFeedProbeSetTrayWindow"));
    g_probe.set_install_delay = reinterpret_cast<void(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeSetInstallDelay"));
    g_probe.set_observer = reinterpret_cast<void(__stdcall*)(HookPhasePod*)>(
        ::GetProcAddress(module, "DeskFeedProbeSetObserver"));
    g_probe.set_completion_mutation = reinterpret_cast<void(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeSetCompletionMutation"));
    return g_probe.start != nullptr && g_probe.sites != nullptr && g_probe.watch != nullptr &&
           g_probe.run != nullptr && g_probe.set_host != nullptr && g_probe.stop != nullptr;
}

// 第三方钩子 + install-race + 提前 done 的对抗序列。返回 true 表示观测符合预期。
bool run_ordering_case() {
    if (!g_options.third_party || g_probe.post_apply == nullptr || g_probe.retire == nullptr) {
        return true;
    }
    HookPhasePod* const phase = g_probe.set_observer != nullptr ? &g_phase : nullptr;
    if (phase != nullptr) {
        g_phase = HookPhasePod{};
        g_phase.size = sizeof(HookPhasePod);
        g_phase.abi = kHookPhaseAbi;
        g_probe.set_observer(phase);
    }
    if (g_probe.set_install_delay != nullptr && g_options.install_delay_ms != 0) {
        g_probe.set_install_delay(g_options.install_delay_ms);
    }
    if (g_probe.set_completion_mutation != nullptr) {
        g_probe.set_completion_mutation(g_options.mutation_early_done ? 1u : 0u);
    }

    // **第三方钩子先装**，负载的钩子后装 → 负载的 CallNextHookEx 会到达第三方。
    std::wstring error;
    if (!third_party_install(g_ui_thread, phase, g_ack, error)) {
        deskfeed::log_format(L"ordering: cannot install the third-party hook: %s", error.c_str());
        if (g_probe.set_completion_mutation != nullptr) {
            g_probe.set_completion_mutation(0);
        }
        return false;
    }

    int const posted = g_probe.post_apply(g_ui_thread, 5000);
    int const retired = g_probe.retire(5000);
    ThirdPartyObservation const third = third_party_observation();
    AckObservation const ack = ack_observation();
    std::wstring const stop_error;
    third_party_uninstall(error);
    if (g_probe.set_completion_mutation != nullptr) {
        g_probe.set_completion_mutation(0);  // 收尾前强制复位：failpoint 绝不留到下一轮
    }

    deskfeed::log_format(L"ordering: post=%d retire=%d entries=%u pumps=%u sentinels=%u "
                        L"ping_in_frame=%u ack_hits=%u ack_in_frame=%u",
                        posted, retired, third.entries, third.pumps, third.sentinels,
                        third.ping_in_frame, ack.hits, ack.hits_while_third_party_active);

    if (g_options.mutation_early_done) {
        // 对抗：done 被提前，收尾会在负载的 CallNext 帧里被处理 —— 必须**观察到**这个症状，
        // 否则说明量具根本没咬住。
        bool const bit = ack.hits_while_third_party_active > 0 || third.ping_in_frame > 0;
        if (!bit) {
            deskfeed::log_line(L"ordering: the early-done mutation was NOT observable; the "
                               L"instrument does not bite");
            return false;
        }
        deskfeed::log_line(L"ordering: the early-done mutation is observable (hard failure seen)");
        return true;
    }

    // 正常顺序：第三方在 CallNext 里跑过（entries>0），但 ack 绝不能在 CallNext 帧里被处理。
    if (third.entries == 0) {
        deskfeed::log_line(L"ordering: the third-party hook never ran; the chain order is wrong");
        return false;
    }
    if (ack.hits_while_third_party_active != 0 || third.ping_in_frame != 0) {
        deskfeed::log_line(L"ordering: the retire ping was handled inside the payload's CallNext "
                           L"frame; the completion gate is not doing its job");
        return false;
    }
    return retired == 1;
}

// UI 线程上：解析 + 应用（结构模式），或只跑对抗序列（feed 模式由负载自己应用）。
bool run_on_ui() {
    if (g_probe.set_slow_apply != nullptr && g_options.slow_apply_ms != 0) {
        g_probe.set_slow_apply(g_options.slow_apply_ms);
    }
    if (!g_options.feed) {
        g_probe.set_host(g_ack);
        if (g_probe.run(g_dump_only ? 1 : 0) != 1) {
            deskfeed::log_line(L"run: the payload did not apply the reading");
            return false;
        }
        deskfeed::log_line(L"run: ok");
    }

    // 观测窗口：原地抽消息而不是 Sleep（应用与更新都靠这条线程的消息）。
    if (g_options.seconds > 0) {
        DWORD const deadline = ::GetTickCount() + g_options.seconds * 1000;
        MSG message{};
        while (static_cast<long>(::GetTickCount() - deadline) < 0) {
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    if (g_options.teardown && !g_options.feed && g_probe.teardown != nullptr) {
        int const ok = g_probe.teardown(5000);
        deskfeed::log_format(L"teardown: %s", ok == 1 ? L"retired" : L"NOT retired");
        if (ok != 1) {
            g_failed = true;
        }
    }
    if (g_ui_done != nullptr) {
        ::SetEvent(g_ui_done);
    }
    return true;
}

bool run_on_ui_guarded() {
    __try {
        return run_on_ui();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case kRunMessage:
            if (!run_on_ui_guarded()) {
                g_failed = true;
                deskfeed::log_line(L"the probe faulted on the UI thread");
            }
            ::DestroyWindow(window);
            return 0;
        case kHangMessage:
            // 停在窗口过程里 = 这条线程不检索消息。负载的钩子记录留在链上，收尾会超时（0）。
            ::Sleep(g_options.hang_ui_ms);
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(window, message, wparam, lparam);
    }
}

// "任务栏线程被挂住"的对抗：短超时的 post/retire 必须**未退休**（0），恢复之后重试必须成功。
bool run_hang_case() {
    if (g_options.hang_ui_ms == 0 || g_probe.post_apply == nullptr || g_probe.retire == nullptr) {
        return true;
    }
    ::PostMessageW(g_window, kHangMessage, 0, 0);
    ::Sleep(80);  // 让 UI 线程进到"不取消息"的窗口里

    int const short_post = g_probe.post_apply(g_ui_thread, 300);
    int const short_retire = g_probe.retire(300);
    deskfeed::log_format(L"hang: short post=%d retire=%d (both must be 0 = retained)", short_post,
                         short_retire);
    bool ok = short_post == 0 && short_retire == 0;

    ::Sleep(g_options.hang_ui_ms + 100);  // 等挂起窗口过去
    int const retry_post = g_probe.post_apply(g_ui_thread, 5000);
    int const retry_retire = g_probe.retire(5000);
    deskfeed::log_format(L"hang: retry post=%d retire=%d (both must be 1)", retry_post, retry_retire);
    ok = ok && retry_post == 1 && retry_retire == 1;
    return ok;
}

// 工作线程：装载负载、连接诊断、**唯一一次**走查，然后交给 UI 线程。
DWORD WINAPI worker_entry(LPVOID) {
    HMODULE const module = ::LoadLibraryExW(g_options.dll_path.c_str(), nullptr,
                                           LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        deskfeed::log_format(L"cannot load the payload: %s", deskfeed::last_error_text().c_str());
        g_failed = true;
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        return 0;
    }
    g_module = module;

    if (!resolve_exports(module)) {
        deskfeed::log_line(L"the payload does not export the probe entry points");
        g_failed = true;
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        return 0;
    }

    if (g_options.hook && g_probe.hook != nullptr) {
        deskfeed::log_format(L"reg hook: %s", g_probe.hook(1) == 1 ? L"installed" : L"refused");
    }
    if (g_probe.set_slow_apply != nullptr && g_options.slow_apply_ms != 0) {
        // 慢执行体是给"挂着的时候 stop 必须未退休"用的，越早设越好。
        g_probe.set_slow_apply(g_options.slow_apply_ms);
    }

    if (g_options.feed) {
        // 生产路径：负载自己 attach mock 邮箱、自己把帧渲染进宿主的岛。**不做** inline 应用：
        // XAML 元素有线程亲和性，内容必须由负载经真实 hook 编组到 UI 线程。
        if (g_probe.set_tray_window == nullptr || g_probe.feed_start == nullptr) {
            deskfeed::log_line(L"the payload does not export the probe tray-window entry point; "
                               L"feed mode needs it");
            g_failed = true;
            ::PostMessageW(g_window, kRunMessage, 0, 0);
            return 0;
        }
        g_probe.set_tray_window(g_window);
        std::wstring scope = g_options.scope;
        DWORD const started = g_probe.feed_start(&scope);
        deskfeed::log_format(L"DeskFeedStart -> %lu (%s)", started,
                             started == 1 ? L"started" : L"not started");
        if (started != 1 && started != 3) {
            g_failed = true;
        }
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);

        // 收尾的对抗：挂住 UI / 让 apply 变慢时，**短超时**的 stop 必须报"未退休"（0），
        // 恢复之后重试必须报"可以卸载"（1）。绝不在这两种情况下强卸模块。
        if (g_probe.feed_stop != nullptr &&
            (g_options.slow_apply_ms != 0 || g_options.hang_ui_ms != 0)) {
            if (g_options.hang_ui_ms != 0) {
                ::PostMessageW(g_window, kHangMessage, 0, 0);
            }
            ::Sleep(80);
            DWORD const short_stop = g_probe.feed_stop(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(400)));
            deskfeed::log_format(L"stop (short timeout, must be 0 = retained) -> %lu", short_stop);
            bool ok = short_stop == 0;
            ::Sleep(g_options.hang_ui_ms != 0 ? g_options.hang_ui_ms + 100 : 300);
            DWORD const retry = g_probe.feed_stop(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(10000)));
            deskfeed::log_format(L"stop (retry, must be 1 = retired) -> %lu", retry);
            ok = ok && retry == 1;
            g_feed_retired = retry == 1;
            if (!ok) {
                g_failed = true;
            }
        }
        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        return 0;
    }

    g_probe.start();
    int sites = 0;
    for (int attempt = 0; attempt < 100 && sites < static_cast<int>(g_options.cores); ++attempt) {
        ::Sleep(100);
        sites = g_probe.sites();
    }
    deskfeed::log_format(L"diagnostics sites after waiting: %d (wanted %u)", sites, g_options.cores);
    if (sites < static_cast<int>(g_options.cores)) {
        deskfeed::log_format(L"only %d of %u core(s) owned by this process attached a site", sites,
                             g_options.cores);
    }

    // 一次注入只走查一次，**不重试**：重试就是又一次走查，会把风险最高的操作重复一遍。
    int const watched = g_probe.watch();
    deskfeed::log_format(L"watch: %s", watched == 1 ? L"the tray element was reported"
                                                    : L"failed (not retried)");
    ::PostMessageW(g_window, kRunMessage, 0, 0);
    ::WaitForSingleObject(g_ui_done, 30000);

    // 对抗都在**工作线程**上跑：UI 线程必须能自由取消息，否则"挂住"这个形状自己就没了。
    if (!run_hang_case()) {
        g_failed = true;
    }
    if (!run_ordering_case()) {
        g_failed = true;
    }
    ::PostMessageW(g_window, WM_CLOSE, 0, 0);
    return 0;
}

}  // namespace

// 收尾判据：逐次 FreeLibrary 直到失败。这是 loader 自己记的账，不是预测：健康路径必须是 1
// （那一次是探针自己的 LoadLibrary；XAML 为每个 site 记的引用已由负载在解析阶段还清）。
void report_module_references(HMODULE module) {
    if (module == nullptr) {
        deskfeed::log_line(L"the payload was never loaded");
        return;
    }
    // 取路径要在卸载之前：最后一次成功的 FreeLibrary 已经把模块的代码与数据一起拿走。
    std::wstring const path = deskfeed::module_file_path(module);
    std::wstring const name = path.substr(path.find_last_of(L"\\/") + 1);

    int references = 0;
    while (::FreeLibrary(module)) {
        ++references;
    }
    deskfeed::log_format(L"payload module references at the end: %d", references);
    deskfeed::log_format(L"the payload module (%s) is %s now", name.c_str(),
                         ::GetModuleHandleW(name.c_str()) == nullptr ? L"fully unloaded"
                                                                    : L"still mapped");
}

int run_host(HostOptions const& options) {
    g_options = options;
    g_dump_only = options.dump_only;
    g_ui_thread = ::GetCurrentThreadId();

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // XAML 核心绑定"创建它时当前的那个调度队列"，所以队列必须先建。
    DispatcherQueueOptions queue_options{};
    queue_options.dwSize = sizeof(queue_options);
    queue_options.threadType = DQTYPE_THREAD_CURRENT;
    queue_options.apartmentType = DQTAT_COM_NONE;
    winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> controller;
    HRESULT const queue_result = ::CreateDispatcherQueueController(
        queue_options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(controller.put()));
    if (FAILED(queue_result)) {
        deskfeed::log_format(L"CreateDispatcherQueueController -> %s",
                             deskfeed::hresult_text(queue_result).c_str());
        return 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&window_class) == 0) {
        deskfeed::log_format(L"RegisterClassEx failed: %s", deskfeed::last_error_text().c_str());
        return 1;
    }

    // 宿主窗口**从不 ShowWindow**：自测全部在隐藏窗口里完成，桌面栈上不留任何可见痕迹。
    g_window = ::CreateWindowExW(0, kWindowClass, L"deskfeed probe host", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 640, 200, nullptr, nullptr,
                                 window_class.hInstance, nullptr);
    if (g_window == nullptr) {
        deskfeed::log_format(L"CreateWindowEx failed: %s", deskfeed::last_error_text().c_str());
        return 1;
    }

    // 收尾 ping 的确认窗口：**user32 的 STATIC**，属于这条 UI 线程；它在 probe_hooks 里被
    // 宿主自己子类化（底层仍是系统过程，绝不用负载模块的窗口过程当确认目标）。
    g_ack = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 16, 16, g_window,
                              nullptr, window_class.hInstance, nullptr);
    if (g_ack == nullptr) {
        deskfeed::log_format(L"cannot create the ack window: %s",
                             deskfeed::last_error_text().c_str());
        return 1;
    }

    auto manager = wuxh::WindowsXamlManager::InitializeForCurrentThread();
    wuxh::DesktopWindowXamlSource source;
    winrt::com_ptr<IDesktopWindowXamlSourceNative> native;
    HRESULT attached = reinterpret_cast<::IUnknown*>(winrt::get_abi(source))->QueryInterface(
        __uuidof(IDesktopWindowXamlSourceNative), native.put_void());
    if (SUCCEEDED(attached)) {
        attached = native->AttachToWindow(g_window);
    }
    if (FAILED(attached)) {
        deskfeed::log_format(L"the XAML island could not be attached: %s",
                             deskfeed::hresult_text(attached).c_str());
        return 1;
    }
    source.Content(build_content());
    deskfeed::log_line(L"XAML island attached (window stays hidden)");

    if (options.cores > 1 && !create_extra_cores(options.cores)) {
        deskfeed::log_line(L"cannot bring up the extra XAML cores");
        return 1;
    }
    deskfeed::log_format(L"this process owns %u XAML core(s)", options.cores);

    // UI 线程跑完"解析 + 应用"之后置位；工作线程靠它决定何时开始对抗序列。
    g_ui_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_ui_done == nullptr) {
        deskfeed::log_line(L"cannot create the UI-done event");
        return 1;
    }

    HANDLE const worker = ::CreateThread(nullptr, 0, worker_entry, nullptr, 0, nullptr);
    if (worker != nullptr) {
        ::CloseHandle(worker);
    }
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (options.feed && !g_feed_retired && g_probe.feed_stop != nullptr) {
        // 生产收尾：只有返回 1 才表示门禁全过。
        DWORD const stopped = g_probe.feed_stop(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(10000)));
        deskfeed::log_format(L"DeskFeedStop -> %lu (%s)", stopped,
                             stopped == 1 ? L"retired" : L"NOT retired");
        if (stopped != 1) {
            g_failed = true;
        }
    }
    if (g_probe.dangling != nullptr && g_probe.dangling() != 0) {
        deskfeed::log_line(L"a hook is still dangling");
        g_failed = true;
    }
    if (options.balance_check && g_probe.stop != nullptr) {
        // 第二次 stop 既不该多还也不该少还（引用数必须仍是 1）。
        deskfeed::log_line(L"balance check: calling the payload stop entry point again");
        g_probe.stop();
    }
    if (g_probe.stop != nullptr) {
        g_probe.stop();
    }

    report_module_references(g_module);

    for (ExtraCore* core : g_extra_cores) {
        if (core->thread != nullptr) {
            ::PostThreadMessageW(::GetThreadId(core->thread), WM_QUIT, 0, 0);
        }
    }
    return g_failed ? 1 : 0;
}

}  // namespace deskfeed::probe
