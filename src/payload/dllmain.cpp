// 注入进 explorer.exe 的负载 DLL 的入口。
//
// DllMain 只做最小的事：记住模块句柄、关掉线程通知、准备清理事件。其余全部在导出的入口起的
// 工作线程上跑——在 DllMain 里创建线程会在持有加载器锁时运行它。
//
// 导出的 ABI 是**给 CreateRemoteThread 用的**，所以每个都返回 `DWORD WINAPI f(void*)`：
// 返回 void 会让线程退出码变成未定义值，调用方无从判断成功。
//   DeskFeedStart(void* scope_or_null)   1 已启动 / 2 请求非法 / 3 已在运行 / 4 起线程失败
//   DeskFeedStop(void* timeout_ms)       1 已退休可以卸载 / 0 未退休（必须保留映射）
// 超时值按 `uintptr_t` 编码在参数指针本身里（不解引用），这样注入器不必在目标进程里再写一个 DWORD。
#include "display/reading.h"
#include "payload/action_worker.h"
#include "payload/hook_post.h"
#include "payload/host.h"
#include "payload/reg_hook.h"
#include "payload/taskbar_xaml.h"
#include "payload/xaml_tap.h"
#include "platform/log.h"
#include "platform/paths.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace {

HINSTANCE g_module = nullptr;
std::atomic<bool> g_started{false};

DWORD WINAPI payload_thread_entry(LPVOID parameter) {
    std::wstring const* scope = static_cast<std::wstring const*>(parameter);
    deskfeed::payload::payload_run(g_module, scope != nullptr ? *scope : std::wstring());
    delete scope;
    return 0;
}

