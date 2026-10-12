#include "widget/preferences.h"

#include <cerrno>
#include <climits>
#include <cwchar>

namespace deskfeed::widget {
namespace {

std::wstring trim(std::wstring const& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 严格解析一个十进制整数：整串都要是数字，溢出（ERANGE）算失败。
// 负值合法（多显示器上左侧/上方的屏幕坐标本来就是负的），所以不能拿“非负”当判据。
bool parse_integer(std::wstring const& text, long long& value) {
    std::wstring const body = trim(text);
    if (body.empty()) {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    long long const parsed = std::wcstoll(body.c_str(), &end, 10);
    if (end == body.c_str() || (end != nullptr && *end != L'\0')) {
        return false;
    }
    if (errno == ERANGE) {
        // 溢出：wcstoll 会回 LLONG_MAX / LLONG_MIN，不能把它当成一个坐标。
        return false;
    }
    value = parsed;
    return true;
}

bool read_bool(SettingsFile const& settings, std::wstring_view section, std::wstring_view key,
               bool fallback) {
    std::wstring text;
    if (!settings.get(section, key, text)) {
        return fallback;
    }
    std::wstring const body = trim(text);
    if (_wcsicmp(body.c_str(), L"true") == 0) {
        return true;
    }
    if (_wcsicmp(body.c_str(), L"false") == 0) {
        return false;
    }
    return fallback;  // 写坏的值按缺省处理，不因为一行坏值改变窗口行为
}

}  // namespace

WindowPreferences read_window_preferences(SettingsFile const& settings) {
    WindowPreferences preferences;

    // 位置要两个键都在、都读得出数字、并且能放进 POINT 的 LONG 才算“锚点已知”；只有一个、
    // 写坏了或溢出就按默认右下角落位，而不是把窗口放到 (0,0) 或一个溢出后的垃圾坐标。
    // 负值合法：多显示器上左侧/上方的屏幕坐标本来就是负的。
    std::wstring right_text;
    std::wstring bottom_text;
    long long right = 0;
    long long bottom = 0;
    if (settings.get(L"window", L"right", right_text) &&
        settings.get(L"window", L"bottom", bottom_text) && parse_integer(right_text, right) &&
        parse_integer(bottom_text, bottom) && right >= LONG_MIN && right <= LONG_MAX &&
        bottom >= LONG_MIN && bottom <= LONG_MAX) {
        preferences.anchor = POINT{static_cast<LONG>(right), static_cast<LONG>(bottom)};
        preferences.anchor_known = true;
    }

    preferences.expanded = read_bool(settings, L"window", L"expanded", false);
    preferences.topmost = read_bool(settings, L"window", L"topmost", true);
    preferences.hidden = read_bool(settings, L"window", L"hidden", false);
    preferences.taskbar_component = read_bool(settings, L"taskbar", L"component", true);
    return preferences;
}

bool write_window_preferences(SettingsFile& settings, WindowPreferences const& preferences,
                              std::wstring* error) {
    bool changed = false;
    std::wstring local_error;

    if (preferences.anchor_known) {
        settings.set(L"window", L"right", std::to_wstring(preferences.anchor.x), &changed,
                     local_error);
        settings.set(L"window", L"bottom", std::to_wstring(preferences.anchor.y), &changed,
                     local_error);
    }
    settings.set(L"window", L"expanded", preferences.expanded ? L"true" : L"false", &changed,
                 local_error);
    settings.set(L"window", L"topmost", preferences.topmost ? L"true" : L"false", &changed,
                 local_error);
    settings.set(L"window", L"hidden", preferences.hidden ? L"true" : L"false", &changed,
                 local_error);

    // 只写 [window]：地址与调度归主进程的命令事务，任务栏组件开关归主进程那个动作
    // （各写一次，不重复写）。
    if (!settings.save(local_error)) {
        // 保存失败：把这次脏改动丢回磁盘状态，内存与磁盘保持一致，然后如实报失败。
        std::wstring reload_error;
        settings.load(reload_error);
        if (error != nullptr) {
            *error = local_error;
        }
        return false;
    }
    return true;
}

}  // namespace deskfeed::widget
