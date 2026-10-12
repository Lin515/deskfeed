// 悬浮窗用到的两个纯逻辑层（显示排版、界面偏好）的离线用例。
//
// 这一份只覆盖不依赖窗口的部分：`src/display/reading.*` 与 `src/widget/preferences.*`。
// 建窗、菜单、悬停提示那部分在 `widget_window_tests.cpp`（隐藏窗口门禁）。
//
// 全部离线：不读凭据、不联网、不注入 explorer、不碰用户正在跑的实例，偏好用例只在系统临时目录
// 里读写自己的 settings.ini。文本用例刻意只用通用标签（cpu / mem），不出现任何私有来源名。

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/utf8.h"
#include "display/frame.h"
#include "display/reading.h"
#include "ipc/settings.h"
#include "platform/paths.h"
#include "widget/preferences.h"

namespace {

int g_failures = 0;

void check(bool condition, std::string const& name) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}

void check_eq(std::string const& actual, std::string const& expected, std::string const& name) {
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  expected: <%s>\n  actual:   <%s>\n", name.c_str(), expected.c_str(),
                actual.c_str());
}

void check_eq_size(std::size_t actual, std::size_t expected, std::string const& name) {
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n  expected: %zu\n  actual:   %zu\n", name.c_str(), expected, actual);
}

deskfeed::FrameItem item(std::string label, std::string value, deskfeed::ItemState state,
                         std::string detail) {
    deskfeed::FrameItem out;
    out.label = std::move(label);
    out.value = std::move(value);
    out.state = state;
    out.detail = std::move(detail);
    return out;
}

deskfeed::Frame content_frame(std::vector<deskfeed::FrameItem> items) {
    deskfeed::Frame frame;
    frame.kind = deskfeed::ViewKind::Content;
    frame.items = std::move(items);
    return frame;
}

deskfeed::ReadingContent rows_content(std::size_t count) {
    std::vector<deskfeed::FrameItem> items;
    for (std::size_t index = 0; index < count; ++index) {
        items.push_back(item("L" + std::to_string(index), "v", deskfeed::ItemState::Ok, ""));
    }
    return deskfeed::reading_from_frame(content_frame(std::move(items)));
}

// ---- 显示排版 ------------------------------------------------------------------------------

void test_tone_mapping() {
    using deskfeed::ItemState;
    using deskfeed::ReadingTone;
    check(deskfeed::reading_tone_of(ItemState::Ok) == ReadingTone::Normal, "tone ok");
    check(deskfeed::reading_tone_of(ItemState::Warn) == ReadingTone::Warn, "tone warn");
    check(deskfeed::reading_tone_of(ItemState::Error) == ReadingTone::Error, "tone error");
}

void test_waiting_and_error_frames() {
    deskfeed::Frame waiting;
    waiting.kind = deskfeed::ViewKind::Waiting;
    deskfeed::ReadingContent const waiting_content = deskfeed::reading_from_frame(waiting);
    check_eq_size(waiting_content.rows.size(), 0, "waiting has no rows");
    check(!waiting_content.failure, "waiting is not a failure");
    check(!waiting_content.alarm, "waiting is not an alarm");

    deskfeed::Frame error;
    error.kind = deskfeed::ViewKind::Error;
    error.failure = deskfeed::FailureKind::NoUrl;
    error.reason = "no url";
    deskfeed::ReadingContent const error_content = deskfeed::reading_from_frame(error);
    check_eq_size(error_content.rows.size(), 1, "error has one row");
    check(error_content.failure, "error frame is a failure");
    check_eq(error_content.rows[0].value, "ERROR", "error row text is ERROR");
    check(error_content.rows[0].tone == deskfeed::ReadingTone::Error, "error row tone");
    check(error_content.rows[0].label.empty(), "error row has no prefix");
    check_eq(error_content.rows[0].tip, "no url", "error row tip is the reason");
}

