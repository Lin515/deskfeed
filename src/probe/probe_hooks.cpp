#include "probe/probe_hooks.h"

#include <atomic>

namespace deskfeed::probe {
namespace {

std::atomic<HHOOK> g_hook{nullptr};
std::atomic<bool> g_in_callback{false};
std::atomic<StageReader> g_reader{nullptr};
HWND g_ack_window = nullptr;
WNDPROC g_ack_original = nullptr;
std::atomic<DWORD> g_hook_thread{0};

// 负载此刻是否正跑在它自己的 `CallNextHookEx` 帧里。取一份 R3 快照来判断：这一层不缓存状态，
// 也不读负载的内存——common ABI 刻意不提供共享 POD。
bool inside_payload_callnext() {
    StageReader const reader = g_reader.load(std::memory_order_acquire);
    if (reader == nullptr) {
        return false;
    }
    HookProbeStage stage{};
    stage.size = static_cast<std::uint32_t>(kProbeStageSize);
    stage.version = kProbeStageVersion;
    if (reader(&stage, static_cast<unsigned>(kProbeStageSize)) != 1) {
        return false;  // 当前没有正在被观测的安装
    }
    return stage_inside_callnext(stage);
}

std::atomic<unsigned> g_entries{0};
std::atomic<unsigned> g_pumps{0};
std::atomic<unsigned> g_sentinels{0};
std::atomic<unsigned> g_ping_in_frame{0};
std::atomic<unsigned long long> g_last_sentinel_tick{0};

std::atomic<unsigned> g_ack_hits{0};
std::atomic<unsigned> g_ack_hits_in_frame{0};
std::atomic<unsigned long long> g_ack_last_tick{0};

// 宿主自己的 STATIC 子窗口的子类过程：只记录"收到了那次 WM_NULL"，然后交给系统原来的过程。
// 底层仍是 STATIC 的系统过程（不是负载模块里的任何窗口过程），所以它不会引入新的"返回屏障"。
LRESULT CALLBACK ack_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NULL) {
        g_ack_hits.fetch_add(1, std::memory_order_relaxed);
        g_ack_last_tick.store(::GetTickCount64(), std::memory_order_relaxed);
        if (inside_payload_callnext()) {
            // 这一条就是硬失败证据：收尾的 ping 在负载的 CallNext 帧里被处理了。
            g_ack_hits_in_frame.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return g_ack_original != nullptr ? ::CallWindowProcW(g_ack_original, window, message, wparam,
                                                        lparam)
                                     : ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK third_party_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code < 0 || g_reader.load(std::memory_order_acquire) == nullptr) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }
    // 重入保护：泵消息本身会再次触发本条线程的钩子，绝不能在泵里再泵。
    bool expected = false;
    if (!g_in_callback.compare_exchange_strong(expected, true)) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    g_entries.fetch_add(1, std::memory_order_relaxed);

