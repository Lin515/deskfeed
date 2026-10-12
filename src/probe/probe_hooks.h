#pragma once

#include <windows.h>

#include <string>

#include "probe/observer_pod.h"

// 探针自己装的**第三方 `WH_GETMESSAGE` 钩子**与宿主侧的 ack 观测。
//
// 它要回答的问题是负载那条收尾证明到底成不成立，正确的顺序是：
//
//   负载回调：执行体 → 析构 → `CallNextHookEx`（**第三方钩子在这里跑**并可能泵消息）
//             → 负载最后一步 `SetEvent(done)` → 裸返回
//   收尾线程：等到 done → 向 T 的窗口发一次跨线程 `WM_NULL` → 只有这次 SendMessage 成功返回，
//             才证明钩子帧已经弹掉 → 才归还自引用
//
// 所以：**第三方钩子在 CallNext 里记下的 sentinel 必须早于 done**；而 ack（宿主窗口收到那次
// `WM_NULL`）必须晚于 done、并且**不能**在第三方钩子还处于 CallNext 帧里时被处理。
//
// 与之配套的对抗 failpoint 是"把 done 提前到 CallNext 之前"：那时收尾线程会立刻发 ping，ping
// 会被第三方钩子在 CallNext 帧里泵到——观测上就是
// `ack.hits_while_third_party_active > 0`，一个**硬失败**（不是"少记了一条 sentinel"这种软条件）。
//
// 为什么第三方钩子只在"负载正处在自己的 CallNext"时泵消息：`WH_GETMESSAGE` 对同一条线程每次取
// 消息都会跑，负载的钩子只在自己那次取消息时在场。若不加这个门，别的消息检索期间的泵也会看到
// 迟到的 ping，从而把正常顺序误判成失败。
namespace deskfeed::probe {

// ack 窗口（宿主自己的 STATIC 子窗口）收到的观测。
struct AckObservation {
    unsigned hits = 0;                       // 收到的 WM_NULL 次数
    unsigned hits_while_third_party_active = 0;  // 其中"第三方钩子正在负载 CallNext 里"的次数
    unsigned long long last_tick = 0;
};

struct ThirdPartyObservation {
    unsigned entries = 0;      // 回调进入次数
    unsigned pumps = 0;        // 真正泵过消息的次数（只在负载 CallNext 帧里）
    unsigned sentinels = 0;    // 泵期间处理过的消息条数（sentinel）
    unsigned ping_in_frame = 0;// 泵期间处理到"发往 ack 窗口的 WM_NULL"的次数（对抗证据）
    unsigned long long last_sentinel_tick = 0;
};

// 在 `thread_id` 上装第三方钩子。必须**早于**负载的钩子安装（负载的 `CallNextHookEx` 才会到它）。
// `phase` 可以为空（那就永不泵消息，只记进入次数）。`ack_window` 是宿主自己的 STATIC 子窗口，
// 它在自己的线程上被子类化（底层仍是系统 STATIC 过程，绝不是负载模块的窗口过程）。
bool third_party_install(DWORD thread_id, HookPhasePod* phase, HWND ack_window, std::wstring& error);

// 恢复导入表……不，恢复窗口过程并卸钩。卸载必须在**它自己的线程**上做，或者宿主已经确定
// 目标线程不再取消息；探针在收尾时调用它，失败只记日志（不改变被测对象的状态）。
bool third_party_uninstall(std::wstring& error);

ThirdPartyObservation third_party_observation();
AckObservation ack_observation();

// 清零观测（每一轮对抗开始前调用）。
void third_party_reset();

}  // namespace deskfeed::probe
