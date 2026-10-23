#include "platform/screens.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace deskfeed {
namespace {

struct MonitorCollector {
    std::vector<ScreenInfo>* out = nullptr;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* collector = reinterpret_cast<MonitorCollector*>(param);
    if (collector == nullptr || collector->out == nullptr) {
        return TRUE;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }

    ScreenInfo screen;
    screen.device_name = info.szDevice;
    screen.bounds = info.rcMonitor;
    screen.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (::EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) != FALSE &&
        device.DeviceID[0] != L'\0') {
        screen.id = device.DeviceID;
    } else {
        // 拿不到接口路径就退回显示设备名。它不如接口路径稳定，但比“没有标识”可解释。
        screen.id = info.szDevice;
    }
    if (screen.id.empty()) {
        return TRUE;
    }

    for (ScreenInfo const& existing : *collector->out) {
        if (_wcsicmp(existing.id.c_str(), screen.id.c_str()) == 0) {
            return TRUE;
        }
    }
    collector->out->push_back(std::move(screen));
    return TRUE;
}

std::vector<ScreenInfo> enumerate_screens_unsorted() {
    std::vector<ScreenInfo> screens;
    MonitorCollector collector;
    collector.out = &screens;
    ::EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&collector));
    return screens;
}

void sort_and_label(std::vector<ScreenInfo>& screens) {
    std::sort(screens.begin(), screens.end(), [](ScreenInfo const& left, ScreenInfo const& right) {
        if (left.bounds.left != right.bounds.left) {
            return left.bounds.left < right.bounds.left;
        }
        if (left.bounds.top != right.bounds.top) {
            return left.bounds.top < right.bounds.top;
        }
        return _wcsicmp(left.id.c_str(), right.id.c_str()) < 0;
    });
    for (std::size_t index = 0; index < screens.size(); ++index) {
        screens[index].label = L"屏幕 " + std::to_wstring(index + 1);
        if (screens[index].primary) {
            screens[index].label += L"（主）";
        }
    }
}

bool same_id(std::wstring const& left, std::wstring const& right) {
    return !left.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring screen_id_for_monitor(HMONITOR monitor) {
    if (monitor == nullptr) {
        return std::wstring();
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return std::wstring();
    }
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (::EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) == FALSE ||
        device.DeviceID[0] == L'\0') {
        return info.szDevice;
    }
    return device.DeviceID;
}

}  // namespace

std::vector<ScreenInfo> enumerate_screens() {
    std::vector<ScreenInfo> screens = enumerate_screens_unsorted();
    sort_and_label(screens);
    return screens;
}

std::wstring primary_screen_id() {
    for (ScreenInfo const& screen : enumerate_screens()) {
        if (screen.primary) {
            return screen.id;
        }
    }
    return std::wstring();
}

std::wstring screen_id_for_window(HWND window) {
    if (window == nullptr || ::IsWindow(window) == FALSE) {
        return std::wstring();
    }
    return screen_id_for_monitor(::MonitorFromWindow(window, MONITOR_DEFAULTTONULL));
}

std::vector<TaskbarWindowInfo> enumerate_taskbar_windows() {
    struct Search {
        std::vector<TaskbarWindowInfo> windows;
    };

    Search search;
    ::EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* search = reinterpret_cast<Search*>(param);
            if (search == nullptr || ::IsWindow(window) == FALSE) {
                return TRUE;
            }
            wchar_t class_name[64]{};
            if (::GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) == 0) {
                return TRUE;
            }
            if (_wcsicmp(class_name, L"Shell_TrayWnd") != 0 &&
                _wcsicmp(class_name, L"Shell_SecondaryTrayWnd") != 0) {
                return TRUE;
            }
            std::wstring const screen_id = screen_id_for_window(window);
            if (screen_id.empty()) {
                return TRUE;
            }
            for (TaskbarWindowInfo const& existing : search->windows) {
                if (existing.window == window) {
                    return TRUE;
                }
            }
            TaskbarWindowInfo info;
            info.window = window;
            info.thread_id = ::GetWindowThreadProcessId(window, nullptr);
            info.screen_id = screen_id;
            if (info.thread_id != 0) {
                search->windows.push_back(std::move(info));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    std::vector<ScreenInfo> const screens = enumerate_screens();
    for (TaskbarWindowInfo& window : search.windows) {
        for (ScreenInfo const& screen : screens) {
            if (same_id(screen.id, window.screen_id)) {
                window.label = screen.label;
                window.primary = screen.primary;
                break;
            }
        }
    }
    // 按屏幕顺序输出：找到每块任务栏所在屏幕在屏幕表里的位置；找不到的排到最后。
    std::sort(search.windows.begin(), search.windows.end(),
              [&screens](TaskbarWindowInfo const& left, TaskbarWindowInfo const& right) {
                  auto rank = [&screens](TaskbarWindowInfo const& window) -> std::size_t {
                      for (std::size_t index = 0; index < screens.size(); ++index) {
                          if (same_id(screens[index].id, window.screen_id)) {
                              return index;
                          }
                      }
                      return screens.size();
                  };
                  std::size_t const left_rank = rank(left);
                  std::size_t const right_rank = rank(right);
                  if (left_rank != right_rank) {
                      return left_rank < right_rank;
                  }
                  return left.window < right.window;
              });
    return search.windows;
}

}  // namespace deskfeed
