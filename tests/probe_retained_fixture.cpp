// 测试专用**退休门禁夹具** DLL：给"DeskFeedStop 报未退休时调用方绝不 FreeLibrary"这条硬门禁
// 提供机械证据。
//
// 证据的形状（为什么不能只看退出码）：
//   * 子进程"结束了"这件事**不是** FreeLibrary 的证据——进程退出时系统本来就回收整张映射表，
//     那条路径与本用例要证明的"调用方没有卸"，在观测上是两回事；
//   * 所以本模块在 `DllMain(DLL_PROCESS_DETACH)` 里只看 `lpReserved`：**只有 `lpReserved ==
//     nullptr`（普通 detach = 真的有人在 `FreeLibrary`）才写标记文件**；进程退出
//     （`lpReserved != nullptr`）**绝不写**；
//   * 于是 `Stop == 0 ⇒ 子进程 exit 1 且标记文件 ABSENT`，`Stop == 1 ⇒ refs == 1 且标记
//     PRESENT`，两轮互为对照。把 `probe/host_gate.h::unload_allowed` 变异成"无视未退休"之后，
//     Stop=0 那一轮会真的卸载（标记出现、exit 0）⇒ 用例必红。
//
// 加载器锁纪律（写标记这件"重活"的唯一理由）：
//   * `DESKFEED_FIXTURE_DETACH_MARKER` 在 **DLL_PROCESS_ATTACH** 时用栈上定长缓冲 cache 到
//     static 里（`GetEnvironmentVariableW`，不分配、不建堆对象）；DETACH 只用这份 cache；
//   * DETACH 里只调 `CreateFileW` / `WriteFile` / `CloseHandle`，不碰 CRT 文件 API、不碰堆、
//     不碰 user32/COM。这是**测试专用**合成模块的刻意选择：生产负载的 DETACH 只关自己的句柄，
//     绝不写文件（见 docs/native-safety.md）。它绕不过任何生产门禁——门禁在调用方那一侧。
//
// 导出面：
//   * 生产 ABI `DeskFeedStart` / `DeskFeedStop` / `DeskFeedGetStatus`，行为由环境变量切换
//     （`DESKFEED_FIXTURE_STOP_CODE` 默认 1 = 可卸载）；
//   * 探针 ABI **故意全部失败/拒绝**：本夹具不建 XAML 岛、不装钩子、没有诊断走查。导出这些符号
//     只是让"缺导出"那道门禁认得出它们；任何拿它跑探针矩阵的人都会看到探针自己的断言变红
//     （sites=0 / watch=0 / set_tray_window=0），不会被一个"假装成功"的夹具骗成绿色。
//
// 本文件只被工具用例按同目录路径 LoadLibrary 进**用例自己 spawn 的**子进程，不部署、不安装、
// 不进任何交付二进制。
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>

#include "payload/probe_api.h"
#include "payload/status_pod.h"

namespace {

// ATTACH 时 cache 的标记文件路径。DETACH 里只用它，不再查环境、不再分配。
wchar_t g_marker_path[512] = {};
bool g_marker_configured = false;

void cache_marker_path() {
    DWORD const length = ::GetEnvironmentVariableW(L"DESKFEED_FIXTURE_DETACH_MARKER", g_marker_path,
                                                   static_cast<DWORD>(std::size(g_marker_path)));
    g_marker_configured = length > 0 && length < std::size(g_marker_path);
}

// 只在普通 detach（= 真的 FreeLibrary）里调用。定长栈缓冲 + 三个最朴素的内核调用。
void write_detach_marker() {
    if (!g_marker_configured) {
        return;
    }
    HANDLE const file = ::CreateFileW(g_marker_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char const text[] = "freed\n";
    DWORD written = 0;
    ::WriteFile(file, text, sizeof(text) - 1, &written, nullptr);
    ::CloseHandle(file);
}

std::uint32_t env_u32(wchar_t const* name, std::uint32_t fallback) {
    wchar_t buffer[64]{};
    DWORD const length =
        ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return fallback;
    }
    return static_cast<std::uint32_t>(::wcstoul(buffer, nullptr, 10));
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(instance);
        cache_marker_path();
    } else if (reason == DLL_PROCESS_DETACH) {
        // 关键区分：nullptr = 普通 detach（FreeLibrary）；非 nullptr = 进程退出，系统回收。
        if (reserved == nullptr) {
            write_detach_marker();
        }
    }
    return TRUE;
}

