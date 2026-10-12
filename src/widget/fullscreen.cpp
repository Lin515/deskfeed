// 全屏判定（见 fullscreen.h）。这里只有窗口查询，没有任何状态：每次调用都重新问系统。
//
// 判据的来历：隐藏悬浮窗这件事只在“用户显然在看别的东西”时才该发生，所以宁可漏判也不能误判——
// 每一条失败路径都返回“不是全屏”。
#include "widget/fullscreen.h"

// WIN32_LEAN_AND_MEAN 的 windows.h 不带 shellapi.h，而 SHQueryUserNotificationState 与
// QUERY_USER_NOTIFICATION_STATE 在它里面。
#include <shellapi.h>

#include <iterator>

namespace deskfeed::widget {
namespace {

// 桌面外壳窗口：它们本来就能盖满整块屏幕（Progman 就是壁纸那一层），前台是它们的时候用户
// 正在看桌面，这时候把悬浮窗藏起来是错的。
bool is_shell_window(wchar_t const* class_name) {
    return ::wcscmp(class_name, L"Progman") == 0 || ::wcscmp(class_name, L"WorkerW") == 0 ||
           ::wcscmp(class_name, L"Shell_TrayWnd") == 0 ||
           ::wcscmp(class_name, L"Shell_SecondaryTrayWnd") == 0;
}

std::wstring rect_text(RECT const& rect) {
    return L"(" + std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L")-(" +
           std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom) + L")";
}

// 失败或没命中时留在 detail 里的话：调用方只拿它拼日志与状态原因，不参与判定。
void note(std::wstring& detail, std::wstring const& text) {
    if (!detail.empty()) {
        detail += L"; ";
    }
    detail += text;
}

}  // namespace

wchar_t const* fullscreen_signal_name(FullscreenSignal signal) {
    switch (signal) {
        case FullscreenSignal::kMonitorCovered:
            return L"foreground-monitor-covered";
        case FullscreenSignal::kD3DFullScreenExclusive:
            return L"d3d-fullscreen-exclusive";
        case FullscreenSignal::kNone:
            break;
    }
    return L"none";
}

FullscreenSignal fullscreen_foreground(std::wstring* detail) {
    if (detail == nullptr) {
        return FullscreenSignal::kNone;
    }
    detail->clear();

    wchar_t class_name[256] = L"";
    HWND const foreground = ::GetForegroundWindow();
    if (foreground == nullptr) {
        note(*detail, L"no foreground window");
    } else if (::GetClassNameW(foreground, class_name, static_cast<int>(std::size(class_name))) <= 0) {
        // 类名读不出来就没法排除桌面外壳，那条排除也就没法保证：不隐藏。
        note(*detail, L"foreground class unavailable, error=" + std::to_wstring(::GetLastError()));
    }

    // B：独占全屏。先问它，因为那种情况下前台窗口的类名与矩形都不可信（有些游戏的前台窗口
    // 是个 0x0 或者看不见的幕布），量矩形那一路永远命中不了。
    QUERY_USER_NOTIFICATION_STATE notification = QUNS_NOT_PRESENT;
    HRESULT const queried = ::SHQueryUserNotificationState(&notification);
    if (SUCCEEDED(queried)) {
        if (notification == QUNS_RUNNING_D3D_FULL_SCREEN) {
            note(*detail, L"SHQueryUserNotificationState=QUNS_RUNNING_D3D_FULL_SCREEN class=" +
                              (class_name[0] == L'\0' ? std::wstring(L"unavailable")
                                                      : std::wstring(class_name)));
            return FullscreenSignal::kD3DFullScreenExclusive;
        }
        note(*detail, L"SHQueryUserNotificationState=" + std::to_wstring(notification));
    } else {
        // 查询失败：不拿它当判据，但留个痕迹，免得日后对账时看不出 B 为什么从没命中过。
        note(*detail, L"SHQueryUserNotificationState failed, hr=" + std::to_wstring(queried));
    }

    if (foreground == nullptr || class_name[0] == L'\0') {
        return FullscreenSignal::kNone;
    }
    if (!is_shell_window(class_name)) {
        // 桌面外壳与任务栏不算：它们本来就能盖满屏幕。
        if (::IsWindowVisible(foreground) == FALSE) {
            note(*detail, L"foreground is not visible");
            return FullscreenSignal::kNone;
        }
        if (::IsIconic(foreground) != FALSE) {
            note(*detail, L"foreground is minimized");
            return FullscreenSignal::kNone;
        }
    } else {
        note(*detail, L"foreground is a shell window");
        return FullscreenSignal::kNone;
    }

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    HMONITOR const monitor = ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &monitor_info) == FALSE) {
        note(*detail, L"monitor info unavailable, error=" + std::to_wstring(::GetLastError()));
        return FullscreenSignal::kNone;
    }

    RECT rect{};
    if (::GetWindowRect(foreground, &rect) == FALSE) {
        // 拿不到矩形：判不出来，不能当作全屏。
        note(*detail, L"foreground rect unavailable, error=" + std::to_wstring(::GetLastError()));
        return FullscreenSignal::kNone;
    }

    note(*detail, std::wstring(L"class=") + class_name + L" rect=" + rect_text(rect) +
                      L" monitor=" + rect_text(monitor_info.rcMonitor));

    // 完整盖住 **rcMonitor**（不是 rcWork）：任务栏可见时最大化的窗口只盖到工作区，那不该算全屏。
    RECT const& bounds = monitor_info.rcMonitor;
    if (rect.left > bounds.left || rect.top > bounds.top || rect.right < bounds.right ||
        rect.bottom < bounds.bottom) {
        return FullscreenSignal::kNone;
    }
    return FullscreenSignal::kMonitorCovered;
}

}  // namespace deskfeed::widget
