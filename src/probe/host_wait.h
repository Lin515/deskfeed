#pragma once

#include <windows.h>

// 等到一条**自己起的**线程真实退出——探针宿主收尾的公共形状。
//
// 为什么单独抽出来并且必须"边等边泵消息"：宿主的工作线程与 UI 线程互相推进（工作线程
// `PostMessage`、UI 线程取消息）；如果收尾时死等一个句柄而不取消息，双方可以等成一个假死。
// 而"已经给它 post 过 `WM_CLOSE`"**不是**它退出的证据：消息只是请求，线程可能还在跑。所以在
// 归还任何模块引用之前，必须拿到它的真实 signal。
//
// 返回值 true **只**代表"线程真的退出了"；超时、句柄为空或等待失败一律 false（fail closed：调用方
// 不得把它当成"已经退干净"）。
namespace deskfeed::probe {

inline bool wait_thread_exit_pumping(HANDLE thread, DWORD timeout_ms) {
    if (thread == nullptr) {
        return false;
    }
    unsigned long long waited = 0;
    while (waited < timeout_ms) {
        DWORD const piece = 50;
        DWORD const index = ::MsgWaitForMultipleObjects(1, &thread, FALSE, piece, QS_ALLINPUT);
        if (index == WAIT_OBJECT_0) {
            return true;
        }
        if (index == WAIT_OBJECT_0 + 1) {
            MSG pending{};
            while (::PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                if (pending.message == WM_QUIT) {
                    // 不在这里退出：线程还没退出，提前退出等于把它丢在后面。
                    continue;
                }
                ::TranslateMessage(&pending);
                ::DispatchMessageW(&pending);
            }
        } else if (index != WAIT_TIMEOUT) {
            return false;
        }
        waited += piece;
    }
    // 时间用尽之后再看一眼：**只**以真实 signal 为准。
    return ::WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
}

}  // namespace deskfeed::probe