void test_content_states_and_alarm() {
    deskfeed::ReadingContent const content = deskfeed::reading_from_frame(content_frame({
        item("cpu", "54C", deskfeed::ItemState::Ok, "sampled 1s ago"),
        item("mem", "68%", deskfeed::ItemState::Warn, "near the limit"),
        item("disk", "full", deskfeed::ItemState::Error, "write failed"),
    }));
    check_eq_size(content.rows.size(), 3, "content rows");
    check(content.rows[0].tone == deskfeed::ReadingTone::Normal, "ok tone");
    check(content.rows[1].tone == deskfeed::ReadingTone::Warn, "warn keeps its own tone");
    check(content.rows[2].tone == deskfeed::ReadingTone::Error, "error tone");
    check(content.rows[1].state == deskfeed::ItemState::Warn, "warn state is preserved");
    check(!content.alarm, "normal content is not an alarm");

    deskfeed::Frame alarmed = content_frame({
        item("cpu", "54C", deskfeed::ItemState::Ok, "producer context"),
    });
    alarmed.alarm = true;
    alarmed.reason = "transport failed";
    deskfeed::ReadingContent const alarmed_content = deskfeed::reading_from_frame(alarmed);
    check(alarmed_content.alarm, "alarm is its own flag");
    check(!alarmed_content.failure, "alarm with values is not failure (values are not lost)");
    check(alarmed_content.rows[0].tone == deskfeed::ReadingTone::Error, "alarm forces error tone");
    check(alarmed_content.rows[0].state == deskfeed::ItemState::Ok,
          "alarm does not rewrite the producer state");
    check_eq(alarmed_content.rows[0].value, "54C", "alarm keeps the value");
    check_eq(alarmed_content.rows[0].tip, "transport failed\nproducer context",
             "alarm tip is reason first, producer detail after");
    check_eq(alarmed_content.rows[0].detail, "producer context",
             "alarm keeps the producer detail for the expanded block");
    check_eq(deskfeed::reading_detail_text(alarmed_content),
             "transport failed\n\ncpu 54C\n  producer context",
             "alarm reason is stated once at the top");
}

void test_per_line_formula() {
    struct Case {
        std::size_t rows;
        std::size_t expected;
    };
    // 3 个及以下一行一个，更多时 ceil(rows/3)——与旧原生实现同一个公式。
    Case const cases[] = {
        {0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 2}, {5, 2}, {6, 2}, {7, 3},
        {8, 3}, {9, 3}, {10, 4}, {12, 4}, {13, 5}, {64, 22},
    };
    for (Case const& entry : cases) {
        check_eq_size(deskfeed::reading_per_line_for(entry.rows), entry.expected,
                      "per_line for " + std::to_string(entry.rows));
    }
}

void test_packing_upto_five_matches_legacy() {
    struct Case {
        std::size_t rows;
        std::size_t per_line;
        std::size_t lines;
    };
    Case const cases[] = {
        {0, 1, 0}, {1, 1, 1}, {2, 1, 2}, {3, 1, 3}, {4, 2, 2}, {5, 2, 3},
    };
    for (Case const& entry : cases) {
        deskfeed::ReadingContent const content = rows_content(entry.rows);
        std::size_t const per_line = deskfeed::reading_per_line_for(entry.rows);
        check_eq_size(per_line, entry.per_line, "auto per_line rows=" + std::to_string(entry.rows));
        std::vector<deskfeed::ReadingLine> const lines = deskfeed::reading_pack(content, per_line);
        check_eq_size(lines.size(), entry.lines, "auto lines rows=" + std::to_string(entry.rows));
        for (deskfeed::ReadingLine const& line : lines) {
            check_eq_size(line.hidden, 0, "<=5 rows never overflow");
        }
    }

    deskfeed::ReadingContent const five = rows_content(5);
    std::size_t const expected[] = {5, 3, 2, 2, 1};
    for (std::size_t per_line = 1; per_line <= 5; ++per_line) {
        check_eq_size(deskfeed::reading_pack(five, per_line).size(), expected[per_line - 1],
                      "sources_per_line=" + std::to_string(per_line));
    }
}

