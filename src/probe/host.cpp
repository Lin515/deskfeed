#include "probe/host.h"

#include "display/mailbox.h"
#include "inject/status_pod.h"
#include "platform/log.h"
#include "platform/win32_util.h"
#include "probe/host_gate.h"
#include "probe/host_wait.h"
#include "probe/mock_producer.h"
#include "probe/observer_pod.h"
#include "probe/probe_hooks.h"
#include "probe/scope_claim.h"

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
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <vector>

namespace deskfeed::probe {
namespace {

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxd = winrt::Windows::UI::Xaml::Documents;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

constexpr wchar_t kWindowClass[] = L"DeskFeedProbeHost";
constexpr UINT kRunMessage = WM_APP + 1;
// UI 线程"停止取消息"：在窗口过程里睡，就一定不会检索消息 —— 这正是对抗"任务栏线程被挂住"的
// 形状（钩子记录留在链上，收尾拿不到完成证明）。判断必须在 PeekMessage **之前**生效。
constexpr UINT kHangMessage = WM_APP + 2;
// `--feed` 的帧序列：工作线程发一帧、投一次这个请求，**UI 线程读宿主自己那棵树**判定这一帧
// 真的变成了什么（文字/颜色/块在不在），并把 PASS/FAIL 写回给工作线程。判定绝不用状态 POD 的
// 标记位代替——那是"负载说它做了什么"，不是"树上真的长出了什么"。
constexpr UINT kCheckMessage = WM_APP + 3;

// 定义在"`--feed` 的帧序列"那一节；窗口过程在它之前，所以先声明。
void run_feed_check(unsigned index, unsigned deadline_ms);
// 辅助生产者子进程的启动/结束（探针绝不自己冒充死掉的邮箱属主）；定义同样在帧序列那一节。
bool spawn_producer_child(std::wstring const& scope, std::wstring& error);
void stop_producer_child();

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

// 帧序列驱动与"宿主自读自己树"的判定状态（定义见下面的帧序列那一节）。
wuxc::Grid g_tray_grid{nullptr};
MockProducer g_producer;
HANDLE g_check_done = nullptr;
unsigned g_check_index = 0;
bool g_check_pass = false;
bool g_check_ran = false;
std::wstring g_check_detail;
winrt::Windows::UI::Color g_normal_value_color{};
bool g_normal_color_known = false;
// 三档颜色的**观测**基线（ok 用上面那个正常档）。它们全部是从宿主自己那棵树上读回来的，绝不抄
// 负载的调色板：抄一份就等于"两边对同一件事有两套解释"。
winrt::Windows::UI::Color g_warn_state_color{};
winrt::Windows::UI::Color g_error_state_color{};
bool g_state_colors_known = false;

// `ownerdeath` 的辅助生产者子进程：邮箱由**另一条进程**持有（探针自己不建 producer，否则就是
// "自己发 closed 帧冒充 owner 死亡"）。句柄由工作线程登记，收尾时再兜底杀一次。
HANDLE g_producer_child = nullptr;
bool g_owner_child_ready = false;
// `ownerdeath` 期间由**父进程**持有的实例仲裁（与主进程同一个名字，见 probe/scope_claim.h）。
// 子进程用 `--parent-claim` 复用这份持有期，不再自己去抢同一个名字而自拒。
ProbeScopeClaim g_feed_claim;
// 生产收尾的结论：由**工作线程**发起 DeskFeedStop 并记在这里，UI 线程只读取（见 worker 里的说明）。
bool g_feed_stop_ran = false;
DWORD g_feed_stop_result = 0;

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
    g_tray_grid = tray;  // 宿主唯一合法可读的树：它属于本线程
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
    if (g_options.mutation_early_done) {
        // 对抗：done 被提前，收尾会在负载的 CallNext 帧里被处理 —— 必须**观察到**这个症状，
        // 否则说明量具根本没咬住。
        //
        // 这一段必须在下面的 dangling 判据**之前**：变异路径下负载刻意按"未退休"处理并置
        // `dangling`（提前置位的完成事件证明不了帧已弹掉），那是这条对抗的**预期结果**，不是失败。
        // 先看 dangling 就会把每一条变异运行都判成"门还报悬挂"，咬没咬到永远无从得知。
        bool const bit = ack.hits_while_third_party_active > 0 || third.ping_in_frame > 0;
        if (!bit) {
            deskfeed::log_line(L"ordering: the early-done mutation was NOT observable; the "
                               L"instrument does not bite");
            return false;
        }
        deskfeed::log_line(L"ordering: the early-done mutation is observable (hard failure seen)");
        return true;
    }