    // 只有负载正处在自己的 CallNextHookEx 里时才泵——否则一条与本次编组无关的消息检索也会
    // 被当成对抗场景，得到假失败。
    if (inside_payload_callnext()) {
        g_pumps.fetch_add(1, std::memory_order_relaxed);
        MSG message{};
        for (int index = 0; index < 16 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++index) {
            g_sentinels.fetch_add(1, std::memory_order_relaxed);
            g_last_sentinel_tick.store(::GetTickCount64(), std::memory_order_relaxed);
            if (message.message == WM_QUIT) {
                // 泵消息**不得吞掉宿主自己的退出消息**：PeekMessage 已经把它从队列里取走，而
                // `DispatchMessage` 对 hwnd 为空的 WM_QUIT 什么都不做——丢掉它等于把宿主的
                // `GetMessage` 循环变成永远等不到退出的假死（`--third-party-hook-pump` 在收尾之后
                // 挂住就是这么来的）。重新入队，交给宿主自己的循环；**不 Dispatch**，并且立刻停止
                // 这一轮泵——重投的 WM_QUIT 会被下一次 PeekMessage 再取到，继续泵就是在反复重投。
                ::PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (g_ack_window != nullptr && message.hwnd == g_ack_window &&
                message.message == WM_NULL) {
                g_ping_in_frame.fetch_add(1, std::memory_order_relaxed);
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    g_in_callback.store(false, std::memory_order_release);
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace

bool third_party_install(DWORD thread_id, StageReader reader, HWND ack_window, std::wstring& error) {
    third_party_reset();
    if (thread_id == 0) {
        error = L"the third-party probe hook needs a target thread";
        return false;
    }
    if (reader == nullptr) {
        // 没有 R3 读取入口就没法判断"负载是否在自己的 CallNext 帧里"，那这个钩子只会产生假失败。
        // 缺必需接口必须由 feature gate 拦成 BLOCKED，绝不在这里退化成一个不会咬的量具。
        error = L"the third-party probe hook needs the payload's observer-stage reader (R3)";
        return false;
    }
    g_reader.store(reader, std::memory_order_release);
    g_ack_window = ack_window;

    if (ack_window != nullptr) {
        if (::IsWindow(ack_window) == FALSE) {
            error = L"the ack window does not exist";
            g_reader.store(nullptr, std::memory_order_release);
            return false;
        }
        // 子类化宿主自己的 STATIC 窗口：必须在窗口所属线程上做（调用方保证）。
        g_ack_original = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
            ack_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ack_proc)));
        if (g_ack_original == nullptr) {
            error = L"cannot subclass the ack window: " + std::to_wstring(::GetLastError());
            g_reader.store(nullptr, std::memory_order_release);
            return false;
        }
    }

    HHOOK const hook = ::SetWindowsHookExW(WH_GETMESSAGE, third_party_proc, nullptr, thread_id);
    if (hook == nullptr) {
        error = L"SetWindowsHookEx(WH_GETMESSAGE) failed: " + std::to_wstring(::GetLastError());
        if (ack_window != nullptr && g_ack_original != nullptr) {
            ::SetWindowLongPtrW(ack_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_ack_original));
            g_ack_original = nullptr;
        }
        g_ack_window = nullptr;
        g_reader.store(nullptr, std::memory_order_release);
        return false;
    }
    g_hook.store(hook);
    g_hook_thread.store(thread_id);
    return true;
}

bool third_party_uninstall(std::wstring& error) {
    error.clear();
    bool ok = true;

    HHOOK const hook = g_hook.exchange(nullptr);
    if (hook != nullptr && ::UnhookWindowsHookEx(hook) == FALSE) {
        error = L"UnhookWindowsHookEx failed: " + std::to_wstring(::GetLastError());
        ok = false;
    }

    if (g_ack_window != nullptr && g_ack_original != nullptr) {
        ::SetWindowLongPtrW(g_ack_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_ack_original));
        g_ack_original = nullptr;
    }
    g_ack_window = nullptr;
    g_reader.store(nullptr, std::memory_order_release);
    return ok;
}

ThirdPartyObservation third_party_observation() {
    ThirdPartyObservation observation;
    observation.entries = g_entries.load();
    observation.pumps = g_pumps.load();
    observation.sentinels = g_sentinels.load();
    observation.ping_in_frame = g_ping_in_frame.load();
    observation.last_sentinel_tick = g_last_sentinel_tick.load();
    return observation;
}

AckObservation ack_observation() {
    AckObservation observation;
    observation.hits = g_ack_hits.load();
    observation.hits_while_third_party_active = g_ack_hits_in_frame.load();
    observation.last_tick = g_ack_last_tick.load();
    return observation;
}

void third_party_reset() {
    g_entries.store(0);
    g_pumps.store(0);
    g_sentinels.store(0);
    g_ping_in_frame.store(0);
    g_last_sentinel_tick.store(0);
    g_ack_hits.store(0);
    g_ack_hits_in_frame.store(0);
    g_ack_last_tick.store(0);
}

}  // namespace deskfeed::probe