void test_bounded_grouping_keeps_everything_reachable() {
    deskfeed::ReadingContent const thirteen = rows_content(13);
    std::size_t const per_line = deskfeed::reading_per_line_for(13);
    check_eq_size(per_line, 5, "13 rows auto per_line");
    std::vector<deskfeed::ReadingLine> const lines = deskfeed::reading_pack(thirteen, per_line);
    check_eq_size(lines.size(), 3, "13 rows stay within 3 lines");
    check_eq_size(lines[0].segments.size(), deskfeed::kMaxSegmentsPerLine + 1,
                  "first line is capped plus the marker");
    check_eq_size(lines[0].hidden, 1, "first line hides one item");
    check_eq(deskfeed::reading_line_text(lines[0]),
             "L0 v · L1 v · L2 v · L3 v · …（另有 1 项）", "bounded line text");
    check_eq(lines[0].segments.back().value, "…（另有 1 项）", "marker text");
    check_eq(lines[0].segments.back().tip, "L4 v", "marker tip names the hidden row");

    // 64 条（协议上限）：每行 22 个，仍然只有 3 行，每行最多画 4 个 + 标记。
    std::vector<deskfeed::FrameItem> items;
    for (std::size_t index = 0; index < 64; ++index) {
        items.push_back(item("Item" + std::to_string(index), "value-" + std::to_string(index),
                             deskfeed::ItemState::Ok, ""));
    }
    deskfeed::ReadingContent const full =
        deskfeed::reading_from_frame(content_frame(std::move(items)));
    std::vector<deskfeed::ReadingLine> const full_lines =
        deskfeed::reading_pack(full, deskfeed::reading_per_line_for(64));
    check_eq_size(full_lines.size(), 3, "64 rows stay within 3 lines");
    check_eq_size(full_lines[0].hidden, 18, "64 rows first line hides 18");
    check_eq_size(full_lines[2].hidden, 16, "64 rows last line hides 16");

    // 行上画出来的 value 加上标记悬停里列出来的 value，必须覆盖全部 64 个。
    std::string accessible;
    for (deskfeed::ReadingLine const& line : full_lines) {
        for (deskfeed::ReadingRow const& segment : line.segments) {
            accessible += segment.tip.empty() ? segment.value : segment.tip;
            accessible += '\n';
        }
    }
    std::string const detail = deskfeed::reading_detail_text(full);
    for (std::size_t index = 0; index < 64; ++index) {
        std::string const needle = "value-" + std::to_string(index);
        check(accessible.find(needle) != std::string::npos,
              needle + " is reachable from a line or the overflow marker");
        check(detail.find(needle) != std::string::npos, "expanded detail lists " + needle);
    }
}

void test_marker_tone_reflects_hidden_rows() {
    auto build = [](std::size_t bad_index, deskfeed::ItemState bad_state) {
        std::vector<deskfeed::FrameItem> items;
        for (std::size_t index = 0; index < 13; ++index) {
            items.push_back(item("L" + std::to_string(index), "v",
                                 index == bad_index ? bad_state : deskfeed::ItemState::Ok, ""));
        }
        return deskfeed::reading_from_frame(content_frame(std::move(items)));
    };
    // 13 条：per_line = 5，每行只画 4 个。第 0 行折掉 {4}，第 1 行折掉 {9}。
    deskfeed::ReadingContent const clean = build(999, deskfeed::ItemState::Ok);
    std::vector<deskfeed::ReadingLine> const clean_lines =
        deskfeed::reading_pack(clean, deskfeed::reading_per_line_for(13));
    check(clean_lines[0].segments.back().tone == deskfeed::ReadingTone::Normal,
          "marker stays normal when every hidden row is normal");

    deskfeed::ReadingContent const warned = build(4, deskfeed::ItemState::Warn);
    std::vector<deskfeed::ReadingLine> const warned_lines =
        deskfeed::reading_pack(warned, deskfeed::reading_per_line_for(13));
    check(warned_lines[0].segments.back().tone == deskfeed::ReadingTone::Warn,
          "marker turns amber when a hidden row is warn");

    deskfeed::ReadingContent const errored = build(4, deskfeed::ItemState::Error);
    std::vector<deskfeed::ReadingLine> const errored_lines =
        deskfeed::reading_pack(errored, deskfeed::reading_per_line_for(13));
    check(errored_lines[0].segments.back().tone == deskfeed::ReadingTone::Error,
          "marker turns red when a hidden row is error");
    check_eq(deskfeed::reading_line_text(errored_lines[0]),
             deskfeed::reading_line_text(clean_lines[0]), "marker tone does not change the text");

    deskfeed::ReadingContent const later = build(9, deskfeed::ItemState::Error);
    std::vector<deskfeed::ReadingLine> const later_lines =
        deskfeed::reading_pack(later, deskfeed::reading_per_line_for(13));
    check(later_lines[0].segments.back().tone == deskfeed::ReadingTone::Normal,
          "a later line's error does not colour the first marker");
    check(later_lines[1].segments.back().tone == deskfeed::ReadingTone::Error,
          "the marker on the line that hides the error turns red");
}

