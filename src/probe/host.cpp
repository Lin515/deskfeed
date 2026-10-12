#include "probe/host.h"

#include "inject/status_pod.h"
#include "platform/log.h"
#include "platform/win32_util.h"
#include "probe/host_gate.h"
#include "probe/host_wait.h"
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

// 负载给自测宿主用的一组导出（签名与负载侧 common ABI 头逐字一致，见
// src/payload/probe_api.h 与 dllmain.cpp；这里只按名字解析，不链接负载）。
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
    // 探针 ABI（R1–R5，common 头 src/payload/probe_api.h）。
    void(__stdcall* set_install_delay)(unsigned) = nullptr;              // R1
    int(__stdcall* hook_status)(HookProbeStatus*, unsigned) = nullptr;   // R2
    int(__stdcall* set_observer)(int) = nullptr;                         // R3（开关）
    int(__stdcall* observer_stage)(HookProbeStage*, unsigned) = nullptr; // R3（拉一份快照）
    int(__stdcall* set_completion_mutation)(unsigned) = nullptr;         // R4
    int(__stdcall* set_tray_window)(HWND) = nullptr;                     // R5
    // 生产入口与状态 ABI。
    DWORD(WINAPI* feed_start)(void*) = nullptr;
    DWORD(WINAPI* feed_stop)(void*) = nullptr;
    DWORD(WINAPI* get_status)(void*) = nullptr;
};

HostOptions g_options;
ProbeExports g_probe;
ExportPresence g_presence;
HMODULE g_module = nullptr;
HWND g_window = nullptr;
HWND g_ack = nullptr;
DWORD g_ui_thread = 0;
HANDLE g_ui_done = nullptr;
bool g_feed_retired = false;
bool g_failed = false;
bool g_blocked = false;
bool g_dump_only = false;

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

