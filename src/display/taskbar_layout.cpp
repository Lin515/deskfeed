#include "display/taskbar_layout.h"

#include <cwctype>

namespace deskfeed {
namespace {

std::wstring_view trim(std::wstring_view text) {
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

bool contains(std::vector<std::wstring> const& ids, std::wstring_view id) {
    for (std::wstring const& existing : ids) {
        if (taskbar_screen_ids_equal(existing, id)) {
            return true;
        }
    }
    return false;
}

void push_unique(std::vector<std::wstring>& ids, std::wstring const& id) {
    if (!contains(ids, id)) {
        ids.push_back(id);
    }
}

}  // namespace

bool taskbar_screen_id_valid(std::wstring_view id) {
    if (id.empty() || id.size() > kMaxTaskbarScreenIdChars) {
        return false;
    }
    for (wchar_t const ch : id) {
        if (ch < 0x20 || ch == 0x7F || ch == L';') {
            return false;
        }
    }
    return true;
}

bool taskbar_screen_ids_equal(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring> parse_taskbar_screen_list(std::wstring_view text, bool* malformed) {
    if (malformed != nullptr) {
        *malformed = false;
    }
    std::vector<std::wstring> ids;
    std::size_t start = 0;
    for (;;) {
        std::size_t const separator = text.find(L';', start);
        std::wstring_view const piece =
            trim(separator == std::wstring_view::npos ? text.substr(start)
                                                      : text.substr(start, separator - start));
        if (!piece.empty()) {
            if (taskbar_screen_id_valid(piece)) {
                std::wstring const id(piece);
                if (!contains(ids, id)) {
                    if (ids.size() >= kMaxTaskbarScreens) {
                        if (malformed != nullptr) {
                            *malformed = true;
                        }
                    } else {
                        ids.push_back(id);
                    }
                }
            } else if (malformed != nullptr) {
                *malformed = true;
            }
        }
        if (separator == std::wstring_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return ids;
}

bool format_taskbar_screen_list(std::vector<std::wstring> const& ids, std::wstring& out) {
    if (ids.size() > kMaxTaskbarScreens) {
        return false;
    }
    std::vector<std::wstring> unique;
    unique.reserve(ids.size());
    for (std::wstring const& id : ids) {
        if (!taskbar_screen_id_valid(id) || contains(unique, id)) {
            return false;
        }
        unique.push_back(id);
    }
    std::wstring text;
    for (std::size_t index = 0; index < unique.size(); ++index) {
        if (index != 0) {
            text.push_back(L';');
        }
        text += unique[index];
    }
    out = std::move(text);
    return true;
}

TaskbarScreenSelection select_taskbar_screens(TaskbarScreenSelectionInput const& input) {
    TaskbarScreenSelection result;

    std::vector<std::wstring> requested;
    if (input.configured) {
        requested.reserve(input.enabled.size());
        for (std::wstring const& id : input.enabled) {
            if (taskbar_screen_id_valid(id)) {
                push_unique(requested, id);
            }
        }
    } else if (!input.primary.empty()) {
        requested.push_back(input.primary);
    }
    result.requested_any = !requested.empty();

    std::vector<std::wstring> available;
    available.reserve(input.available.size());
    for (std::wstring const& id : input.available) {
        if (taskbar_screen_id_valid(id)) {
            push_unique(available, id);
        }
    }

    // 按 available 的顺序输出，菜单/日志因此与屏幕的物理顺序一致。
    for (std::wstring const& id : available) {
        if (contains(requested, id)) {
            result.selected.push_back(id);
        }
    }

    // 部分屏幕不可用不是退化：只有“要求了至少一块，却一块都没命中”才退化到主屏。
    if (result.selected.empty() && result.requested_any && !input.primary.empty() &&
        contains(available, input.primary)) {
        result.selected.push_back(input.primary);
        result.degraded = true;
    }
    return result;
}

}  // namespace deskfeed