void test_row_and_line_text() {
    deskfeed::ReadingRow with_label;
    with_label.label = "cpu";
    with_label.value = "54C";
    check_eq(deskfeed::reading_row_text(with_label), "cpu 54C", "row text with label");

    deskfeed::ReadingRow without_label;
    without_label.value = "ERROR";
    check_eq(deskfeed::reading_row_text(without_label), "ERROR", "row text without label");

    deskfeed::ReadingLine line;
    line.segments.push_back(with_label);
    line.segments.push_back(without_label);
    check_eq(deskfeed::reading_line_text(line), "cpu 54C · ERROR", "line joins with the separator");
}

void test_rows_view_shape() {
    deskfeed::ReadingContent const content = deskfeed::reading_from_frame(content_frame({
        item("cpu", "54C", deskfeed::ItemState::Ok, ""),
        item("mem", "68%", deskfeed::ItemState::Ok, ""),
        item("net", "up", deskfeed::ItemState::Ok, ""),
        item("disk", "full", deskfeed::ItemState::Error, ""),
        item("gpu", "idle", deskfeed::ItemState::Warn, ""),
    }));
    // 头部与旧量具逐字一致（正常帧不带 alarm）；warn 与 ok 都印 error=0。
    std::string const expected =
        "reading: rows=5 stale=no failed=no refresh_ms=30000\n"
        "row 0: error=0 prefix=<cpu> value=<54C>\n"
        "row 1: error=0 prefix=<mem> value=<68%>\n"
        "row 2: error=0 prefix=<net> value=<up>\n"
        "row 3: error=1 prefix=<disk> value=<full>\n"
        "row 4: error=0 prefix=<gpu> value=<idle>\n"
        "lines: auto (per_line=2, max 3) -> 3 line(s)\n"
        "  line 0: cpu 54C · mem 68%\n"
        "  line 1: net up · disk full\n"
        "  line 2: gpu idle\n"
        "lines: sources_per_line=1 -> 5 line(s)\n"
        "lines: sources_per_line=2 -> 3 line(s)\n"
        "lines: sources_per_line=3 -> 2 line(s)\n"
        "lines: sources_per_line=4 -> 2 line(s)\n"
        "lines: sources_per_line=5 -> 1 line(s)\n";
    check_eq(deskfeed::reading_rows_text(content, 30000), expected, "rows view shape");

    // 整体失效时头部追加 alarm=yes：只印 failed=no 会让人以为这一份是新鲜的。
    deskfeed::Frame alarmed = content_frame({item("cpu", "54C", deskfeed::ItemState::Ok, "")});
    alarmed.alarm = true;
    alarmed.reason = "boom";
    std::string const alarmed_text =
        deskfeed::reading_rows_text(deskfeed::reading_from_frame(alarmed), 30000);
    check(alarmed_text.rfind("reading: rows=1 stale=no failed=no refresh_ms=30000 alarm=yes", 0) == 0,
          "alarm is marked in the header");
}

void test_detail_and_dump_shape() {
    deskfeed::ReadingContent const content = deskfeed::reading_from_frame(content_frame({
        item("cpu", "54C", deskfeed::ItemState::Ok, "tip cpu"),
        item("mem", "68%", deskfeed::ItemState::Ok, ""),
        item("net", "up", deskfeed::ItemState::Ok, "tip net"),
        item("gpu", "idle", deskfeed::ItemState::Ok, ""),
    }));
    // 折行后的悬停文本：段内 tip 用换行连，dump 里换成 " / "；空 tip 不产生分隔符。
    std::string const expected =
        "cpu 54C · mem 68%     [tip cpu]\n"
        "net up · gpu idle     [tip net]\n"
        "\n"
        "--- 展开 ---\n"
        "cpu 54C\n"
        "  tip cpu\n"
        "mem 68%\n"
        "net up\n"
        "  tip net\n"
        "gpu idle\n"
        "--- 未显示的来源 ---\n"
        "（无）\n";
    check_eq(deskfeed::reading_dump_text(content), expected, "dump shape");

    deskfeed::ReadingContent const empty = deskfeed::reading_from_frame(content_frame({}));
    std::string const empty_expected =
        "（没有可显示的来源：窗口会整窗隐藏）\n"
        "\n"
        "--- 展开 ---\n"
        "（空）\n"
        "--- 未显示的来源 ---\n"
        "（无）\n";
    check_eq(deskfeed::reading_dump_text(empty), empty_expected, "empty dump shape");
}