// 远程传进来的 scope 指针指向目标进程里的一块缓冲（由注入器写好），但本模块仍然要有界、要防
// 坏指针。SEH 外壳里不得有需要析构的局部对象。
bool copy_scope_seh(void const* pointer, wchar_t* buffer, std::size_t capacity) {
    __try {
        for (std::size_t index = 0; index < capacity; ++index) {
            wchar_t const value = static_cast<wchar_t const*>(pointer)[index];
            buffer[index] = value;
            if (value == L'\0') {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    buffer[capacity - 1] = L'\0';
    return true;
}

std::wstring read_scope(void* pointer, bool& ok) {
    ok = true;
    if (pointer == nullptr) {
        return std::wstring();  // 生产实例
    }
    wchar_t buffer[64]{};
    if (!copy_scope_seh(pointer, buffer, std::size(buffer))) {
        ok = false;
        return std::wstring();
    }
    buffer[std::size(buffer) - 1] = L'\0';
    std::wstring scope(buffer);
    std::wstring error;
    if (!deskfeed::scope_validate(scope, error) && !scope.empty()) {
        ok = false;
        return std::wstring();
    }
    return scope;
}

void probe_placeholder_reading() {
    deskfeed::ReadingContent content;
    deskfeed::ReadingRow first;
    first.label = "probe";
    first.value = "waiting for a frame";
    first.tone = deskfeed::ReadingTone::Normal;
    first.tip = "the probe host inserts this until a frame arrives";
    content.rows.push_back(first);
    deskfeed::ReadingRow second;
    second.label = "mode";
    second.value = "structural self-test";
    second.tone = deskfeed::ReadingTone::Warn;
    content.rows.push_back(second);
    deskfeed::payload::taskbar_xaml_probe_set_reading(content);
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStart(void* scope_param) {
    bool scope_ok = false;
    std::wstring const scope = read_scope(scope_param, scope_ok);
    if (!scope_ok) {
        return 2;
    }
    if (g_module == nullptr) {
        return 4;
    }
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return 3;
    }
    auto* payload_scope = new (std::nothrow) std::wstring(scope);
    if (payload_scope == nullptr) {
        g_started.store(false);
        return 4;
    }
    HANDLE const thread = ::CreateThread(nullptr, 0, payload_thread_entry, payload_scope, 0, nullptr);
    if (thread == nullptr) {
        delete payload_scope;
        g_started.store(false);
        return 4;
    }
    ::CloseHandle(thread);
    return 1;
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStop(void* timeout_param) {
    unsigned timeout = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(timeout_param));
    if (timeout == 0 || timeout > 60000) {
        timeout = 10000;
    }

    deskfeed::payload::payload_request_stop();

    // 卸载门是**线程已终止**，不是事件、不是标志位：事件之后线程入口还有一条 `return`，那也是
    // 本模块的指令。
    HANDLE const thread = deskfeed::payload::payload_thread_handle();
    if (thread == nullptr) {
        // 线程从未起过：没有任何本模块的线程代码在跑。
        return 1;
    }
    if (::WaitForSingleObject(thread, timeout) != WAIT_OBJECT_0) {
        return 0;
    }
    return deskfeed::payload::payload_unload_safe() == 1 ? 1 : 0;
}

// 本模块同时充当 XAML 诊断的 TAP，所以 COM 的这两个入口也在它身上。
extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID class_id,
                                                                    REFIID interface_id,
                                                                    void** object) {
    return deskfeed::payload::tap_get_class_object(class_id, interface_id, object);
}

// 永远不答应卸载。XAML 只在它持有 TAP 对象时问这个，而模块是被注入的负载，何时卸载由注入器决定；
// 在这里答应会把模块从负载自己的线程脚下抽走。
extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

// ---------------------------------------------------------------------------
// 自测宿主（deskfeed-probe.exe）用的入口。它们让诊断这条路线的全部危险动作在一个自己拥有 XAML
// 岛的进程里先跑一遍，不必注入 explorer。
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeStart() {
    std::wstring error;
    if (!deskfeed::payload::xaml_tap_start(error)) {
        deskfeed::log_format(L"probe: diagnostics start failed: %s", error.c_str());
        return -1;
    }
    return static_cast<int>(deskfeed::payload::xaml_tap_sites().size());
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSites() {
    return static_cast<int>(deskfeed::payload::xaml_tap_sites().size());
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeWatch() {
    std::wstring error;
    if (!deskfeed::payload::taskbar_xaml_watch(error)) {
        deskfeed::log_format(L"probe: watch failed: %s", error.c_str());
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRun(int dump_only) {
    std::wstring error;
    bool visible = false;
    if (!deskfeed::payload::taskbar_xaml_probe_run(dump_only != 0, /*allow_placeholder=*/true, visible,
                                                   error)) {
        deskfeed::log_format(L"probe: run failed: %s", error.c_str());
        return 0;
    }
    return 1;
}

// 宿主线程就是树所属的线程：告诉负载不必再投递（也没有 Shell_TrayWnd 可投），并把宿主窗口交给
// 它当悬停提示的属主与钩子收尾的确认窗口。
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetHost(HWND window) {
    deskfeed::payload::taskbar_xaml_set_probe_inline(true);
    deskfeed::payload::taskbar_xaml_set_host_window(window);
    probe_placeholder_reading();
}

extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetSlowApply(unsigned ms) {
    deskfeed::payload::taskbar_xaml_probe_set_slow_apply(ms);
}

// 走真实的"投递到 UI 线程 + 收尾证明"这条路径（钩子门）。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbePostApply(unsigned thread_id,
                                                                     unsigned timeout_ms) {
    auto const posted =
        deskfeed::payload::hook_post_run(thread_id, deskfeed::payload::HookAction::Apply, timeout_ms);
    if (!posted.ok && !posted.error.empty()) {
        deskfeed::log_format(L"probe: post failed: %s", posted.error.c_str());
    }
    return posted.ok && posted.retired ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRetire(unsigned timeout_ms) {
    std::wstring detail;
    if (!deskfeed::payload::hook_post_retire(nullptr, 0, timeout_ms, detail)) {
        deskfeed::log_format(L"probe: retire failed: %s", detail.c_str());
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHook(int enable) {
    if (enable == 0) {
        std::wstring error;
        return deskfeed::payload::reg_hook_restore(error) ? 1 : 0;
    }
    std::wstring detail;
    bool const before = deskfeed::payload::reg_hook_probe_read(detail);
    deskfeed::log_format(L"probe: hook selftest before install (%s): %s", before ? L"passed" : L"failed",
                         detail.c_str());
    std::wstring error;
    if (!deskfeed::payload::reg_hook_install(error)) {
        deskfeed::log_format(L"probe: cannot install the reg hook: %s", error.c_str());
        return 0;
    }
    detail.clear();
    bool const after = deskfeed::payload::reg_hook_probe_read(detail);
    deskfeed::log_format(L"probe: hook selftest after install (%s): %s", after ? L"passed" : L"failed",
                         detail.c_str());
    return after ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeBlockVisible() {
    return deskfeed::payload::taskbar_xaml_block_visible() ? 1 : 0;
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHoverCells(long* out, int capacity) {
    return deskfeed::payload::taskbar_xaml_probe_hover_cells(out, capacity);
}

// 走真实拆除路径。返回 1 = 所有既定门禁都拿到（可以卸载）；0 = 有一条没拿到（必须保留映射）。
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeTeardown(unsigned timeout_ms) {
    std::wstring detail;
    if (!deskfeed::payload::taskbar_xaml_detach(timeout_ms, detail)) {
        deskfeed::log_format(L"probe: teardown did not retire everything: %s", detail.c_str());
        return 0;
    }
    return 1;
}

extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeStop() {
    deskfeed::payload::xaml_tap_stop();
}

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeDangling() {
    return deskfeed::payload::hook_post_dangling() ? 1 : 0;
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
        deskfeed::payload::payload_prepare();
    }
    return TRUE;
}