// 按名字把负载的探针导出解析出来。**只填**，不做"缺了就跳过"的判断：需要什么由 feature gate
// （host_gate.h）决定，缺必需接口一律 BLOCKED。返回 false 只表示"这个模块连基线都不像负载"。
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
    g_probe.set_install_delay = reinterpret_cast<void(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeSetInstallDelay"));
    g_probe.hook_status = reinterpret_cast<int(__stdcall*)(HookProbeStatus*, unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeHookStatus"));
    g_probe.set_observer = reinterpret_cast<int(__stdcall*)(int)>(
        ::GetProcAddress(module, "DeskFeedProbeSetObserver"));
    g_probe.observer_stage = reinterpret_cast<int(__stdcall*)(HookProbeStage*, unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeObserverStage"));
    g_probe.set_completion_mutation = reinterpret_cast<int(__stdcall*)(unsigned)>(
        ::GetProcAddress(module, "DeskFeedProbeSetCompletionMutation"));
    g_probe.set_tray_window = reinterpret_cast<int(__stdcall*)(HWND)>(
        ::GetProcAddress(module, "DeskFeedProbeSetTrayWindow"));
    g_probe.feed_start = reinterpret_cast<DWORD(WINAPI*)(void*)>(
        ::GetProcAddress(module, "DeskFeedStart"));
    g_probe.feed_stop = reinterpret_cast<DWORD(WINAPI*)(void*)>(
        ::GetProcAddress(module, "DeskFeedStop"));
    g_probe.get_status = reinterpret_cast<DWORD(WINAPI*)(void*)>(
        ::GetProcAddress(module, "DeskFeedGetStatus"));

    g_presence.start = g_probe.start != nullptr;
    g_presence.sites = g_probe.sites != nullptr;
    g_presence.watch = g_probe.watch != nullptr;
    g_presence.run = g_probe.run != nullptr;
    g_presence.set_host = g_probe.set_host != nullptr;
    g_presence.stop = g_probe.stop != nullptr;
    g_presence.set_slow_apply = g_probe.set_slow_apply != nullptr;
    g_presence.post_apply = g_probe.post_apply != nullptr;
    g_presence.retire = g_probe.retire != nullptr;
    g_presence.hook = g_probe.hook != nullptr;
    g_presence.teardown = g_probe.teardown != nullptr;
    g_presence.dangling = g_probe.dangling != nullptr;
    g_presence.hook_status = g_probe.hook_status != nullptr;
    g_presence.set_observer = g_probe.set_observer != nullptr;
    g_presence.observer_stage = g_probe.observer_stage != nullptr;
    g_presence.set_install_delay = g_probe.set_install_delay != nullptr;
    g_presence.set_completion_mutation = g_probe.set_completion_mutation != nullptr;
    g_presence.set_tray_window = g_probe.set_tray_window != nullptr;
    g_presence.feed_start = g_probe.feed_start != nullptr;
    g_presence.feed_stop = g_probe.feed_stop != nullptr;
    g_presence.get_status = g_probe.get_status != nullptr;

    return g_probe.start != nullptr || g_probe.feed_start != nullptr;
}

// 在**本进程内**读一次 144 字节状态 POD，并逐项校验。返回 1 表示这份 POD 可信；返回 0 时
// `why` 说明原因。校验与取值共用同一份实现：绝不用"截断过的" scope 冒充合法 scope，也不把
// 尺寸/abi/width 对不上的一份 POD 当结论（fail closed）。
// `require_scope`：运行期必须报告本次运行的 scope；收尾之后负载可以合法地把 scope 清掉，
// 那时只校验 ABI 与字段域。
int read_status_pod(deskfeed::inject::PayloadStatusPod& pod, bool require_scope,
                    std::wstring& why) {
    using deskfeed::inject::kPayloadScopeCapacity;
    using deskfeed::inject::kPayloadStatusAbi;
    using deskfeed::inject::kPayloadStatusPodBytes;
    using deskfeed::inject::kPayloadStatusWidth;
    using deskfeed::inject::PayloadStatusPod;

    pod = PayloadStatusPod{};
    pod.size = static_cast<std::uint32_t>(sizeof(pod));
    pod.abi = kPayloadStatusAbi;
    pod.width = kPayloadStatusWidth;
    if (g_probe.get_status == nullptr) {
        why = L"the payload exports no DeskFeedGetStatus";
        return 0;
    }
    if (g_probe.get_status(&pod) != 1) {
        why = L"DeskFeedGetStatus returned 0";
        return 0;
    }
    if (pod.abi != kPayloadStatusAbi || pod.size != kPayloadStatusPodBytes) {
        why = L"the status POD reports a different size/abi";
        return 0;
    }
    if (pod.width != kPayloadStatusWidth) {
        why = L"the status POD reports a different ABI width";
        return 0;
    }
    // 规范白名单：scope 最多 32 个字符，且必须在 ABI 容量内以 NUL 结尾。
    std::size_t length = 0;
    while (length < kPayloadScopeCapacity && pod.scope[length] != L'\0') {
        ++length;
    }
    if (length >= kPayloadScopeCapacity) {
        why = L"the reported scope is not NUL terminated inside the ABI capacity";
        return 0;
    }
    if (length > 32) {
        why = L"the reported scope is longer than the 32-character whitelist";
        return 0;
    }
    if (require_scope && std::wstring(pod.scope, length) != g_options.scope) {
        why = L"the reported scope is not the scope of this run";
        return 0;
    }
    return 1;
}

// 第三方钩子 + install-race + 提前 done 的对抗序列。返回 true 表示观测符合预期。
//
// 这一条**不再**"缺接口就 return true"：缺任何一项必需导出时 feature gate 已经把它拦成 BLOCKED，
// 而这里再硬断言一次（防御性）：运行时少一个入口就是失败，绝不静默跳过。
bool run_ordering_case() {
    if (!g_options.third_party && g_options.install_delay_ms == 0) {
        return true;
    }
    if (g_probe.post_apply == nullptr || g_probe.retire == nullptr ||
        g_probe.set_observer == nullptr || g_probe.observer_stage == nullptr ||
        g_probe.hook_status == nullptr || g_probe.dangling == nullptr) {
        deskfeed::log_line(L"ordering: a required probe export disappeared after the gate; "
                           L"refusing to run the case");
        return false;
    }

    // R3 打开阶段记录：第三方钩子随后每次取样都是从负载拉一份最新快照。
    if (g_probe.set_observer(1) != 1) {
        deskfeed::log_line(L"ordering: the payload refused to observe hook stages (R3)");
        return false;
    }
    if (g_probe.set_install_delay != nullptr && g_options.install_delay_ms != 0) {
        g_probe.set_install_delay(g_options.install_delay_ms);
    }
    if (g_options.mutation_early_done) {
        if (g_probe.set_completion_mutation == nullptr ||
            g_probe.set_completion_mutation(
                static_cast<unsigned>(CompletionMutation::EarlyBeforeCallNext)) != 1) {
            deskfeed::log_line(L"ordering: the early-done failpoint (R4) could not be armed");
            g_probe.set_observer(0);
            return false;
        }
    }

    // **第三方钩子先装**，负载的钩子后装 → 负载的 CallNextHookEx 会到达第三方。
    std::wstring error;
    if (!third_party_install(g_ui_thread, g_probe.observer_stage, g_ack, error)) {
        deskfeed::log_format(L"ordering: cannot install the third-party hook: %s", error.c_str());
        if (g_probe.set_completion_mutation != nullptr) {
            g_probe.set_completion_mutation(static_cast<unsigned>(CompletionMutation::Normal));
        }
        g_probe.set_observer(0);
        return false;
    }

    int const posted = g_probe.post_apply(g_ui_thread, 5000);
    int const retired = g_probe.retire(5000);
    ThirdPartyObservation const third = third_party_observation();
    AckObservation const ack = ack_observation();
    std::wstring const stop_error;
    third_party_uninstall(error);
    if (g_probe.set_completion_mutation != nullptr) {
        // 收尾前强制复位：failpoint 绝不留到下一轮（生产启动也会复位，这里是双保险）。
        g_probe.set_completion_mutation(static_cast<unsigned>(CompletionMutation::Normal));
    }
    g_probe.set_observer(0);

    deskfeed::log_format(L"ordering: post=%d retire=%d entries=%u pumps=%u sentinels=%u "
                        L"ping_in_frame=%u ack_hits=%u ack_in_frame=%u",
                        posted, retired, third.entries, third.pumps, third.sentinels,
                        third.ping_in_frame, ack.hits, ack.hits_while_third_party_active);

    // R2：把钩子门的数字状态如实打出来（只判"读得到"与"没有悬挂"，其余供人核查）。
    HookProbeStatus status{};
    status.size = static_cast<std::uint32_t>(kProbeStatusSize);
    status.version = kProbeStatusVersion;
    int const status_ok = g_probe.hook_status(&status, static_cast<unsigned>(kProbeStatusSize));
    if (status_ok != 1) {
        deskfeed::log_line(L"ordering: the hook-gate status POD (R2) could not be read");
        return false;
    }
    deskfeed::log_format(L"ordering: status installed=%u retired=%u still_installed=%u in_flight=%u "
                        L"pending=%u completion_expected=%u dangling=%u done=%u",
                        status.hook_installed, status.retired, status.still_installed, status.in_flight,
                        status.pending, status.completion_expected, status.dangling,
                        status.done_signaled);
    if (status.dangling != 0) {
        deskfeed::log_line(L"ordering: the hook gate still reports a dangling record");
        return false;
    }

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
    if (g_blocked) {
        // BLOCKED：缺必需导出，一条也不跑。这里只负责把工作线程放行（它随后收窗口）。
        if (g_ui_done != nullptr) {
            ::SetEvent(g_ui_done);
        }
        return true;
    }
    if (g_options.slow_apply_ms != 0) {
        // gate 已保证这个导出存在；再断言一次，绝不静默忽略这个请求。
        if (g_probe.set_slow_apply == nullptr) {
            deskfeed::log_line(L"slow-apply: the payload export disappeared after the gate");
            if (g_ui_done != nullptr) {
                ::SetEvent(g_ui_done);
            }
            return false;
        }
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

    if (g_options.teardown && !g_options.feed) {
        if (g_probe.teardown == nullptr) {
            deskfeed::log_line(L"teardown: the payload export disappeared after the gate");
            g_failed = true;
        } else {
            int const ok = g_probe.teardown(5000);
            deskfeed::log_format(L"teardown: %s", ok == 1 ? L"retired" : L"NOT retired");
            if (ok != 1) {
                g_failed = true;
            }
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
    if (g_options.hang_ui_ms == 0) {
        return true;
    }
    if (g_probe.post_apply == nullptr || g_probe.retire == nullptr) {
        deskfeed::log_line(L"hang: a required probe export disappeared after the gate; "
                           L"refusing to run the case");
        return false;
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
        g_blocked = true;  // 一条也跑不了：UI 线程必须什么都不碰（导出全是空指针）
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);
        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        return 0;
    }
    g_module = module;

    if (!resolve_exports(module)) {
        deskfeed::log_line(L"the module does not export any known payload entry point");
        g_failed = true;
        g_blocked = true;
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);
        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        return 0;
    }

    // Feature gate：命令行开了什么，就必须有什么。缺一条就 BLOCKED（exit 2），一条也不跑——
    // 决不允许"日志里说一句就跳过"再以 0 退出，让验收脚本把没跑读成通过。
    GateDecision const gate = gate_decision(g_options, g_presence);
    if (gate.blocked) {
        for (GateMiss const& miss : gate.missing) {
            deskfeed::log_format(L"BLOCKED: %s needs export %s", miss.feature.c_str(), miss.symbol);
        }
        deskfeed::log_format(L"BLOCKED: %zu required probe interface(s) are missing; no case was run",
                             gate.missing.size());
        g_blocked = true;
        g_failed = true;
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);
        ::PostMessageW(g_window, WM_CLOSE, 0, 0);
        return 0;
    }

    if (g_options.hook) {
        // gate 已保证 hook 存在；缺了就不是 PASS 而是失败。
        if (g_probe.hook == nullptr) {
            deskfeed::log_line(L"reg hook: the payload export disappeared after the gate");
            g_failed = true;
        } else {
            int const installed = g_probe.hook(1);
            deskfeed::log_format(L"reg hook: %s", installed == 1 ? L"installed" : L"refused");
            if (installed != 1) {
                g_failed = true;
            }
            // 自检完立刻还原：模块随后要被 FreeLibrary，钩子还留着就是在卸载自己的代码。
            int const restored = g_probe.hook(0);
            deskfeed::log_format(L"reg hook: restore -> %s", restored == 1 ? L"ok" : L"FAILED");
            if (restored != 1) {
                g_failed = true;
            }
        }
    }
    if (g_options.slow_apply_ms != 0) {
        // 慢执行体是给"挂着的时候 stop 必须未退休"用的，越早设越好（UI 线程里还会再设一次）。
        if (g_probe.set_slow_apply == nullptr) {
            deskfeed::log_line(L"slow-apply: the payload export disappeared after the gate");
            g_failed = true;
        } else {
            g_probe.set_slow_apply(g_options.slow_apply_ms);
        }
    }

    if (g_options.feed) {
        // 生产路径：负载自己 attach mock 邮箱、自己把帧渲染进宿主的岛。**不做** inline 应用：
        // XAML 元素有线程亲和性，内容必须由负载经真实 hook 编组到 UI 线程。
        // R5：把宿主的岛登记成托盘目标。返回 0 = 负载拒绝了这个窗口，那是失败而不是跳过。
        int const tray_ok = g_probe.set_tray_window(g_window);
        DWORD owner_pid = 0;
        DWORD const owner_thread = ::GetWindowThreadProcessId(g_window, &owner_pid);
        if (tray_ok != 1) {
            deskfeed::log_line(L"the payload refused the probe tray window (R5)");
            g_failed = true;
        }
        if (owner_pid != ::GetCurrentProcessId() || owner_thread != g_ui_thread) {
            deskfeed::log_format(L"the tray window is not ours: pid=%lu thread=%lu (expected pid=%lu "
                                 L"thread=%lu)",
                                 owner_pid, owner_thread, ::GetCurrentProcessId(), g_ui_thread);
            g_failed = true;
        }
        std::wstring scope = g_options.scope;
        DWORD const started = g_probe.feed_start(&scope);
        deskfeed::log_format(L"DeskFeedStart -> %lu (%s)", started,
                             started == 1 ? L"started" : L"not started");
        if (started != 1 && started != 3) {
            g_failed = true;
        }
        // 144 字节状态 POD：started 必须为真、线程必须活着（gate 已保证导出存在）。
        deskfeed::inject::PayloadStatusPod pod{};
        std::wstring status_why;
        if (read_status_pod(pod, /*require_scope=*/true, status_why) != 1) {
            deskfeed::log_format(L"the status POD (144B) is not trustworthy after DeskFeedStart: %s",
                                 status_why.c_str());
            g_failed = true;
        } else {
            deskfeed::log_format(L"status: started=%u thread_alive=%u unload_safe=%u dangling=%u "
                                L"mailbox=%u producer_alive=%u block=%u scope=<%s>",
                                pod.started, pod.thread_alive, pod.unload_safe, pod.dangling,
                                pod.mailbox_attached, pod.producer_alive, pod.xaml_block_present,
                                pod.scope);
            if (pod.started == 0) {
                deskfeed::log_line(L"the status POD does not report a started payload");
                g_failed = true;
            }
        }
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);

        // 收尾的对抗：挂住 UI / 让 apply 变慢时，**短超时**的 stop 必须报"未退休"（0），
        // 恢复之后重试必须报"可以卸载"（1）。绝不在这两种情况下强卸模块。
        if (g_options.slow_apply_ms != 0 || g_options.hang_ui_ms != 0) {
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
        // 少一条 site 就意味着这次运行证明不了 >1 核心时的引用归还：那是失败，不是"少跑了一条"。
        deskfeed::log_format(L"only %d of %u core(s) owned by this process attached a site", sites,
                             g_options.cores);
        g_failed = true;
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
// 返回值就是账上的引用数：调用方必须据此判红，而不是只把这一行打给人看。
int report_module_references(HMODULE module) {
    if (module == nullptr) {
        deskfeed::log_line(L"the payload was never loaded");
        return -1;
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
    return references;
}

// 只归还**本进程自己那一次** `LoadLibrary` 的引用（BLOCKED 路径专用）：BLOCKED 时除了装载与解析
// 导出什么都没跑，所以只该还这一份。**绝不**循环 `FreeLibrary` 去清"未知的引用"——那会把别的持有者
// 的引用一起抽走。返回后模块是否还在表里如实打印。
void release_single_reference(HMODULE module) {
    if (module == nullptr) {
        deskfeed::log_line(L"this process never loaded the payload; nothing to return");
        return;
    }
    // 取路径要在卸载之前：最后一次成功的 FreeLibrary 已经把模块的代码与数据一起拿走。
    std::wstring const path = deskfeed::module_file_path(module);
    std::wstring const name = path.substr(path.find_last_of(L'\\') + 1);

    bool const returned = ::FreeLibrary(module) != FALSE;
    deskfeed::log_format(L"BLOCKED: this process's single load reference -> %s",
                         returned ? L"returned" : L"could not be returned");
    deskfeed::log_format(L"the payload module (%s) is %s now", name.c_str(),
                         ::GetModuleHandleW(name.c_str()) == nullptr ? L"fully unloaded"
                                                                    : L"still mapped");
}

// 有界地等到 `thread` **真实 signal** 的等待器在 `probe/host_wait.h`（探针收尾的公共形状，见那里的
// 说明：必须边等边泵消息，而且"post 过 WM_CLOSE"不是证据）。

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
    if (worker == nullptr) {
        // 建不出工作线程就**立刻失败退出**：绝不留在 GetMessage 里等一条永远不会工作的线程——
        // 那会把"起不来"变成"永久挂住"，而且什么都没证明。
        deskfeed::log_format(L"cannot create the host worker thread: %s",
                             deskfeed::last_error_text().c_str());
        g_failed = true;
        return kHostExitFailed;
    }
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // 消息循环结束（工作线程 post 了 WM_CLOSE、窗口销毁并 PostQuitMessage）**不等于**工作线程已经
    // 退出。"已经请它退"、"已经 post 过 WM_CLOSE"都不是证据：归还模块引用之前必须等到**真实**
    // signal，等待期间继续泵消息。没等到就什么都不碰。
    bool const worker_exited = wait_thread_exit_pumping(worker, 30000);
    ::CloseHandle(worker);
    if (!worker_exited) {
        deskfeed::log_line(L"the host worker thread has not really exited");
        g_failed = true;
    }

    // 额外的 XAML 核心也必须**真实退出**：它们各自的 WindowsXamlManager / DesktopWindowXamlSource
    // 是那些线程上的局部对象，线程还在跑就让它们析构（或让进程收尾去拆）就是在别人还在用的对象上做
    // 拆除。先请它们退（WM_QUIT），再逐个等到真实 signal；绝不 ShutdownQueueAsync（冻结禁用）。
    for (ExtraCore* core : g_extra_cores) {
        if (core->thread != nullptr) {
            ::PostThreadMessageW(::GetThreadId(core->thread), WM_QUIT, 0, 0);
        }
    }
    for (ExtraCore* core : g_extra_cores) {
        if (core->thread != nullptr && !wait_thread_exit_pumping(core->thread, 10000)) {
            deskfeed::log_line(L"an extra XAML core thread has not really exited; refusing to "
                               L"destroy XAML objects under a live thread");
            g_failed = true;
        }
    }

    if (g_blocked) {
        // BLOCKED：除了"装载 + 解析导出 + 本进程那一次 LoadLibrary"，一条用例都没跑。所以只归还
        // **自己这一份**引用；绝不循环 FreeLibrary 去清"未知的引用"——那会把别的持有者（含第三方
        // 钩子）的引用一起抽走，正是在不安全的时候强行卸载。
        release_single_reference(g_module);
    } else {
        UnloadGate gate;
        gate.worker_exited = worker_exited;
        gate.feed_mode = options.feed;

        if (options.feed && !g_feed_retired) {
            // 生产收尾：只有返回 1 才表示门禁全过。
            DWORD const stopped = g_probe.feed_stop(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(10000)));
            deskfeed::log_format(L"DeskFeedStop -> %lu (%s)", stopped,
                                 stopped == 1 ? L"retired" : L"NOT retired");
            gate.stop_ran = stopped == 1;
            if (stopped != 1) {
                g_failed = true;
            }
        } else if (options.feed) {
            // 早先那一轮（挂住 UI / 慢执行）已经拿到过"可以卸载"，那正是 g_feed_retired 的含义。
            gate.stop_ran = true;
        }
        if (options.feed) {
            // 状态 POD 复核：线程必须已经不在跑、unload_safe 必须为真、不得有悬挂。
            deskfeed::inject::PayloadStatusPod pod{};
            std::wstring status_why;
            if (read_status_pod(pod, /*require_scope=*/false, status_why) != 1) {
                deskfeed::log_format(L"the status POD (144B) is not trustworthy after DeskFeedStop: %s",
                                     status_why.c_str());
                g_failed = true;
            } else {
                deskfeed::log_format(L"status after stop: started=%u thread_alive=%u unload_safe=%u "
                                    L"dangling=%u block=%u",
                                    pod.started, pod.thread_alive, pod.unload_safe, pod.dangling,
                                    pod.xaml_block_present);
                gate.status_clear = pod.thread_alive == 0 && pod.unload_safe == 1 && pod.dangling == 0;
                if (!gate.status_clear) {
                    deskfeed::log_line(L"the payload is not safely retired according to its own "
                                       L"status POD");
                    g_failed = true;
                }
            }
        }

        // 悬挂判据：只要载荷提供了它就**必须**报 0；该模式要求它却缺了同样是失败。它不适用时
        // （`--mock` 基线）不算通过，闭环由最后的引用计数兜。
        bool const dangling_required = options.teardown || options.third_party || options.hook ||
                                       options.hang_ui_ms != 0 || options.feed;
        if (g_probe.dangling != nullptr) {
            if (g_probe.dangling() != 0) {
                deskfeed::log_line(L"a hook is still dangling");
                g_failed = true;
            } else {
                gate.dangling_clear = true;
            }
        } else if (dangling_required) {
            deskfeed::log_line(L"the dangling judge export disappeared after the gate");
            g_failed = true;
        } else {
            gate.dangling_clear = true;
        }

        if (options.balance_check) {
            // 第二次 stop 既不该多还也不该少还（引用数必须仍是 1）。
            deskfeed::log_line(L"balance check: calling the payload stop entry point again");
            g_probe.stop();
        }
        if (g_probe.stop != nullptr) {
            g_probe.stop();
            if (!options.feed) {
                // 结构自测：探针停止入口返回 void，它本身**不是**安全证明。它能证明的只有"真的被
                // 调用过"；闭环由上面的悬挂判据与下面的引用计数合起来钉住。
                gate.stop_ran = true;
            }
        } else if (!options.feed) {
            // 结构自测的 gate 要求 DeskFeedProbeStop 存在；真走到这里说明它在运行期丢了。
            deskfeed::log_line(L"the payload stop entry point disappeared after the gate");
            g_failed = true;
        }

        // 卸载前的**唯一**判据（`probe/host_gate.h` 的纯函数）：线程真退出、停止入口真跑过、悬挂判据
        // 为 0，feed 还要状态 POD 报 thread_alive=0 / unload_safe=1 / dangling=0。任何一项不成立都
        // **不**归还引用、**不** FreeLibrary，模块保持映射到本进程结束——而不是"报个 warning 然后
        // 照样强卸"。第三方提前 done 之类的变异必须能观察到不安全症状，但**不能**借它强行卸载。
        if (!unload_allowed(gate)) {
            deskfeed::log_line(L"retained/unverifiable: the retirement gate did not pass; the module "
                               L"is left mapped until this process ends (nothing is force-unloaded)");
            g_failed = true;
        } else {
            // 引用计数不是"打给人看的一行"：健康路径必须刚好是 1（探针自己那一次 LoadLibrary）。
            // 0/负值说明多还了一次（模块可能在它自己的代码还在跑时被抽走），>1 说明少还了。这一步
            // 会让 loader 逐次 FreeLibrary，所以它**只**允许在门禁全部通过之后跑。
            int const references = report_module_references(g_module);
            if (references != 1) {
                deskfeed::log_format(L"the payload module reference count is %d, not 1: the site "
                                     L"references were not returned exactly once",
                                     references);
                g_failed = true;
            }
        }
    }
    if (g_blocked) {
        return kHostExitBlocked;
    }
    return g_failed ? kHostExitFailed : kHostExitOk;
}

}  // namespace deskfeed::probe