// ---- 界面偏好 ------------------------------------------------------------------------------

std::wstring make_temp_dir() {
    wchar_t base[MAX_PATH] = {};
    DWORD const length = ::GetTempPathW(MAX_PATH, base);
    std::wstring dir = (length > 0 && length < MAX_PATH) ? std::wstring(base) : std::wstring(L".\\");
    if (!dir.empty() && dir.back() == L'\\') {
        dir.pop_back();
    }
    dir += L"\\deskfeed-widget-tests-" + std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void write_text(std::wstring const& path, std::wstring const& text) {
    std::wstring error;
    deskfeed::write_file_bytes(path, deskfeed::utf8_from_wide(text), error);
}

std::wstring value_of(deskfeed::SettingsFile& file, wchar_t const* section, wchar_t const* key) {
    std::wstring value;
    file.get(section, key, value);
    return value;
}

bool anchor_from(std::wstring const& dir, std::wstring const& right, std::wstring const& bottom,
                 deskfeed::widget::WindowPreferences& out) {
    std::wstring const path = dir + L"\\anchor.ini";
    write_text(path, L"[window]\r\nright=" + right + L"\r\nbottom=" + bottom + L"\r\n");
    deskfeed::SettingsFile file(path);
    std::wstring error;
    if (!file.load(error)) {
        return false;
    }
    out = deskfeed::widget::read_window_preferences(file);
    return true;
}

void test_preferences() {
    using namespace deskfeed;

    std::wstring const dir = make_temp_dir();
    std::wstring const path = dir + L"\\settings.ini";
    write_text(path,
               L"; 用户自己的注释，不许被界面切换弄丢\r\n"
               L"[source]\r\n"
               L"url=http://127.0.0.1:8099/snapshot\r\n"
               L"poll_ms=30000\r\n"
               L"\r\n"
               L"[custom]\r\n"
               L"keep=me\r\n");

    SettingsFile settings(path);
    std::wstring error;
    check(settings.load(error), "load the fixture settings file");

    widget::WindowPreferences defaults = widget::read_window_preferences(settings);
    check(!defaults.anchor_known, "anchor defaults to unknown");
    check(!defaults.expanded, "expanded defaults to false");
    check(defaults.topmost, "topmost defaults to true");
    check(!defaults.hidden, "hidden defaults to false");
    check(defaults.taskbar_component, "taskbar component defaults to true");

    // 负坐标合法：左侧/上方的显示器坐标本来就是负的。
    widget::WindowPreferences prefs = defaults;
    prefs.anchor_known = true;
    prefs.anchor = POINT{-1920, -200};
    prefs.expanded = true;
    prefs.topmost = false;
    prefs.hidden = true;
    check(widget::write_window_preferences(settings, prefs, &error), "write [window]");

    SettingsFile reread(path);
    check(reread.load(error), "reload from disk");
    check(value_of(reread, L"source", L"url") == L"http://127.0.0.1:8099/snapshot",
          "[source] url untouched");
    check(value_of(reread, L"source", L"poll_ms") == L"30000", "[source] poll_ms untouched");
    check(value_of(reread, L"custom", L"keep") == L"me", "unknown section untouched");
    widget::WindowPreferences round_trip = widget::read_window_preferences(reread);
    check(round_trip.anchor_known && round_trip.anchor.x == -1920 && round_trip.anchor.y == -200,
          "negative anchor round trips");
    check(round_trip.expanded && !round_trip.topmost && round_trip.hidden, "flags round trip");

    // 共享同一份 SettingsFile：主进程先换源，界面再切一次置顶，新地址必须还在。
    check(settings.set(L"source", L"url", L"http://127.0.0.1:9000/new", nullptr, error),
          "master switches the url");
    check(settings.save(error), "master commits the url");
    widget::WindowPreferences after_switch = widget::read_window_preferences(settings);
    after_switch.topmost = true;
    check(widget::write_window_preferences(settings, after_switch, &error),
          "ui toggle on the shared instance");
    SettingsFile shared(path);
    shared.load(error);
    check(value_of(shared, L"source", L"url") == L"http://127.0.0.1:9000/new",
          "shared instance: the ui toggle did not resurrect the old url");

    // 反证：窗口若自己持有一份旧快照，落盘时会把新地址覆盖回去——这就是必须共享实例的原因。
    SettingsFile stale(path);
    stale.load(error);
    check(settings.set(L"source", L"url", L"http://127.0.0.1:9000/newer", nullptr, error),
          "master switches again");
    check(settings.save(error), "master commits again");
    widget::WindowPreferences stale_prefs = widget::read_window_preferences(stale);
    stale_prefs.expanded = false;
    check(widget::write_window_preferences(stale, stale_prefs, &error),
          "stale instance writes its snapshot");
    SettingsFile clobbered(path);
    clobbered.load(error);
    check(value_of(clobbered, L"source", L"url") == L"http://127.0.0.1:9000/new",
          "a stale SettingsFile clobbers the newer url (why the window must not own one)");

    // 锚点解析的边界：溢出/越界/非数字/尾随垃圾/只有一个键都退回默认位置。
    widget::WindowPreferences parsed{};
    check(anchor_from(dir, L"2560", L"1392", parsed) && parsed.anchor_known &&
              parsed.anchor.x == 2560 && parsed.anchor.y == 1392,
          "plain anchor parses");
    check(anchor_from(dir, L"-1920", L"-1080", parsed) && parsed.anchor_known &&
              parsed.anchor.x == -1920,
          "negative anchor parses");
    check(anchor_from(dir, L"  2560  ", L"1392", parsed) && parsed.anchor_known &&
              parsed.anchor.x == 2560,
          "whitespace around the value is tolerated");
    check(anchor_from(dir, L"99999999999999999999", L"1392", parsed) && !parsed.anchor_known,
          "an overflowing anchor falls back to the default position");
    check(anchor_from(dir, L"2147483648", L"1392", parsed) && !parsed.anchor_known,
          "an anchor outside LONG falls back to the default position");
    check(anchor_from(dir, L"abc", L"1392", parsed) && !parsed.anchor_known,
          "a non-numeric anchor falls back to the default position");
    check(anchor_from(dir, L"2560x", L"1392", parsed) && !parsed.anchor_known,
          "trailing junk falls back to the default position");

    std::wstring const partial_path = dir + L"\\partial.ini";
    write_text(partial_path, L"[window]\r\nright=2560\r\n");
    SettingsFile partial(partial_path);
    partial.load(error);
    check(!widget::read_window_preferences(partial).anchor_known,
          "only one position key means the anchor stays unknown");

    // 保存失败：把临时文件路径占成一个目录，save() 就建不了临时文件。
    // 断言：返回 false、错误非空、磁盘上的 URL 一个字节都没变。
    std::wstring const blocked_path = dir + L"\\blocked.ini";
    write_text(blocked_path, L"[source]\r\nurl=http://127.0.0.1:9000/keep\r\n");
    std::wstring const temporary =
        blocked_path + L".tmp-" + std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(temporary.c_str(), nullptr);
    SettingsFile blocked(blocked_path);
    check(blocked.load(error), "load the blocked fixture");
    widget::WindowPreferences blocked_prefs = widget::read_window_preferences(blocked);
    blocked_prefs.topmost = false;
    std::wstring save_error;
    check(!widget::write_window_preferences(blocked, blocked_prefs, &save_error),
          "a failing save reports failure");
    check(!save_error.empty(), "a failing save explains itself");
    SettingsFile after_failure(blocked_path);
    after_failure.load(error);
    check(value_of(after_failure, L"source", L"url") == L"http://127.0.0.1:9000/keep",
          "a failing save leaves the url on disk untouched");
    ::RemoveDirectoryW(temporary.c_str());

    ::DeleteFileW(path.c_str());
    ::DeleteFileW((dir + L"\\anchor.ini").c_str());
    ::DeleteFileW(partial_path.c_str());
    ::DeleteFileW(blocked_path.c_str());
    ::RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    test_tone_mapping();
    test_waiting_and_error_frames();
    test_content_states_and_alarm();
    test_per_line_formula();
    test_packing_upto_five_matches_legacy();
    test_bounded_grouping_keeps_everything_reachable();
    test_marker_tone_reflects_hidden_rows();
    test_row_and_line_text();
    test_rows_view_shape();
    test_detail_and_dump_shape();
    test_preferences();

    if (g_failures == 0) {
        std::printf("widget tests: all passed\n");
        return 0;
    }
    std::printf("widget tests: %d failure(s)\n", g_failures);
    return 1;
}