    if (status.dangling != 0) {
        deskfeed::log_line(L"ordering: the hook gate still reports a dangling record");
        return false;
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
            // 与这一段的其它失败早退一样**放行工作线程**：run 已经返回，UI 线程不会再有别的进展。
            // 漏掉这一句会让工作线程白等满 30 秒才继续——"没等到信号"于是被伪装成"等了很久才成功"。
            if (g_ui_done != nullptr) {
                ::SetEvent(g_ui_done);
            }
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
            // 这里**不销毁**宿主窗口：它（连同作为它子窗口的 ack 窗口）必须活到
            // `--third-party-hook-pump` / `--hang-ui-ms` / `--feed` 的收尾之后——负载的
            // `hook_post_retire` 要向"属于同一个 T 与同一个进程的窗口"发那次证明帧已弹掉的 ping。
            // 早先在这里 `DestroyWindow` 会把 ack 窗口一起带走，`resolve_ack_window` 于是回退到
            // 真实的 `Shell_TrayWnd`，被 PID 判据拒绝 ⇒ `retire=0 / ack_hits=0`，而且 worker 末尾
            // 那次 `WM_CLOSE` 变成对已销毁窗口的空投 ⇒ UI 线程永远等不到退出消息。
            // 结束消息循环是**工作线程**的事：它的每一条返回路径都发 `WM_CLOSE`（BLOCKED、装载
            // 失败、feed 的每条收尾路径与结构自测的末尾），收到它才 `DestroyWindow` + 退出。
            return 0;
        case kHangMessage:
            // 停在窗口过程里 = 这条线程不检索消息。负载的钩子记录留在链上，收尾会超时（0）。
            ::Sleep(g_options.hang_ui_ms);
            return 0;
        case kCheckMessage:
            // UI 线程读**自己那棵树**：有界采样直到 PASS 或 deadline，结论写回给工作线程。
            run_feed_check(static_cast<unsigned>(wparam), 5000);
            if (g_check_done != nullptr) {
                ::SetEvent(g_check_done);
            }
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

// ---- `--feed` 的帧序列：宿主自读自己那棵树 ---------------------------------
//
// 为什么必须是"读自己那棵树"而不是读状态 POD：POD 只说负载**声称**做了什么。这里要的是
// "帧 → 块"这条生产路径真的走通了：发一帧合成帧进隔离 scope 的邮箱，负载经真实钩子编组把
// 内容插进宿主自己的岛，然后由**拥有这棵树的那条 UI 线程**读出 Run 的文字与前景色，逐项判定。
//
// 颜色判定刻意用**相对关系**（正常/warn/error 三档互不相同、alarm 时全部同一档且不同于正常），
// 不把负载的调色板抄一份到探针里：抄一份就等于"两边对同一件事有两套解释"。
struct FeedStep {
    wchar_t const* name = L"";
    // 这一步判定**之前**要按顺序发布的帧。一步可以发多帧：`latest` 的背压突发就是一次发一批，
    // 然后判定树上只能是最后那一帧。帧序列由工作线程发，判定由 UI 线程读自己那棵树做。
    std::vector<Frame> frames;
    bool closed = false;         // 发完 frames 后再 publish_closed（生产者正常收尾）
    bool owner_death = false;    // 这一批帧由**辅助生产者进程**持有邮箱（探针自己不建 producer）
    bool kill_owner = false;     // 判定前真的结束辅助生产者进程（不是发一个 closed 帧冒充）
    bool expect_block = true;
    wchar_t const* expect_contains = L"";
    wchar_t const* expect_absent = L"";
    std::vector<std::wstring> absent_values;  // 这些子串全部不得出现（排队/复活/残留检查）
    // 0 = 不查色；1 = 该子串必须用**正常**档（并与首次记录的正常色一致）；
    // 2 = 该子串必须**不是**正常档；3 = 这些 value 全部同色且**不是**正常档（整体失效）；
    // 4 = values 恰好三个（ok/warn/error 顺序）两两不同、ok 等于已记录的正常档，并**记录**三档
    //     观测色；5 = values 恰好三个，每一个都必须回到它自己 state 记录过的观测色（warn 不许被
    //     折成正常档）。全部是观测来的相对关系，探针不抄负载调色板。
    int tone = 0;
    wchar_t const* tone_subject = L"";
    std::vector<std::wstring> values;
    wchar_t const* order_first = L"";
    wchar_t const* order_second = L"";
};

Frame content_frame(std::vector<FrameItem> items) {
    return make_content_frame(std::move(items));
}

// `--feed` 默认步（空脚本）与 `ownerdeath` 用的通用条目/值：只有协议字段，没有任何业务语义。
constexpr wchar_t kDefaultValue[] = L"54C";
constexpr wchar_t kDefaultDetail[] = L"cpu sample detail";
constexpr wchar_t kOwnerValue[] = L"AUX-ALIVE";
constexpr char kOwnerValueUtf8[] = "AUX-ALIVE";
constexpr int kBurstFrames = 8;

Frame cpu_frame(std::string value, std::string detail) {
    return content_frame({make_item("cpu", "CPU", std::move(value), ItemState::Ok,
                                    std::move(detail))});
}

// ok/warn/error 三档同时出现在**同一批真实帧**里：这是"三档互不相同"唯一能一次证明的组合。
Frame trio_frame(bool alarm, std::string reason) {
    return make_content_frame({make_item("cpu", "CPU", "54C", ItemState::Ok, "cpu detail"),
                               make_item("disk", "DISK", "7C", ItemState::Warn),
                               make_item("net", "NET", "ERR", ItemState::Error)},
                              alarm, FailureKind::Timeout, std::move(reason));
}

void build_feed_steps(std::wstring const& script, std::vector<FeedStep>& out) {
    out.clear();
    auto step = [&out](wchar_t const* name) -> FeedStep& {
        out.push_back(FeedStep{});
        out.back().name = name;
        return out.back();
    };
    if (script.empty()) {
        // **默认步**：`--feed` 不带脚本时也必须证明"帧 → 块"。曾经的实现只在有脚本时才建
        // producer，于是默认 `--feed` 连邮箱都没有，整条生产路径根本没被走到。
        FeedStep& only = step(L"default/generic-frame");
        only.frames.push_back(cpu_frame("54C", "cpu sample detail"));
        only.expect_contains = kDefaultValue;
        only.tone = 1;
        only.tone_subject = kDefaultValue;
        return;
    }

    if (script == L"latest") {
        FeedStep& first = step(L"latest/baseline");
        first.frames.push_back(cpu_frame("54C", "cpu sample detail"));
        first.expect_contains = L"54C";
        first.tone = 1;
        first.tone_subject = L"54C";
        // **真实有界突发**：一次发 8 帧再判定。邮箱只保留最新一帧 ⇒ 树上只能是 burst-8；若通道
        // 排队积压，前面任何一帧都会被抓到。这就是"不排队"的机械证据，不是把期望调绿的措辞。
        FeedStep& burst = step(L"latest/bounded-burst");
        for (int index = 1; index <= kBurstFrames; ++index) {
            burst.frames.push_back(cpu_frame("burst-" + std::to_string(index),
                                             "burst detail " + std::to_string(index)));
        }
        burst.expect_contains = L"burst-8";
        for (int index = 1; index < kBurstFrames; ++index) {
            burst.absent_values.push_back(L"burst-" + std::to_wstring(index));
        }
        burst.absent_values.push_back(L"54C");  // 旧值被整份替换，不能残留

        // 数组顺序 = 显示顺序（重排必须真的反映到树上）。
        FeedStep& reorder = step(L"latest/reorder");
        reorder.frames.push_back(
            content_frame({make_item("mem", "MEM", "61%"), make_item("cpu", "CPU", "54C")}));
        reorder.expect_contains = L"61%";
        reorder.order_first = L"61%";
        reorder.order_second = L"54C";
        reorder.absent_values.push_back(L"burst-8");

        // 未出现的旧条目立即消失（完整替换，不是增量补丁）。
        FeedStep& omit = step(L"latest/omit");
        omit.frames.push_back(content_frame({make_item("mem", "MEM", "61%")}));
        omit.expect_contains = L"61%";
        omit.absent_values.push_back(L"54C");
        omit.absent_values.push_back(L"cpu sample detail");

        // 合法空列表 = 清空内容（不是取数失败，也不显示 ERROR）。
        FeedStep& cleared = step(L"latest/clear-empty");
        cleared.frames.push_back(content_frame({}));
        cleared.expect_block = false;

        // 清空之后再失败：只能显示 ERROR，**绝不**找回清空前的历史值（连 detail 也不行）。
        FeedStep& error_after = step(L"latest/error-after-clear");
        error_after.frames.push_back(make_error_frame(FailureKind::Timeout, "connect failed"));
        error_after.expect_contains = L"ERROR";
        error_after.absent_values = {L"54C", L"61%", L"burst-8", L"cpu sample detail"};
    } else if (script == L"warn") {
        FeedStep& first = step(L"warn/baseline");
        first.frames.push_back(cpu_frame("54C", {}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& warn = step(L"warn/distinct-tone");
        warn.frames.push_back(content_frame(
            {make_item("cpu", "CPU", "54C"), make_item("disk", "DISK", "7C", ItemState::Warn)}));
        warn.expect_contains = L"7C";
        warn.tone = 2;
        warn.tone_subject = L"7C";
    } else if (script == L"failure") {
        FeedStep& first = step(L"failure/baseline");
        first.frames.push_back(cpu_frame("54C", {}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& failed = step(L"failure/error-row");
        failed.frames.push_back(make_error_frame(FailureKind::Timeout, "connect failed"));
        failed.expect_contains = L"ERROR";
        failed.tone = 2;
        failed.tone_subject = L"ERROR";
    } else if (script == L"alarm") {
        FeedStep& first = step(L"alarm/baseline");
        first.frames.push_back(
            content_frame({make_item("cpu", "CPU", "54C"), make_item("mem", "MEM", "61%")}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& alarm = step(L"alarm/all-red");
        alarm.frames.push_back(make_content_frame(
            {make_item("cpu", "CPU", "54C"), make_item("mem", "MEM", "61%")}, /*alarm=*/true,
            FailureKind::Timeout, "connect failed"));
        alarm.expect_contains = L"54C";  // 整体失效时值必须**保留**，不能被折成"没有值"
        alarm.tone = 3;
        alarm.values = {L"54C", L"61%"};
    } else if (script == L"restore") {
        // 同一批真实帧上的三档颜色：ok/warn/error 必须两两不同（tone 4 记录三档观测色），整体失效
        // 时三条**全部**同色且不等于正常档（tone 3），恢复后每条必须回到**它自己** state 的观测色
        // （tone 5：warn 不许被折成正常档，error 也不许）。旧实现只证明"各自 != 正常档"，
        // 证明不了 warn 与 error 是两档。
        FeedStep& record = step(L"restore/three-states");
        record.frames.push_back(trio_frame(false, {}));
        record.expect_contains = L"54C";
        record.tone = 4;
        record.values = {L"54C", L"7C", L"ERR"};
        FeedStep& alarm = step(L"restore/alarm-all-red");
        alarm.frames.push_back(trio_frame(true, "connect failed"));
        alarm.expect_contains = L"54C";  // 整体失效时值必须保留
        alarm.tone = 3;
        alarm.values = {L"54C", L"7C", L"ERR"};
        FeedStep& back = step(L"restore/producer-states-back");
        back.frames.push_back(trio_frame(false, {}));
        back.expect_contains = L"54C";
        back.tone = 5;
        back.values = {L"54C", L"7C", L"ERR"};
    } else if (script == L"closed") {
        FeedStep& first = step(L"closed/baseline");
        first.frames.push_back(cpu_frame("54C", {}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& closed = step(L"closed/removes-block");
        closed.closed = true;
        closed.expect_block = false;
    } else if (script == L"empty") {
        FeedStep& first = step(L"empty/baseline");
        first.frames.push_back(cpu_frame("54C", {}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& waiting = step(L"empty/waiting-removes-block");
        waiting.frames.push_back(make_waiting_frame());  // 0 行：不显示读数，也不显示 ERROR
        waiting.expect_block = false;
        waiting.expect_absent = L"ERROR";
    } else if (script == L"delete") {
        FeedStep& first = step(L"delete/baseline");
        first.frames.push_back(cpu_frame("54C", {}));
        first.tone = 1;
        first.tone_subject = L"54C";
        FeedStep& cleared = step(L"delete/clear-removes-block");
        cleared.frames.push_back(content_frame({}));  // 空列表：清空，不是 ERROR
        cleared.expect_block = false;
        cleared.expect_absent = L"ERROR";
    } else if (script == L"ownerdeath") {
        // 生产者进程死亡与"正常 closed"是**两条**不同的清块路径：这一条要求邮箱的属主是**另一个
        // 进程**，宿主自己活着。帧必须由辅助生产者进程发（探针自己不建 producer，否则就是自己发
        // closed 帧或让宿主自己去死来冒充）。辅助进程不存在时这一条记 NOT-RUN，绝不当通过。
        FeedStep& foreign = step(L"ownerdeath/foreign-frame");
        foreign.owner_death = true;
        foreign.expect_contains = kOwnerValue;
        FeedStep& gone = step(L"ownerdeath/producer-exit-clears");
        gone.kill_owner = true;  // 判定前真的结束那条进程；由它的**真实句柄已触发**让负载清块
        gone.expect_block = false;
    }
}

// ---- 辅助生产者子进程（`ownerdeath` 专用）---------------------------------
//
// 邮箱的属主必须是**另一条进程**：探针自己建 producer 就是"自己发 closed 帧冒充 owner 死亡"，
// 而"宿主自己死"更是什么都证明不了（树跟着一起没了）。所以这里 spawn 一个真实进程持有邮箱、
// 发布一帧、驻留到被父进程结束；结束用的是本进程自己 `CreateProcessW` 出来的那个句柄，
// **绝不**按镜像名杀进程、也绝不碰任何不属于本次运行的进程。
std::wstring producer_child_path() {
    return deskfeed::join_path(directory_of(deskfeed::module_self_path()),
                               L"deskfeed-probe-producer-child.exe");
}

bool spawn_producer_child(std::wstring const& scope, std::wstring& error) {
    stop_producer_child();  // 幂等：先清掉上一轮留下的
    std::wstring const path = producer_child_path();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"the auxiliary producer fixture is not next to this tool";
        return false;
    }
    // `--parent-claim`：实例仲裁由父进程持有（贯穿整段 ownerdeath 运行），子进程不再自己去抢
    // 同一个名字——那会撞上父进程而自拒。两条路径共用 probe/scope_claim.h 的同一份实现。
    std::wstring command =
        L"\"" + path + L"\" --scope " + scope + L" --value " + kOwnerValue + L" --parent-claim";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process) == FALSE) {
        error = L"cannot start the auxiliary producer: " + deskfeed::last_error_text();
        return false;
    }
    ::CloseHandle(process.hThread);
    g_producer_child = process.hProcess;
    return true;
}

void stop_producer_child() {
    g_owner_child_ready = false;
    if (g_producer_child == nullptr) {
        return;
    }
    ::TerminateProcess(g_producer_child, 0);
    ::WaitForSingleObject(g_producer_child, 5000);
    ::CloseHandle(g_producer_child);
    g_producer_child = nullptr;
}

// R2 只读诊断：把钩子门的当前状态记进日志。它**只读**负载的固定 POD（不新增 ABI、不改负载状态），
// 用途是在收尾失败时把"到底哪一条证明没拿到"变成可归因的事实，而不是靠猜。
void log_hook_status(wchar_t const* when) {
    if (g_probe.hook_status == nullptr) {
        return;
    }
    deskfeed::payload::HookProbeStatus status{};
    status.size = deskfeed::payload::kProbeStatusSize;
    status.version = deskfeed::payload::kProbeStatusVersion;
    if (g_probe.hook_status(&status, static_cast<unsigned>(sizeof(status))) != 1) {
        deskfeed::log_format(L"hook status (%s): the payload refused the query", when);
        return;
    }
    deskfeed::log_format(L"hook status (%s): installed=%u retired=%u still_installed=%u in_flight=%u "
                         L"pending=%u expected=%u dangling=%u done=%u",
                         when, status.hook_installed, status.retired, status.still_installed,
                         status.in_flight, status.pending, status.completion_expected,
                         status.dangling, status.done_signaled);
    if (g_probe.observer_stage == nullptr) {
        return;
    }
    deskfeed::payload::HookProbeStage stage{};
    stage.size = deskfeed::payload::kProbeStageSize;
    stage.version = deskfeed::payload::kProbeStageVersion;
    if (g_probe.observer_stage(&stage, static_cast<unsigned>(sizeof(stage))) != 1) {
        return;
    }
    deskfeed::log_format(L"observer (%s): stage=%u sequence=%u complete=%u", when, stage.stage,
                         stage.sequence, stage.complete);
}

// UI 线程：读宿主自己的树。返回 false 表示这棵树根本读不到（宿主自己的对象，那是内部错误）。
bool read_own_tree(bool& present, std::wstring& text,
                   std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>>& runs) {
    present = false;
    text.clear();
    runs.clear();
    if (g_tray_grid == nullptr) {
        return false;
    }
    auto children = g_tray_grid.Children();
    for (std::uint32_t index = 0; index < children.Size(); ++index) {
        auto element = children.GetAt(index).try_as<wux::FrameworkElement>();
        if (element == nullptr || element.Name() != L"DeskFeedBlock") {
            continue;
        }
        present = true;
        auto border = element.try_as<wuxc::Border>();
        if (border == nullptr) {
            continue;
        }
        auto label = border.Child().try_as<wuxc::TextBlock>();
        if (label == nullptr) {
            continue;
        }
        auto inlines = label.Inlines();
        for (std::uint32_t at = 0; at < inlines.Size(); ++at) {
            auto run = inlines.GetAt(at).try_as<wuxd::Run>();
            if (run == nullptr) {
                continue;
            }
            std::wstring const piece = run.Text().c_str();
            winrt::Windows::UI::Color color{};
            if (auto brush = run.Foreground().try_as<wuxm::SolidColorBrush>()) {
                color = brush.Color();
            }
            text += piece;
            runs.emplace_back(piece, color);
        }
    }
    return true;
}

bool same_color(winrt::Windows::UI::Color left, winrt::Windows::UI::Color right) {
    return left.A == right.A && left.R == right.R && left.G == right.G && left.B == right.B;
}

// 找第一个文本包含 `needle` 的 Run 的颜色。
bool color_of(std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>> const& runs,
              std::wstring const& needle, winrt::Windows::UI::Color& out) {
    for (auto const& entry : runs) {
        if (!needle.empty() && entry.first.find(needle) != std::wstring::npos) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

// 一次判定：全部条件都成立才 PASS，`why` 写第一个不成立的原因。
bool evaluate_step(FeedStep const& step, std::wstring& why) {
    bool present = false;
    std::wstring text;
    std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>> runs;
    if (!read_own_tree(present, text, runs)) {
        why = L"the host's own XAML tree is not reachable";
        return false;
    }
    if (present != step.expect_block) {
        why = present ? L"a block is still in the host's tree" : L"no block was inserted into the host's tree";
        return false;
    }
    if (!step.expect_block) {
        // 树上确实没有块 ⇒ 读回来的文字就是空的，`expect_absent` 在这里天然空集（不是被忽略）。
        // "清空后不许复活旧值"的判定落在"ERROR 帧仍在块里、而旧值必须 absent"那一步上，那一步真的
        // 会咬：值/详情只要有一个残留就红。
        return true;
    }
    if (step.expect_contains[0] != L'\0' && text.find(step.expect_contains) == std::wstring::npos) {
        why = L"the block text does not contain the expected value";
        return false;
    }
    if (step.expect_absent[0] != L'\0' && text.find(step.expect_absent) != std::wstring::npos) {
        why = L"the block text still contains a value that must be gone";
        return false;
    }
    for (std::wstring const& gone : step.absent_values) {
        if (!gone.empty() && text.find(gone) != std::wstring::npos) {
            why = L"the block still carries a value that this frame must have removed";
            return false;
        }
    }
    if (step.order_first[0] != L'\0' && step.order_second[0] != L'\0') {
        std::size_t const first = text.find(step.order_first);
        std::size_t const second = text.find(step.order_second);
        if (first == std::wstring::npos || second == std::wstring::npos || first >= second) {
            why = L"the entries are not in frame order";
            return false;
        }
    }
    if (step.tone == 0) {
        return true;
    }
    if (step.tone == 4 || step.tone == 5) {
        // 三档颜色判定：values 恰好三个，顺序固定为 ok/warn/error。tone 4 要求"两两不同 + ok 是正常
        // 档"并**记录**这一次真实渲染出来的三档色；tone 5 要求每条回到它自己记录过的色。这样 warn 与
        // error 是不是两档、恢复时 warn 有没有被折成正常档，都是可判定的，而不是只跟正常档比一次。
        if (step.values.size() != 3) {
            why = L"the three-state tone check needs exactly three subjects (ok/warn/error)";
            return false;
        }
        winrt::Windows::UI::Color observed[3]{};
        for (std::size_t index = 0; index < step.values.size(); ++index) {
            if (!color_of(runs, step.values[index], observed[index])) {
                why = L"a three-state subject is not painted in the block";
                return false;
            }
        }
        if (step.tone == 4) {
            if (same_color(observed[0], observed[1]) || same_color(observed[0], observed[2]) ||
                same_color(observed[1], observed[2])) {
                why = L"ok/warn/error do not use three distinct tones";
                return false;
            }
            if (g_normal_color_known && !same_color(observed[0], g_normal_value_color)) {
                why = L"the ok entry does not use the normal tone";
                return false;
            }
            g_normal_value_color = observed[0];
            g_normal_color_known = true;
            g_warn_state_color = observed[1];
            g_error_state_color = observed[2];
            g_state_colors_known = true;
            return true;
        }
        if (!g_state_colors_known || !g_normal_color_known) {
            why = L"no three-state tone baseline was recorded before this step";
            return false;
        }
        winrt::Windows::UI::Color const want[3] = {g_normal_value_color, g_warn_state_color,
                                                   g_error_state_color};
        for (std::size_t index = 0; index < 3; ++index) {
            if (!same_color(observed[index], want[index])) {
                why = index == 0   ? L"the ok entry did not return to the normal tone"
                      : index == 1 ? L"the warn entry did not return to its own tone"
                                   : L"the error entry did not return to its own tone";
                return false;
            }
        }
        return true;
    }
    winrt::Windows::UI::Color color{};
    if (step.tone == 3) {
        bool have = false;
        winrt::Windows::UI::Color shared{};
        for (std::wstring const& value : step.values) {
            winrt::Windows::UI::Color current{};
            if (!color_of(runs, value, current)) {
                why = L"an expected value is not painted in the block";
                return false;
            }
            if (!have) {
                shared = current;
                have = true;
            } else if (!same_color(shared, current)) {
                why = L"the entries of a failed frame do not share one tone";
                return false;
            }
        }
        if (have && g_normal_color_known && same_color(shared, g_normal_value_color)) {
            why = L"a failed frame still uses the normal tone";
            return false;
        }
        return true;
    }
    if (!color_of(runs, step.tone_subject, color)) {
        why = L"the subject run is not in the block";
        return false;
    }
    if (step.tone == 1) {
        if (!g_normal_color_known) {
            g_normal_value_color = color;  // 第一次看到正常档：记下来当后续比较的基准
            g_normal_color_known = true;
            return true;
        }
        if (!same_color(color, g_normal_value_color)) {
            why = L"the run is not painted in the normal tone";
            return false;
        }
        return true;
    }
    // tone == 2：必须与正常档**不同**（warn/error 各有自己的位置，不能被折成正常色）。
    if (!g_normal_color_known) {
        why = L"no normal tone baseline was recorded before this step";
        return false;
    }
    if (same_color(color, g_normal_value_color)) {
        why = L"the run uses the normal tone where a distinct tone is required";
        return false;
    }
    return true;
}

// UI 线程：有界采样（边等边泵消息）直到 PASS 或 deadline。PASS/FAIL/NOT-RUN 三态都要有明确
// 结论：超时是 NOT-RUN，绝不当成通过。
void run_feed_check(unsigned index, unsigned deadline_ms) {
    g_check_ran = true;
    g_check_pass = false;
    g_check_detail.clear();
    std::vector<FeedStep> steps;
    build_feed_steps(g_options.frame_script, steps);
    if (index >= steps.size()) {
        g_check_detail = L"the step index is out of range";
        return;
    }
    FeedStep const& step = steps[index];
    if (step.owner_death && !g_owner_child_ready) {
        g_check_ran = false;  // NOT-RUN：辅助生产者进程没起来，这一条真的没被执行过
        g_check_detail = L"the auxiliary producer process is not available (fixture missing?)";
        return;
    }
    DWORD const deadline = ::GetTickCount() + deadline_ms;
    std::wstring why;
    unsigned packed_previous = 0xffffffffu;
    for (;;) {
        if (g_probe.hook_status != nullptr) {
            deskfeed::payload::HookProbeStatus status{};
            status.size = deskfeed::payload::kProbeStatusSize;
            status.version = deskfeed::payload::kProbeStatusVersion;
            if (g_probe.hook_status(&status, static_cast<unsigned>(sizeof(status))) == 1) {
                unsigned const packed = status.hook_installed | (status.retired << 1) |
                                        (status.still_installed << 2) | (status.in_flight << 3) |
                                        (status.pending << 4) | (status.completion_expected << 5) |
                                        (status.dangling << 6) | (status.done_signaled << 7);
                if (packed != packed_previous) {
                    deskfeed::log_format(L"in-check: installed=%u retired=%u still=%u in_flight=%u "
                                         L"pending=%u expected=%u dangling=%u done=%u",
                                         status.hook_installed, status.retired,
                                         status.still_installed, status.in_flight, status.pending,
                                         status.completion_expected, status.dangling,
                                         status.done_signaled);
                    packed_previous = packed;
                }
            }
        }
        if (evaluate_step(step, why)) {
            g_check_pass = true;
            return;
        }
        if (static_cast<long>(::GetTickCount() - deadline) >= 0) {
            break;
        }
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::Sleep(20);
    }
    g_check_detail = why;
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
        // 生产路径：负载自己 attach 隔离邮箱、自己把帧渲染进宿主的岛。**不做** inline 应用：
        // XAML 元素有线程亲和性，内容必须由负载经真实 hook 编组到 UI 线程。
        //
        // 帧必须在 `DeskFeedStart` **之前**就位并真的发出去：负载线程 attach 之后立刻读一次最新帧，
        // 读到 0 行就不会插块（也就没有"帧 → 块"这条路可验）。**默认步（空脚本）也走这条路**——
        // 旧实现只在有脚本时才建 producer，于是默认 `--feed` 连邮箱都没有，什么都没证明。
        std::vector<FeedStep> steps;
        build_feed_steps(g_options.frame_script, steps);
        bool const owner_death_script = !steps.empty() && steps.front().owner_death;
        auto publish_step = [](FeedStep const& step) -> bool {
            for (Frame const& frame : step.frames) {
                if (!g_producer.publish(frame)) {
                    return false;
                }
            }
            return !step.closed || g_producer.publish_closed();
        };
        // 生产收尾：**由工作线程**发起 `DeskFeedStop`，绝不在 UI 线程上做。负载的退休门禁要求向
        // T（= 我们的 UI 线程）发一次跨线程 `WM_NULL` 并拿到应答；如果 Stop 就跑在 T 上，T 正卡在
        // 这次调用里，那个 ping 永远拿不到应答 ⇒ 必然误报"未退休"。注入器的真实形态也正是从
        // **另一条**线程远程调用它。窗口必须活到 Stop 结束，因为收尾的 ping 打的就是它。
        auto finish_feed = []() {
            if (g_feed_retired) {
                return;  // 对抗路径（挂住 UI / 慢执行）已经拿到过"可以卸载"
            }
            DWORD const stopped = g_probe.feed_stop(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(10000)));
            g_feed_stop_ran = true;
            g_feed_stop_result = stopped;
            g_feed_retired = stopped == 1;
            deskfeed::log_format(L"DeskFeedStop -> %lu (%s)", stopped,
                                 stopped == 1 ? L"retired" : L"NOT retired");
            if (stopped != 1) {
                g_failed = true;
            }
        };

        bool producer_ready = false;
        if (owner_death_script) {
            // 邮箱属主是**另一条进程**：探针自己不建 producer（自己建就成了"自己发帧冒充 owner
            // 死亡"）。父进程先把实例仲裁拿到手（与主进程同一个名字），再拉起子进程；辅助进程真的
            // 发布之后才继续。起不来就 NOT-RUN + 失败，绝不带着一个证明不了的前提去跑生产入口。
            std::wstring spawn_error;
            std::string claim_error;
            if (!g_feed_claim.acquire(g_options.scope, claim_error)) {
                spawn_error.clear();
                for (char const character : claim_error) {
                    spawn_error.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
                }
            } else if (spawn_producer_child(g_options.scope, spawn_error)) {
                DisplayFrameConsumer peek;
                std::string peek_error;
                bool published = false;
                for (int attempt = 0; attempt < 200 && !published; ++attempt) {
                    if (!peek.attached()) {
                        peek.attach(g_options.scope, peek_error);
                    } else {
                        Frame current{};
                        published = peek.read(current) && !current.items.empty() &&
                                    current.items[0].value == kOwnerValueUtf8;
                    }
                    if (!published) {
                        ::Sleep(50);
                    }
                }
                peek.detach();
                if (published) {
                    g_owner_child_ready = true;
                    producer_ready = true;
                    deskfeed::log_format(L"ownerdeath: the auxiliary producer (pid %lu) published its "
                                         L"frame; this host is not the mailbox owner",
                                         ::GetProcessId(g_producer_child));
                } else {
                    spawn_error = L"the auxiliary producer did not publish within the deadline";
                }
            }
            if (!producer_ready) {
                deskfeed::log_format(L"NOT-RUN: ownerdeath (%s)", spawn_error.c_str());
                g_failed = true;
            }
        } else {
            std::string producer_error;
            if (!g_producer.create(g_options.scope, producer_error)) {
                deskfeed::log_format(L"cannot create the isolated mock producer: %S",
                                     producer_error.c_str());
                g_failed = true;
            } else if (!publish_step(steps.front())) {
                deskfeed::log_line(L"the isolated mock producer refused the first frames");
                g_failed = true;
            } else {
                producer_ready = true;
                deskfeed::log_format(L"frame script <%s>: %zu step(s); step 0 published before "
                                     L"DeskFeedStart",
                                     g_options.frame_script.c_str(), steps.size());
            }
        }
        if (!producer_ready) {
            // fail closed：没有"已发布的帧"就没有可验的"帧 → 块"，绝不带着这个前提去跑生产入口。
            deskfeed::log_line(L"the feed precondition (a live mailbox with a published frame) was not "
                               L"met; the production entry point was not run");
            finish_feed();  // 没有 Start 也要走一次 Stop：门禁与"能不能卸"由它给结论
            g_producer.close();
            stop_producer_child();
            g_feed_claim.release();
            ::PostMessageW(g_window, WM_CLOSE, 0, 0);
            return 0;
        }
        // R5：把宿主的岛登记成托盘目标。返回 0 = 负载拒绝了这个窗口，那是失败而不是跳过。
        if (g_probe.set_observer != nullptr) {
            g_probe.set_observer(1);  // 只记录回调阶段（只读诊断），不改变任何行为
        }
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
        // ABI：`DeskFeedStart(void* scope_or_null)` 收的是**目标进程里的 NUL 结尾宽字符串**
        // （注入器也是把 `scope.c_str()` 写进目标进程再远程调用），nullptr = 生产实例。
        // 这里曾经把 `&std::wstring` 当参数传进去：负载按字符串读会读到对象头，于是直接以
        // "请求非法(2)" 拒绝——自测宿主只有在真的跑起来之后才会暴露这条。
        std::wstring const scope = g_options.scope;
        DWORD const started = g_probe.feed_start(
            scope.empty() ? nullptr : const_cast<wchar_t*>(scope.c_str()));
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
        // 逐步发帧 + 由 UI 线程读宿主自己的树判定。PASS/FAIL/NOT-RUN 三态都写进日志：
        // NOT-RUN（超时，或这一步需要的东西不存在）**不算通过**，也不允许把整轮当绿。
        for (size_t index = 0; index < steps.size(); ++index) {
            if (index != 0) {
                if (steps[index].kill_owner) {
                    // 真的结束那条辅助生产者进程：负载是靠**它的真实进程句柄已触发**清块的，
                    // 不是靠探针发一个 closed 帧冒充。
                    deskfeed::log_line(L"ownerdeath: ending the auxiliary producer process now");
                    stop_producer_child();
                } else if (!publish_step(steps[index])) {
                    deskfeed::log_format(L"NOT-RUN: %s (the producer refused the frame)",
                                         steps[index].name);
                    g_failed = true;
                    continue;
                }
            }
            ::ResetEvent(g_check_done);
            g_check_ran = false;
            g_check_pass = false;
            if (::PostMessageW(g_window, kCheckMessage, static_cast<WPARAM>(index), 0) == FALSE) {
                deskfeed::log_format(L"NOT-RUN: %s (the check request was not delivered)",
                                     steps[index].name);
                g_failed = true;
                continue;
            }
            if (::WaitForSingleObject(g_check_done, 8000) != WAIT_OBJECT_0) {
                deskfeed::log_format(L"NOT-RUN: %s (the UI thread did not answer within the deadline)",
                                     steps[index].name);
                g_failed = true;
                continue;
            }
            if (!g_check_ran) {
                deskfeed::log_format(L"NOT-RUN: %s (%s)", steps[index].name, g_check_detail.c_str());
                g_failed = true;
                continue;
            }
            if (g_check_pass) {
                deskfeed::log_format(L"PASS: %s", steps[index].name);
            } else {
                deskfeed::log_format(L"FAIL: %s (%s)", steps[index].name, g_check_detail.c_str());
                g_failed = true;
            }
            log_hook_status(steps[index].name);
        }
        ::PostMessageW(g_window, kRunMessage, 0, 0);
        ::WaitForSingleObject(g_ui_done, 30000);
        log_hook_status(L"before DeskFeedStop");
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
            // 收尾门禁读的是 `g_feed_stop_ran && g_feed_stop_result == 1`，而 `finish_feed()` 在
            // `g_feed_retired` 为真时直接返回——所以"生产 Stop 真的跑过、结论是可以卸载"必须在
            // **这里**记下来。漏记过一次：短超时 0 与重试 1 都拿到了、状态 POD 也报
            // `unload_safe=1 / dangling=0`，门禁却因 ran=0 把模块留在映射里，白判一次未退休。
            g_feed_stop_ran = true;
            g_feed_stop_result = retry;
            if (!ok) {
                g_failed = true;
            }
        }
        finish_feed();  // 窗口还活着：收尾的 ping 要打到它，之后才允许关窗口
        log_hook_status(L"after DeskFeedStop");
        g_producer.close();  // 隔离 scope 的邮箱随最后一个句柄关闭而消失
        stop_producer_child();  // 幂等：判定循环提前 continue 时也不能留下辅助生产者进程
        g_feed_claim.release();  // 全部 probe producer 都结束了，才归还实例仲裁
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
    g_check_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_check_done == nullptr) {
        deskfeed::log_line(L"cannot create the frame-check event");
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

    // 收尾次序是有意的：宿主自己那棵树是**本线程**持有的 COM 引用，必须在 XAML source 与 manager
    // 析构之前松开（它们在函数返回时析构）；两个收尾事件只在这条 UI 线程上被使用，而工作线程已经
    // 拿到**真实**退出证明，所以关掉它们不会与任何等待者并发（无 UAF）。工作线程没真退出时什么都
    // 不碰——宁可让进程退出时统一回收，也不在别人还在用的时候动这些东西。
    if (worker_exited) {
        stop_producer_child();  // ownerdeath 的辅助生产者进程必须真的结束，绝不留下孤儿
        g_producer.close();     // 工作线程早退/BLOCKED 时可能还留着邮箱句柄
        g_feed_claim.release(); // 实例仲裁也只在全部 producer 结束后才归还
        g_tray_grid = nullptr;  // 松开宿主自己那棵树的引用（树本身仍由 XAML source 持有）
        if (g_check_done != nullptr) {
            ::CloseHandle(g_check_done);
            g_check_done = nullptr;
        }
        if (g_ui_done != nullptr) {
            ::CloseHandle(g_ui_done);
            g_ui_done = nullptr;
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

        if (options.feed) {
            // 生产收尾由**工作线程**发起并记下结论（见 worker 里 `finish_feed` 的说明：Stop 跑在
            // T 上会让负载的 `WM_NULL` ping 永远拿不到应答）。这里只读那次调用的结论，绝不补一次
            // UI 线程上的 Stop。
            gate.stop_ran = g_feed_stop_ran && g_feed_stop_result == 1;
            if (!gate.stop_ran) {
                deskfeed::log_format(L"DeskFeedStop did not report a safe retirement (ran=%d result=%lu)",
                                     g_feed_stop_ran ? 1 : 0, g_feed_stop_result);
                g_failed = true;
            }
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
