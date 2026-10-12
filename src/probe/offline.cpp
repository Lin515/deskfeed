#include "probe/offline.h"

#include "core/health.h"
#include "core/json.h"
#include "core/snapshot.h"
#include "core/utf8.h"
#include "display/frame.h"
#include "display/reading.h"
#include "platform/paths.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deskfeed::probe {
namespace {

long long now_unix_ms() {
    FILETIME stamp{};
    ::GetSystemTimeAsFileTime(&stamp);
    ULARGE_INTEGER value{};
    value.LowPart = stamp.dwLowDateTime;
    value.HighPart = stamp.dwHighDateTime;
    // FILETIME 是 1601-01-01 起的 100ns 计数；换成 Unix 毫秒要减掉 1970 年之前那一段。
    constexpr long long kEpochOffsetMs = 11644473600000LL;
    return static_cast<long long>(value.QuadPart / 10000ull) - kEpochOffsetMs;
}

bool parse_view_kind(std::string_view name, ViewKind& out) {
    if (name == "waiting") { out = ViewKind::Waiting; return true; }
    if (name == "content") { out = ViewKind::Content; return true; }
    if (name == "error") { out = ViewKind::Error; return true; }
    return false;
}

bool parse_failure_kind(std::string_view name, FailureKind& out) {
    // 名字与 frame_failure_name() 同源，避免两处各写一张表。
    for (int value = 0; value <= static_cast<int>(FailureKind::Internal); ++value) {
        auto const kind = static_cast<FailureKind>(value);
        char const* const candidate = frame_failure_name(kind);
        if (candidate != nullptr && name == candidate) {
            out = kind;
            return true;
        }
    }
    return false;
}

bool parse_item_state(std::string_view name, ItemState& out) {
    if (name == "ok") { out = ItemState::Ok; return true; }
    if (name == "warn") { out = ItemState::Warn; return true; }
    if (name == "error") { out = ItemState::Error; return true; }
    return false;
}

std::string field_name(std::string const& context, std::string_view name) {
    return context + "." + std::string(name);
}

// 诊断用 Frame JSON：只认这一组字段，类型不对或名字不认识就整份拒绝（量具宁可拒绝也不猜）。
bool parse_frame_json(std::string_view text, Frame& out, std::string& error) {
    JsonValue root;
    if (!json_parse(text, root, error, JsonParseLimits{kMaxJsonDepth})) {
        return false;
    }
    if (!root.is_object()) {
        error = "the frame is not a JSON object";
        return false;
    }

    Frame frame;
    if (JsonValue const* kind = root.member("kind")) {
        if (!kind->is_string() || !parse_view_kind(kind->text, frame.kind)) {
            error = "kind must be one of waiting/content/error";
            return false;
        }
    }
    if (JsonValue const* alarm = root.member("alarm")) {
        if (!alarm->is_bool()) {
            error = "alarm must be a boolean";
            return false;
        }
        frame.alarm = alarm->boolean;
    }
    if (JsonValue const* failure = root.member("failure")) {
        if (!failure->is_string() || !parse_failure_kind(failure->text, frame.failure)) {
            error = "failure must be a known failure name (see display/frame.cpp)";
            return false;
        }
    }
    if (JsonValue const* reason = root.member("reason")) {
        if (!reason->is_string()) {
            error = "reason must be a string";
            return false;
        }
        if (reason->text.size() > kFrameMaxReasonBytes) {
            error = "reason is longer than the frame allows";
            return false;
        }
        frame.reason = reason->text;
    }

    if (JsonValue const* items = root.member("items")) {
        if (!items->is_array()) {
            error = "items must be an array";
            return false;
        }
        if (items->items.size() > kMaxItems) {
            error = "items has more entries than the protocol allows";
            return false;
        }
        for (std::size_t index = 0; index < items->items.size(); ++index) {
            JsonValue const& entry = items->items[index];
            std::string const context = "items[" + std::to_string(index) + "]";
            if (!entry.is_object()) {
                error = context + " is not an object";
                return false;
            }
            FrameItem item;
            static constexpr std::pair<char const*, std::size_t> kTextLimits[] = {
                {"id", kMaxIdBytes},       {"label", kMaxLabelBytes},
                {"value", kMaxValueBytes}, {"detail", kMaxDetailBytes},
            };
            for (auto const& [name, limit] : kTextLimits) {
                JsonValue const* field = entry.member(name);
                if (field == nullptr) {
                    continue;
                }
                if (!field->is_string()) {
                    error = field_name(context, name) + " must be a string";
                    return false;
                }
                if (field->text.size() > limit) {
                    error = field_name(context, name) + " is longer than the protocol allows";
                    return false;
                }
            }
            if (JsonValue const* id = entry.member("id")) {
                item.id = id->text;
            }
            if (JsonValue const* label = entry.member("label")) {
                item.label = label->text;
            }
            if (JsonValue const* value = entry.member("value")) {
                item.value = value->text;
            }
            if (JsonValue const* detail = entry.member("detail")) {
                item.detail = detail->text;
            }
            if (JsonValue const* state = entry.member("state")) {
                if (!state->is_string() || !parse_item_state(state->text, item.state)) {
                    error = field_name(context, "state") + " must be one of ok/warn/error";
                    return false;
                }
            }
            frame.items.push_back(std::move(item));
        }
    }

    // 没写 kind 时按内容推断，方便手写夹具；写了就按写的来（不覆盖显式意图）。
    if (root.member("kind") == nullptr) {
        frame.kind = frame.items.empty() ? ViewKind::Waiting : ViewKind::Content;
    }

    out = std::move(frame);
    return true;
}

bool read_all_bytes(std::wstring const& path, std::string& out, std::wstring& error) {
    bool exists = false;
    if (!deskfeed::read_file_bytes(path, out, exists, error)) {
        return false;
    }
    if (!exists) {
        error = L"file not found: " + path;
        return false;
    }
    return true;
}

// 快照 v1 → 显示模型：严格校验 → health 状态机（含**真实 Unix 毫秒**的到期判定）→ 显示帧。
bool frame_from_snapshot_file(std::string const& body, Frame& out, std::wstring& error) {
    Snapshot snapshot;
    ValidationResult const validation = snapshot_validate(body, snapshot);
    if (!validation.ok) {
        error = L"cannot parse the snapshot: " + deskfeed::wide_from_utf8(validation.message);
        return false;
    }

    long long const now = now_unix_ms();
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(snapshot, now);
    health.tick(now);  // 首份即过期也要按到期呈现，而不是等下一次轮询

    // View → Frame 只有 display/frame.h 的一份实现（master.cpp 用的是同一个函数）。
    out = frame_from_view(health.view());
    return true;
}

bool load_snapshot_frame(std::wstring const& path, Frame& out, int& exit_code) {
    std::string body;
    std::wstring error;
    if (!read_all_bytes(path, body, error)) {
        write_stderr(L"reading: " + error + L"\n");
        exit_code = 1;
        return false;
    }
    if (!frame_from_snapshot_file(body, out, error)) {
        write_stderr(L"reading: " + error + L"\n");
        exit_code = 1;
        return false;
    }
    exit_code = 0;
    return true;
}

bool load_diagnostic_frame(std::wstring const& path, Frame& out, int& exit_code) {
    std::string body;
    std::wstring error;
    if (!read_all_bytes(path, body, error)) {
        write_stderr(L"frame-reading: " + error + L"\n");
        exit_code = 1;
        return false;
    }
    std::string parse_error;
    if (!parse_frame_json(body, out, parse_error)) {
        write_stderr(L"frame-reading: cannot parse the frame: " +
                     deskfeed::wide_from_utf8(parse_error) + L"\n");
        exit_code = 1;
        return false;
    }
    exit_code = 0;
    return true;
}

}  // namespace