// ---- 生产 ABI（调用方只按名字解析；本夹具是合成模块，不做任何真实工作）--------------------

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStart(void* scope) {
    (void)scope;
    return env_u32(L"DESKFEED_FIXTURE_START_CODE", 1);
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedStop(void* encoded_timeout) {
    (void)encoded_timeout;  // 冻结 ABI：超时值编码在指针本身里，夹具不解引用
    // 默认 1 = 可以卸载；DESKFEED_FIXTURE_STOP_CODE=0 演"未退休"，调用方据此**绝不** FreeLibrary。
    return env_u32(L"DESKFEED_FIXTURE_STOP_CODE", 1);
}

extern "C" __declspec(dllexport) DWORD WINAPI DeskFeedGetStatus(void* raw) {
    if (raw == nullptr) {
        return 0;
    }
    auto* const pod = static_cast<deskfeed::payload::PayloadStatusPod*>(raw);
    if (pod->size != sizeof(*pod) || pod->abi != deskfeed::payload::kPayloadStatusAbi) {
        pod->abi = 0;  // 与真实负载同一套语义：不认就写回 abi = 0
        return 0;
    }
    pod->width = deskfeed::payload::kPayloadStatusWidth;
    pod->started = env_u32(L"DESKFEED_FIXTURE_STATUS_STARTED", 1);
    pod->thread_alive = env_u32(L"DESKFEED_FIXTURE_STATUS_THREAD_ALIVE", 0);
    pod->unload_safe = env_u32(L"DESKFEED_FIXTURE_STATUS_UNLOAD_SAFE", 1);
    pod->dangling = env_u32(L"DESKFEED_FIXTURE_STATUS_DANGLING", 0);
    pod->mailbox_attached = 0;
    pod->producer_alive = 0;
    pod->xaml_block_present = 0;
    pod->producer_pid_low = 0;
    pod->producer_pid_high = 0;
    pod->frame_generation = 0;
    pod->feed_failures = 0;
    pod->producer_start_time = 0;
    pod->reserved = 0;
    for (std::size_t index = 0; index < deskfeed::payload::kPayloadScopeCapacity; ++index) {
        pod->scope[index] = L'\0';
    }
    return 1;
}

// ---- 探针 ABI：**故意 fail closed**（见文件头说明）---------------------------------------

extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeStart() { return 0; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSites() { return 0; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeWatch() { return 0; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRun(int mode) {
    (void)mode;
    return 0;
}
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetHost(HWND window) { (void)window; }
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeStop() {}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeDangling() {
    return 1;  // 不证明安全：悬挂判据在这一份夹具里永远报告"有未证明的悬挂"
}
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetSlowApply(unsigned ms) { (void)ms; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbePostApply(unsigned thread, unsigned ms) {
    (void)thread;
    (void)ms;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeRetire(unsigned ms) {
    (void)ms;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHook(int install) {
    (void)install;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeBlockVisible() { return 0; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeTeardown(unsigned ms) {
    (void)ms;
    return 0;
}
extern "C" __declspec(dllexport) void __stdcall DeskFeedProbeSetInstallDelay(unsigned ms) { (void)ms; }
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeHookStatus(
    deskfeed::payload::HookProbeStatus* out, unsigned bytes) {
    (void)out;
    (void)bytes;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetObserver(int enable) {
    (void)enable;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeObserverStage(
    deskfeed::payload::HookProbeStage* out, unsigned bytes) {
    (void)out;
    (void)bytes;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetCompletionMutation(unsigned mode) {
    (void)mode;
    return 0;
}
extern "C" __declspec(dllexport) int __stdcall DeskFeedProbeSetTrayWindow(HWND window) {
    (void)window;
    return 0;  // 拒绝这个窗口：本夹具没有托盘目标，绝不假装布防成功
}