void write_stdout(std::wstring const& text) {
    if (text.empty()) {
        return;
    }
    HANDLE const target = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (target == nullptr || target == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (::GetConsoleMode(target, &mode)) {
        DWORD written = 0;
        ::WriteConsoleW(target, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // 被重定向：写 UTF-8 字节，读数里的 ℃ 与 — 才能穿过管道与文件。
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes,
                          nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(target, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void write_stderr(std::wstring const& text) {
    if (text.empty()) {
        return;
    }
    HANDLE const target = ::GetStdHandle(STD_ERROR_HANDLE);
    if (target == nullptr || target == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (::GetConsoleMode(target, &mode)) {
        DWORD written = 0;
        ::WriteConsoleW(target, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    int const bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes,
                          nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(target, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

bool run_offline(OfflineMode mode, std::wstring const& path, long long refresh_ms, int& exit_code) {
    Frame frame;
    bool loaded = false;
    switch (mode) {
        case OfflineMode::Reading:
        case OfflineMode::ReadingRows:
            loaded = load_snapshot_frame(path, frame, exit_code);
            break;
        case OfflineMode::FrameReading:
            loaded = load_diagnostic_frame(path, frame, exit_code);
            break;
    }
    if (!loaded) {
        return false;
    }

    ReadingContent const content = reading_from_frame(frame);
    std::string const utf8 = mode == OfflineMode::ReadingRows
                                 ? reading_rows_text(content, refresh_ms)
                                 : reading_dump_text(content);
    write_stdout(deskfeed::wide_from_utf8(utf8));
    exit_code = 0;
    return true;
}

}  // namespace deskfeed::probe
