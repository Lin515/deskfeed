// 全部离线用例：协议解析与校验、健康/到期状态机、调度、命令行、控制通道线格式、
// 地址处理、设置落盘、进程绑定、以及回环 mock 生产端上的 HTTP 墙钟预算与取消。
// 不读凭据、不连外网、不注入 explorer、不碰用户正在跑的实例。
//
// 运行：build/bin/deskfeed-tests.exe [--filter <子串>]

// winsock2.h 必须先于 windows.h（否则 windows.h 会先拉进 winsock 1 的头）。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/health.h"
#include "core/json.h"
#include "core/schedule.h"
#include "core/snapshot.h"
#include "core/utf8.h"
#include "display/frame.h"
#include "display/mailbox.h"
#include "display/taskbar_layout.h"
#include "ipc/binding.h"
#include "ipc/command.h"
#include "ipc/control_channel.h"
#include "ipc/settings.h"
#include "net/http_client.h"
#include "net/url.h"
#include "platform/console.h"
#include "platform/paths.h"
#include "tests/test_support.h"

using namespace deskfeed;

namespace testing {

std::vector<Case>& cases() {
    static std::vector<Case> instance;
    return instance;
}

[[noreturn]] void report_failure(std::string message) { throw Failure{std::move(message)}; }

}  // namespace testing

namespace {

// ---- 小工具 ----------------------------------------------------------------

// 单调时钟，只用于用例里的“不许拖过预算”这类计时断言。
long long tick_ms() { return static_cast<long long>(::GetTickCount64()); }

// 墙钟 Unix 毫秒：协议里的 expiresAtMs 就是这个域。与进程内的绝对时间比较必须用它，
// 不能拿单调时钟凑（两者不能互相换算）。
long long unix_ms() {
    FILETIME file_time{};
    ::GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return static_cast<long long>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

std::string body_with(std::string const& items, std::string const& extra = std::string()) {
    return "{\"version\":1,\"items\":" + items + extra + "}";
}

Snapshot must_accept(std::string const& body) {
    Snapshot snapshot;
    ValidationResult const result = snapshot_validate(body, snapshot);
    if (!result.ok) {
        testing::report_failure("expected accept, got " +
                                std::string(validation_kind_name(result.kind)) + ": " +
                                result.message);
    }
    return snapshot;
}

ValidationKind must_reject(std::string const& body) {
    Snapshot snapshot;
    ValidationResult const result = snapshot_validate(body, snapshot);
    if (result.ok) {
        testing::report_failure("expected reject, got accept");
    }
    return result.kind;
}

std::string one_item(std::string const& id = "cpu", std::string const& label = "CPU",
                     std::string const& value = "54") {
    return "[{\"id\":\"" + id + "\",\"label\":\"" + label + "\",\"value\":\"" + value + "\"}]";
}

std::wstring temp_directory() {
    wchar_t buffer[MAX_PATH]{};
    DWORD const chars = ::GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    std::wstring base = chars > 0 ? std::wstring(buffer, chars) : std::wstring(L".\\");
    if (!base.empty() && base.back() == L'\\') {
        base.pop_back();
    }
    static int counter = 0;
    return base + L"\\deskfeed-tests-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(++counter);
}

}  // namespace

// ---- JSON ------------------------------------------------------------------

TEST(json_basic_values) {
    JsonValue value;
    std::string error;
    CHECK(json_parse("{\"a\":1,\"b\":[true,null,\"x\"]}", value, error));
    CHECK(value.is_object());
    CHECK_EQ(value.members.size(), static_cast<std::size_t>(2));
    CHECK(value.member("a") != nullptr);
    CHECK(value.member("a")->is_number());
    CHECK_EQ(value.member("a")->number_token, std::string("1"));
    CHECK(value.member("missing") == nullptr);
}

TEST(json_rejects_trailing_content) {
    JsonValue value;
    std::string error;
    CHECK(!json_parse("{} {}", value, error));
    CHECK(!json_parse("1 2", value, error));
}

TEST(json_rejects_duplicate_keys) {
    JsonValue value;
    std::string error;
    CHECK(!json_parse("{\"a\":1,\"a\":2}", value, error));
}

TEST(json_depth_limit) {
    std::string deep;
    for (int index = 0; index < 40; ++index) {
        deep += "[";
    }
    for (int index = 0; index < 40; ++index) {
        deep += "]";
    }
    JsonValue value;
    std::string error;
    CHECK(!json_parse(deep, value, error, JsonParseLimits{32}));

    std::string exact;
    for (int index = 0; index < 32; ++index) {
        exact += "[";
    }
    for (int index = 0; index < 32; ++index) {
        exact += "]";
    }
    CHECK(json_parse(exact, value, error, JsonParseLimits{32}));
}

TEST(json_string_escapes_and_surrogates) {
    JsonValue value;
    std::string error;
    CHECK(json_parse("\"\\u0041\\u00e9\\ud83d\\ude00\"", value, error));
    CHECK_EQ(value.text, std::string("A\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(!json_parse("\"\\ud83d\"", value, error));      // 孤立高代理项
    CHECK(!json_parse("\"\\udc00\"", value, error));      // 孤立低代理项
    CHECK(!json_parse("\"a\nb\"", value, error));         // 裸控制字符
}

TEST(json_rejects_invalid_utf8) {
    JsonValue value;
    std::string error;
    std::string const invalid = "\"\xC3\x28\"";
    CHECK(!json_parse(invalid, value, error));
}

TEST(json_exact_integer_semantics) {
    JsonValue value;
    std::string error;
    long long out = 0;

    CHECK(json_parse("1e3", value, error));
    CHECK(json_number_as_integer(value, out));
    CHECK_EQ(out, 1000LL);

    CHECK(json_parse("1000.0", value, error));
    CHECK(json_number_as_integer(value, out));
    CHECK_EQ(out, 1000LL);

    CHECK(json_parse("9.007199254740991e15", value, error));
    CHECK(json_number_as_integer(value, out));
    CHECK_EQ(out, 9007199254740991LL);

    CHECK(json_parse("9007199254740992", value, error));
    CHECK(json_number_as_integer(value, out));
    CHECK_EQ(out, 9007199254740992LL);  // 是整数，超出安全范围由协议层拒绝

    CHECK(json_parse("1e-3", value, error));
    CHECK(!json_number_as_integer(value, out));

    CHECK(json_parse("1.5", value, error));
    CHECK(!json_number_as_integer(value, out));

    CHECK(json_parse("-0", value, error));
    CHECK(json_number_as_integer(value, out));
    CHECK_EQ(out, 0LL);
}

// ---- 快照协议 --------------------------------------------------------------

TEST(snapshot_accepts_minimal) {
    Snapshot const snapshot = must_accept(body_with(one_item()));
    CHECK_EQ(snapshot.version, 1LL);
    CHECK_EQ(snapshot.items.size(), static_cast<std::size_t>(1));
    CHECK_EQ(snapshot.items[0].id, std::string("cpu"));
    CHECK(snapshot.items[0].state == ItemState::Ok);
    CHECK(!snapshot.has_expires_at);
}

TEST(snapshot_accepts_empty_items) {
    Snapshot const snapshot = must_accept(body_with("[]"));
    CHECK(snapshot.items.empty());
}

TEST(snapshot_accepts_bom) {
    Snapshot const snapshot = must_accept("\xEF\xBB\xBF" + body_with(one_item()));
    CHECK_EQ(snapshot.items.size(), static_cast<std::size_t>(1));
}

TEST(snapshot_ignores_unknown_fields) {
    Snapshot const snapshot = must_accept(
        "{\"version\":1,\"extra\":{\"deep\":[1,2,3]},\"items\":[{\"id\":\"a\",\"label\":\"L\","
        "\"value\":\"V\",\"unknown\":true}]}");
    CHECK_EQ(snapshot.items.size(), static_cast<std::size_t>(1));
}

TEST(snapshot_required_fields_and_types) {
    CHECK(must_reject("{\"items\":[]}") == ValidationKind::Version);
    CHECK(must_reject("{\"version\":2,\"items\":[]}") == ValidationKind::Version);
    CHECK(must_reject("{\"version\":\"1\",\"items\":[]}") == ValidationKind::Version);
    CHECK(must_reject("{\"version\":1}") == ValidationKind::Items);
    CHECK(must_reject("{\"version\":1,\"items\":{}}") == ValidationKind::Items);
    CHECK(must_reject("{\"version\":1,\"items\":[1]}") == ValidationKind::ItemShape);
    CHECK(must_reject(body_with("[{\"label\":\"L\",\"value\":\"V\"}]")) ==
          ValidationKind::RequiredField);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"value\":\"V\"}]")) ==
          ValidationKind::RequiredField);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\"}]")) ==
          ValidationKind::RequiredField);
    CHECK(must_reject(body_with("[{\"id\":1,\"label\":\"L\",\"value\":\"V\"}]")) ==
          ValidationKind::FieldType);
}

TEST(snapshot_ids_and_states) {
    CHECK(must_reject(body_with("[{\"id\":\"\",\"label\":\"L\",\"value\":\"V\"}]")) ==
          ValidationKind::EmptyId);
    CHECK(must_reject(body_with(
              "[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\"},"
              "{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\"}]")) == ValidationKind::DuplicateId);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\",\"state\":\"bad\"}]")) ==
          ValidationKind::InvalidState);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\",\"state\":null}]")) ==
          ValidationKind::FieldType);
    Snapshot const snapshot = must_accept(
        body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\",\"state\":\"error\"}]"));
    CHECK(snapshot.items[0].state == ItemState::Error);
}

TEST(snapshot_capacity_limits) {
    CHECK(must_reject(body_with("[{\"id\":\"" + std::string(kMaxIdBytes + 1, 'a') +
                                "\",\"label\":\"L\",\"value\":\"V\"}]")) ==
          ValidationKind::FieldLimit);
    must_accept(body_with("[{\"id\":\"" + std::string(kMaxIdBytes, 'a') +
                          "\",\"label\":\"L\",\"value\":\"V\"}]"));

    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"" +
                                std::string(kMaxLabelBytes + 1, 'l') + "\",\"value\":\"V\"}]")) ==
          ValidationKind::FieldLimit);
    must_accept(body_with("[{\"id\":\"a\",\"label\":\"" + std::string(kMaxLabelBytes, 'l') +
                          "\",\"value\":\"V\"}]"));

    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"" +
                                std::string(kMaxValueBytes + 1, 'v') + "\"}]")) ==
          ValidationKind::FieldLimit);

    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\",\"detail\":\"" +
                                std::string(kMaxDetailBytes + 1, 'd') + "\"}]")) ==
          ValidationKind::FieldLimit);

    std::string items = "[";
    for (std::size_t index = 0; index < kMaxItems + 1; ++index) {
        if (index > 0) {
            items += ",";
        }
        items += "{\"id\":\"i" + std::to_string(index) + "\",\"label\":\"L\",\"value\":\"V\"}";
    }
    items += "]";
    CHECK(must_reject(body_with(items)) == ValidationKind::ItemCount);

    std::string const huge(kMaxBodyBytes + 1, 'x');
    CHECK(must_reject(huge) == ValidationKind::TooLarge);
}

TEST(snapshot_blank_and_single_line) {
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"  \",\"value\":\"\\t \"}]")) ==
          ValidationKind::Blank);
    must_accept(body_with("[{\"id\":\"a\",\"label\":\"\",\"value\":\"V\"}]"));
    must_accept(body_with("[{\"id\":\"a\",\"label\":\"\\u3000\",\"value\":\"V\"}]"));
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"\\u3000\",\"value\":\"\\u00a0\"}]")) ==
          ValidationKind::Blank);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"a\\nb\",\"value\":\"V\"}]")) ==
          ValidationKind::SingleLine);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"a\\u2028b\"}]")) ==
          ValidationKind::SingleLine);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"a\\u0085b\"}]")) ==
          ValidationKind::SingleLine);
    // 垂直制表（U+000B）与换页（U+000C）同样是硬换行。
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"a\\u000bb\",\"value\":\"V\"}]")) ==
          ValidationKind::SingleLine);
    CHECK(must_reject(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"a\\u000cb\"}]")) ==
          ValidationKind::SingleLine);
    // 制表符不算换行。
    must_accept(body_with("[{\"id\":\"a\",\"label\":\"L\\t\",\"value\":\"V\"}]"));
}

TEST(utf8_line_break_predicate_covers_unicode_hard_breaks) {
    // 谓词本身逐项钉住：CR / LF / VT / FF / NEL / LS / PS 是硬换行。
    CHECK(has_line_break("\r"));
    CHECK(has_line_break("\n"));
    CHECK(has_line_break("\v"));
    CHECK(has_line_break("\f"));
    CHECK(has_line_break("a\xC2\x85z"));      // U+0085 NEL
    CHECK(has_line_break("a\xE2\x80\xA8z"));  // U+2028 LINE SEPARATOR
    CHECK(has_line_break("a\xE2\x80\xA9z"));  // U+2029 PARAGRAPH SEPARATOR
    // 制表符、空格、空串都不是换行。
    CHECK(!has_line_break("\t"));
    CHECK(!has_line_break("plain text"));
    CHECK(!has_line_break(""));
    // 不是合法 UTF-8 时退回 ASCII 兜底：明显的 CR / LF 仍判为换行，其余不误判。
    CHECK(has_line_break("a\xFF\r" "b"));
    CHECK(!has_line_break("a\xFF" "b"));
}

TEST(snapshot_expiry_field) {
    Snapshot const snapshot = must_accept(body_with(one_item(), ",\"expiresAtMs\":1000"));
    CHECK(snapshot.has_expires_at);
    CHECK_EQ(snapshot.expires_at_ms, 1000LL);

    must_accept(body_with(one_item(), ",\"expiresAtMs\":1000.0"));
    must_accept(body_with(one_item(), ",\"expiresAtMs\":1e3"));
    must_accept(body_with(one_item(), ",\"expiresAtMs\":9007199254740991.0"));

    CHECK(must_reject(body_with(one_item(), ",\"expiresAtMs\":1e-3")) == ValidationKind::Expiry);
    CHECK(must_reject(body_with(one_item(), ",\"expiresAtMs\":1.5")) == ValidationKind::Expiry);
    CHECK(must_reject(body_with(one_item(), ",\"expiresAtMs\":9007199254740992")) ==
          ValidationKind::Expiry);
    CHECK(must_reject(body_with(one_item(), ",\"expiresAtMs\":-1")) == ValidationKind::Expiry);
    CHECK(must_reject(body_with(one_item(), ",\"expiresAtMs\":null")) == ValidationKind::Expiry);
}

// ---- 健康 / 到期状态机 ------------------------------------------------------

TEST(health_waiting_then_error) {
    HealthState health;
    health.set_configured(true);
    CHECK(health.view().kind == ViewKind::Waiting);

    health.apply_failure(FailureKind::Transport, "连不上", 1000);
    View const view = health.view();
    CHECK(view.kind == ViewKind::Error);
    CHECK(view.alarm);
    CHECK_EQ(view.reason, std::string("连不上"));
}

TEST(health_no_url_is_error) {
    HealthState health;
    View const view = health.view();
    CHECK(view.kind == ViewKind::Error);
    CHECK(view.failure == FailureKind::NoUrl);
}

TEST(health_failure_keeps_values_and_colours_them) {
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body_with(one_item())), 1000);
    CHECK(!health.view().alarm);

    health.apply_failure(FailureKind::Timeout, "超时", 2000);
    View const view = health.view();
    CHECK(view.kind == ViewKind::Content);
    CHECK(view.alarm);
    CHECK_EQ(view.items.size(), static_cast<std::size_t>(1));
    CHECK_EQ(view.items[0].value, std::string("54"));
}

TEST(health_recovery_restores_per_item_state) {
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\","
                                                "\"state\":\"warn\"}]")),
                          1000);
    health.apply_failure(FailureKind::Timeout, "超时", 2000);
    CHECK(health.view().alarm);

    health.apply_snapshot(must_accept(body_with("[{\"id\":\"a\",\"label\":\"L\",\"value\":\"V\","
                                                "\"state\":\"warn\"}]")),
                          3000);
    View const view = health.view();
    CHECK(!view.alarm);
    CHECK(view.items[0].state == ItemState::Warn);
}

TEST(health_empty_items_clear_then_failure_is_error) {
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body_with(one_item())), 1000);
    health.apply_snapshot(must_accept(body_with("[]")), 2000);
    View const cleared = health.view();
    CHECK(cleared.kind == ViewKind::Content);
    CHECK(cleared.items.empty());
    CHECK(!cleared.alarm);

    health.apply_failure(FailureKind::Transport, "连不上", 3000);
    View const failed = health.view();
    CHECK(failed.kind == ViewKind::Error);
    CHECK(failed.items.empty());
}

TEST(health_expiry_is_independent_and_not_renewed) {
    std::string const body = body_with(one_item(), ",\"expiresAtMs\":5000");
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body), 1000);
    CHECK(!health.expired());
    CHECK_EQ(health.next_expiry_ms(), 5000LL);

    health.tick(5000);
    CHECK(health.expired());
    View const expired = health.view();
    CHECK(expired.kind == ViewKind::Content);
    CHECK(expired.alarm);
    CHECK_EQ(expired.reason, std::string("快照已过期"));

    // 同一份旧快照再以 200 返回，不延长到期时间。
    health.apply_snapshot(must_accept(body), 6000);
    CHECK(health.expired());
    CHECK(health.view().alarm);
}

TEST(health_first_snapshot_already_expired_is_kept_red) {
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body_with(one_item(), ",\"expiresAtMs\":500")), 1000);
    View const view = health.view();
    CHECK(view.kind == ViewKind::Content);
    CHECK(view.alarm);
    CHECK_EQ(view.items.size(), static_cast<std::size_t>(1));
    CHECK(health.expired());
}

TEST(health_no_expiry_never_infers_age) {
    HealthState health;
    health.set_configured(true);
    health.apply_snapshot(must_accept(body_with(one_item())), 1000);
    health.tick(24LL * 60 * 60 * 1000);
    CHECK(!health.expired());
    CHECK(!health.view().alarm);
}

// ---- 调度 ------------------------------------------------------------------

TEST(schedule_defaults_and_clamps) {
    CHECK_EQ(schedule_clamp_poll(0).value, 30000LL);
    CHECK(schedule_clamp_poll(0).used_default);
    CHECK_EQ(schedule_clamp_poll(500).value, 1000LL);
    CHECK(schedule_clamp_poll(500).clamped);
    CHECK_EQ(schedule_clamp_poll(999999999999LL).value, schedule_bounds().poll_max);
    CHECK_EQ(schedule_clamp_retry(-5).value, 5000LL);
    CHECK_EQ(schedule_clamp_timeout(100).value, 1000LL);
    CHECK_EQ(schedule_clamp_timeout(100000).value, schedule_bounds().timeout_max);

    FetchScheduler scheduler;
    ScheduleConfig config;
    scheduler.set_config(config);
    scheduler.reset(1000);
    CHECK_EQ(scheduler.due_ms(), 1000LL);
    scheduler.on_result(true, 1000);
    CHECK_EQ(scheduler.due_ms(), 31000LL);
    scheduler.on_result(false, 1000);
    CHECK_EQ(scheduler.due_ms(), 6000LL);
}

// ---- 命令行 ----------------------------------------------------------------

TEST(command_basic_forms) {
    Command command;
    std::wstring error;

    CHECK(command_parse({}, command, error));
    CHECK(command.kind == CommandKind::Start);

    CHECK(command_parse({L"--url", L"http://127.0.0.1/s"}, command, error));
    CHECK(command.kind == CommandKind::Apply);
    CHECK_EQ(command.url, std::wstring(L"http://127.0.0.1/s"));
    CHECK(!command.has_pid);

    CHECK(command_parse({L"--url", L"http://x/", L"--pid", L"1234"}, command, error));
    CHECK(command.has_pid);
    CHECK_EQ(command.pid, 1234UL);

    CHECK(command_parse({L"--quit-if-bound", L"99"}, command, error));
    CHECK(command.kind == CommandKind::QuitIfBound);
    CHECK_EQ(command.pid, 99UL);
    CHECK_EQ(command.wait_exit_ms, 0UL);  // 缺省：只等 IPC 确认

    CHECK(command_parse({L"--quit-if-bound", L"99", L"--wait-exit-ms", L"60000"}, command, error));
    CHECK_EQ(command.wait_exit_ms, 60000UL);
    CHECK(command_parse({L"--quit", L"--wait-exit-ms", L"1"}, command, error));
    CHECK_EQ(command.wait_exit_ms, 1UL);
}

TEST(command_rejects_bad_forms) {
    Command command;
    std::wstring error;
    CHECK(!command_parse({L"--pid", L"123"}, command, error));
    CHECK(!command_parse({L"--url", L""}, command, error));
    CHECK(!command_parse({L"--scope", L""}, command, error));
    CHECK(!command_parse({L"--url", L"http://x", L"--quit"}, command, error));
    CHECK(!command_parse({L"--nope"}, command, error));
    CHECK(!command_parse({L"--pid", L"abc", L"--url", L"http://x"}, command, error));
    // --wait-exit-ms 只对退出类命令有效，且范围是 1..60000。
    CHECK(!command_parse({L"--url", L"http://x", L"--wait-exit-ms", L"100"}, command, error));
    CHECK(!command_parse({L"--quit", L"--wait-exit-ms", L"0"}, command, error));
    CHECK(!command_parse({L"--quit", L"--wait-exit-ms", L"60001"}, command, error));
    CHECK(!command_parse({L"--quit", L"--wait-exit-ms"}, command, error));
    CHECK(!command_parse({L"--quit-if-bound", L"9", L"--wait-exit-ms", L"abc"}, command, error));
}

TEST(command_isolation_flags) {
    Command command;
    std::wstring error;
    CHECK(command_parse({L"--headless", L"--scope", L"acc1"}, command, error));
    CHECK(command.headless);
    CHECK(command.no_taskbar);
    CHECK(command.hidden);
    CHECK_EQ(command.scope, std::wstring(L"acc1"));

    CHECK(command_parse({L"--no-inject"}, command, error));
    CHECK(command.no_taskbar);
    CHECK(!command.hidden);
}

TEST(scope_validation) {
    std::wstring error;
    CHECK(scope_validate(L"", error));
    CHECK(scope_validate(L"acc-1_x", error));
    CHECK(!scope_validate(L"../evil", error));
    CHECK(!scope_validate(L"a\\b", error));
    CHECK(!scope_validate(std::wstring(33, L'a'), error));
    CHECK(!scope_validate(std::wstring(200, L'a'), error));
}

TEST(scope_env_oversized_is_rejected_not_silently_production) {
    // 超长/非法的环境变量实例名必须原样读出来并被拒绝；
    // 若被悄悄当成空串，测试就会落到生产命名空间上。
    ::SetEnvironmentVariableW(L"DESKFEED_SCOPE", std::wstring(200, L'a').c_str());
    std::wstring const oversized = instance_suffix();
    std::wstring error;
    CHECK_EQ(oversized.size(), static_cast<std::size_t>(200));
    CHECK(!scope_validate(oversized, error));

    ::SetEnvironmentVariableW(L"DESKFEED_SCOPE", L"bad/name");
    CHECK(!scope_validate(instance_suffix(), error));

    ::SetEnvironmentVariableW(L"DESKFEED_SCOPE", L"ok-1");
    CHECK(scope_validate(instance_suffix(), error));

    ::SetEnvironmentVariableW(L"DESKFEED_SCOPE", nullptr);
    CHECK_EQ(instance_suffix(), std::wstring());
}

// ---- 控制通道线格式 --------------------------------------------------------

TEST(control_roundtrip) {
    ControlRequest request;
    request.op = ControlOp::Command;
    request.sender_pid = 4321;
    request.args = {L"--url", L"http://127.0.0.1:8099/snapshot", L"--pid", L"77"};

    ControlRequest decoded;
    std::string error;
    CHECK(control_decode(control_encode(request), decoded, error));
    CHECK(decoded.op == ControlOp::Command);
    CHECK_EQ(decoded.sender_pid, 4321U);
    CHECK_EQ(decoded.args.size(), static_cast<std::size_t>(4));
    CHECK_EQ(decoded.args[2], std::wstring(L"--pid"));

    ControlRequest screens;
    screens.op = ControlOp::SetScreens;
    screens.sender_pid = 77;
    screens.args = {L"\\\\?\\DISPLAY#A#1", L"\\\\?\\DISPLAY#B#2"};
    std::string screens_error;
    CHECK(control_decode(control_encode(screens), decoded, screens_error));
    CHECK(decoded.op == ControlOp::SetScreens);
    CHECK_EQ(decoded.args.size(), static_cast<std::size_t>(2));
    CHECK_EQ(decoded.args[1], std::wstring(L"\\\\?\\DISPLAY#B#2"));
}

TEST(control_rejects_corrupt_payloads) {
    ControlRequest decoded;
    std::string error;
    CHECK(!control_decode("", decoded, error));
    CHECK(!control_decode("short", decoded, error));

    std::string bytes = control_encode(ControlRequest{ControlOp::Command, 1, {L"--quit"}});
    bytes[0] = 0x00;  // 破坏 magic
    CHECK(!control_decode(bytes, decoded, error));

    bytes = control_encode(ControlRequest{ControlOp::Command, 1, {L"--quit"}});
    bytes.push_back('x');  // 多余字节
    CHECK(!control_decode(bytes, decoded, error));
}

// ---- 地址 ------------------------------------------------------------------

TEST(url_crack_keeps_query_and_drops_fragment) {
    UrlParts parts;
    std::wstring error;
    CHECK(url_crack(L"http://127.0.0.1:8099/snapshot?case=a&x=1#frag", parts, error));
    CHECK_EQ(parts.scheme, std::wstring(L"http"));
    CHECK_EQ(parts.host, std::wstring(L"127.0.0.1"));
    CHECK_EQ(parts.port, static_cast<unsigned short>(8099));
    CHECK_EQ(parts.path, std::wstring(L"/snapshot?case=a&x=1"));
    CHECK(!parts.secure);

    CHECK(url_crack(L"https://example.com", parts, error));
    CHECK_EQ(parts.path, std::wstring(L"/"));
    CHECK(parts.secure);
}

TEST(url_crack_rejects_whitespace_and_bad_scheme) {
    UrlParts parts;
    std::wstring error;
    CHECK(!url_crack(L" http://x/", parts, error));
    CHECK(!url_crack(L"http://x/ ", parts, error));
    CHECK(!url_crack(L"ftp://x/", parts, error));
    CHECK(!url_crack(L"", parts, error));
    CHECK(!url_crack(L"http:///nohost", parts, error));
}

TEST(url_redact_removes_secrets) {
    bool removed = false;
    CHECK_EQ(url_redact(L"http://user:pass@host:80/path?token=abc#frag", &removed),
             std::wstring(L"http://***@host:80/path"));
    CHECK(removed);
    CHECK_EQ(url_redact(L"http://host/path", &removed), std::wstring(L"http://host/path"));
    CHECK(!removed);
    CHECK_EQ(url_redact(L"not a url", &removed), std::wstring(L"<unsupported-url>"));
    CHECK(removed);
}

TEST(url_resolve_relative_and_dot_segments) {
    std::wstring out;
    std::wstring error;
    CHECK(url_resolve(L"http://h/a/b/c", L"d", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/b/d"));
    CHECK(url_resolve(L"http://h/a/b/c?x=1", L"../d?y=2", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/d?y=2"));
    CHECK(url_resolve(L"http://h/a/", L"/z", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/z"));
    CHECK(url_resolve(L"http://h/a", L"https://other/x", out, error));
    CHECK_EQ(out, std::wstring(L"https://other/x"));
    CHECK(url_resolve(L"http://h/a/b", L"?q=1", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/b?q=1"));

    // 中间出现 "://" 的相对引用不是绝对地址。
    CHECK(url_resolve(L"http://h/a/b", L"?next=https://example.com", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/b?next=https://example.com"));
    CHECK(url_resolve(L"http://h/a/b", L"/next?target=http://x", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/next?target=http://x"));
    CHECK(url_resolve(L"http://h/a/b", L"c/d:e", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/c/d:e"));

    // 点段合并：重复斜杠产生的空段会被 ".." 吃掉。
    CHECK(url_resolve(L"http://h/a//../b", L"", out, error) == false);  // 空引用被拒绝
    CHECK(url_resolve(L"http://h/a//x", L"../b", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/a/b"));
    CHECK(url_resolve(L"http://h/", L"..", out, error));
    CHECK_EQ(out, std::wstring(L"http://h/"));
}

// ---- 设置 ------------------------------------------------------------------

TEST(settings_roundtrip_preserves_other_lines) {
    std::wstring const directory = temp_directory();
    std::wstring const path = directory + L"\\settings.ini";
    std::wstring error;

    // 先写一份带注释、其它栏目与未知键的文件。
    std::string const original =
        "; user settings\r\n[source]\r\nurl=http://old/snapshot\r\npoll_ms=1000\r\n"
        "[window]\r\nright=2560\r\nbottom=1392\r\nhidden=true\r\n[taskbar]\r\ncomponent=true\r\n";
    CHECK(write_file_bytes(path, original, error));

    SettingsFile settings(path);
    CHECK(settings.load(error));
    SourceSettings const source = settings.read_source();
    CHECK_EQ(source.url, std::wstring(L"http://old/snapshot"));
    CHECK_EQ(source.poll_ms, 1000LL);

    CHECK(settings.write_source_url(L"http://new/snapshot", nullptr, error));
    std::string written;
    bool exists = false;
    CHECK(read_file_bytes(path, written, exists, error));
    CHECK(exists);
    CHECK(written.find("url=http://new/snapshot") != std::string::npos);
    CHECK(written.find("right=2560") != std::string::npos);
    CHECK(written.find("bottom=1392") != std::string::npos);
    CHECK(written.find("component=true") != std::string::npos);
    CHECK(written.find("; user settings") != std::string::npos);
    CHECK(written.find("poll_ms=1000") != std::string::npos);
    CHECK(written.find("pid") == std::string::npos);  // PID 绝不落盘

    // 值按原样保存：地址里的查询串不会被改写。
    CHECK(settings.write_source_url(L"http://new/snapshot?token=a%20b", nullptr, error));
    CHECK(read_file_bytes(path, written, exists, error));
    CHECK(written.find("url=http://new/snapshot?token=a%20b") != std::string::npos);

    delete_file(path);
    ::RemoveDirectoryW(directory.c_str());
}

TEST(settings_missing_file_is_empty) {
    std::wstring const directory = temp_directory();
    std::wstring const path = directory + L"\\settings.ini";
    std::wstring error;
    SettingsFile settings(path);
    CHECK(settings.load(error));
    CHECK(settings.read_source().url.empty());
    CHECK_EQ(settings.read_source().poll_ms, 0LL);
}

// ---- 进程绑定 --------------------------------------------------------------

TEST(binding_rejects_exited_process) {
    // 起一个立刻退出的进程，等它结束后再准备绑定：必须被拒绝。
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring const command = L"cmd.exe /c exit 0";
    CHECK(::CreateProcessW(nullptr, const_cast<wchar_t*>(command.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE);
    ::WaitForSingleObject(process.hProcess, 10000);

    ProcessBinding::Prepared prepared;
    std::string error;
    CHECK(!ProcessBinding::prepare(process.hProcess, process.dwProcessId, 1, nullptr, 0, prepared,
                                   error));
    ::CloseHandle(process.hProcess);
    ::CloseHandle(process.hThread);
}

TEST(binding_commit_and_generations) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring const command = L"cmd.exe /c ping -n 6 127.0.0.1 > nul";
    CHECK(::CreateProcessW(nullptr, const_cast<wchar_t*>(command.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE);

    ProcessBinding binding;
    ProcessBinding::Prepared prepared;
    std::string error;
    CHECK(ProcessBinding::prepare(process.hProcess, process.dwProcessId, 7, nullptr, 0, prepared,
                                  error));
    binding.commit(prepared);
    CHECK(binding.active());
    CHECK_EQ(binding.pid(), process.dwProcessId);
    CHECK_EQ(binding.generation(), static_cast<std::uint64_t>(7));
    CHECK(!binding.signaled());  // 进程还在跑

    // 绑定期内句柄归 binding 所有，用完之后才 clear；不要先 clear 再拿这个句柄值去用，
    // 那是在用一个已经被关掉的句柄。
    ::TerminateProcess(process.hProcess, 0);
    ::WaitForSingleObject(process.hProcess, 5000);
    CHECK(binding.signaled());
    binding.clear();
    CHECK(!binding.active());
    ::CloseHandle(process.hThread);
}

TEST(binding_signaled_reflects_the_actual_bound_handle) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring const command = L"cmd.exe /c ping -n 8 127.0.0.1 > nul";
    CHECK(::CreateProcessW(nullptr, const_cast<wchar_t*>(command.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE);

    ProcessBinding binding;
    CHECK(!binding.signaled());  // 没有绑定时永远是 false

    ProcessBinding::Prepared prepared;
    std::string error;
    CHECK(ProcessBinding::prepare(process.hProcess, process.dwProcessId, 31, nullptr, 0, prepared,
                                  error));
    binding.commit(prepared);
    CHECK(binding.active());
    CHECK(!binding.signaled());

    ::TerminateProcess(process.hProcess, 0);
    ::WaitForSingleObject(process.hProcess, 5000);
    CHECK(binding.signaled());
    binding.clear();
    ::CloseHandle(process.hThread);
}

namespace {

LRESULT CALLBACK count_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return ::DefWindowProcW(window, message, wparam, lparam);
}

// 一个只用来收消息的隐藏窗口（消息专用窗口，不出现在屏幕上）。
HWND create_message_window() {
    static bool registered = false;
    wchar_t const* const class_name = L"DeskFeedTestMessageWindow";
    if (!registered) {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &count_window_proc;
        description.hInstance = ::GetModuleHandleW(nullptr);
        description.lpszClassName = class_name;
        if (::RegisterClassExW(&description) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        registered = true;
    }
    return ::CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// 在窗口的消息队列里找一条指定消息，最多等 wait_ms 毫秒。
bool take_posted_message(HWND window, UINT wanted, WPARAM& wparam, unsigned wait_ms) {
    long long const deadline = tick_ms() + wait_ms;
    for (;;) {
        MSG message{};
        while (::PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            if (message.message == wanted) {
                wparam = message.wParam;
                return true;
            }
        }
        if (tick_ms() >= deadline) {
            return false;
        }
        ::Sleep(10);
    }
}

}  // namespace

TEST(binding_failed_attempt_cannot_quit_a_later_binding) {
    // 复现“失败的绑定尝试留下迟到的退出消息”这条竞态：
    //   prepare(代次 21) 会立刻起等待者，进程一退出就把代次 21 投递到消息队列；
    //   随后落盘失败 → discard（收不回已经排队的窗口消息）；
    //   再有一次成功的绑定（代次 22，绑定一个**还在跑**的进程）。
    // 判定条件必须同时看代次与“当前绑定的句柄是否真的触发”，否则代次 21 的旧消息会把
    // 新实例退掉。这里把主进程的判断条件原样编码进用例。
    HWND const window = create_message_window();
    CHECK(window != nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    // A：立刻退出，代次 21 的等待者会投递一条消息。
    PROCESS_INFORMATION a{};
    std::wstring const command_a = L"cmd.exe /c exit 0";
    CHECK(::CreateProcessW(nullptr, const_cast<wchar_t*>(command_a.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &a) != FALSE);
    ProcessBinding::Prepared stale;
    std::string error;
    CHECK(ProcessBinding::prepare(a.hProcess, a.dwProcessId, 21, window, WM_APP + 9, stale, error));
    ::WaitForSingleObject(a.hProcess, 5000);
    WPARAM stale_token = 0;
    CHECK(take_posted_message(window, WM_APP + 9, stale_token, 3000));
    CHECK_EQ(stale_token, static_cast<WPARAM>(21));
    ProcessBinding::discard(stale);  // 模拟落盘失败：不提交
    ::CloseHandle(a.hThread);

    // B：还在跑，用代次 22 成功绑定。
    PROCESS_INFORMATION b{};
    std::wstring const command_b = L"cmd.exe /c ping -n 8 127.0.0.1 > nul";
    CHECK(::CreateProcessW(nullptr, const_cast<wchar_t*>(command_b.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &b) != FALSE);
    ProcessBinding binding;
    ProcessBinding::Prepared live;
    CHECK(ProcessBinding::prepare(b.hProcess, b.dwProcessId, 22, window, WM_APP + 9, live, error));
    binding.commit(live);

    // 代次不复用：新绑定拿到的是另一个值。
    CHECK_EQ(binding.generation(), static_cast<std::uint64_t>(22));
    CHECK(stale_token != static_cast<WPARAM>(binding.generation()));

    // 主进程的判断条件：代次不符，或者当前绑定的进程还活着 —— 两条都成立，旧消息必须被忽略。
    bool const stale_would_quit =
        stale_token == static_cast<WPARAM>(binding.generation()) && binding.signaled();
    CHECK(!stale_would_quit);
    CHECK(!binding.signaled());

    ::TerminateProcess(b.hProcess, 0);
    ::WaitForSingleObject(b.hProcess, 5000);
    CHECK(binding.signaled());
    binding.clear();
    ::CloseHandle(b.hThread);
    ::DestroyWindow(window);
}

// ---- 显示帧与显示邮箱（Phase 2 的 seam）-------------------------------------

namespace {

void put_u32_le(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFFu));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFFu));
}

Frame sample_frame() {
    Frame frame;
    frame.kind = ViewKind::Content;
    frame.alarm = true;
    frame.failure = FailureKind::Timeout;
    frame.reason = "超过 8000 ms 整次取数预算";
    FrameItem first;
    first.id = "cpu";
    first.label = "CPU";
    first.value = "54℃";
    first.state = ItemState::Warn;
    first.detail = "第一行\n第二行";
    frame.items.push_back(first);
    FrameItem second;
    second.id = "disk";
    second.label = "磁盘";
    second.value = "剩余 68%";
    second.state = ItemState::Error;
    frame.items.push_back(second);
    return frame;
}

}  // namespace

TEST(taskbar_screen_list_parse_and_format) {
    bool malformed = true;
    std::vector<std::wstring> ids =
        parse_taskbar_screen_list(L"  \\\\?\\DISPLAY#A#1  ;\\\\?\\DISPLAY#B#2; ;", &malformed);
    CHECK(!malformed);
    CHECK_EQ(ids.size(), static_cast<std::size_t>(2));
    CHECK_EQ(ids[0], std::wstring(L"\\\\?\\DISPLAY#A#1"));
    CHECK_EQ(ids[1], std::wstring(L"\\\\?\\DISPLAY#B#2"));

    // 大小写不敏感去重，保留第一次出现的那一份。
    ids = parse_taskbar_screen_list(L"\\\\?\\DISPLAY#A#1;\\\\?\\DISPLAY#a#1");
    CHECK_EQ(ids.size(), static_cast<std::size_t>(1));
    CHECK_EQ(ids[0], std::wstring(L"\\\\?\\DISPLAY#A#1"));

    // 空文本 = 显式空表（不在任何任务栏显示），不是写坏。
    malformed = true;
    ids = parse_taskbar_screen_list(L"   ", &malformed);
    CHECK(ids.empty());
    CHECK(!malformed);

    // 非法段被丢弃并标 malformed；合法的照常保留。
    malformed = false;
    ids = parse_taskbar_screen_list(L"good;bad\nvalue;another", &malformed);
    CHECK(malformed);
    CHECK_EQ(ids.size(), static_cast<std::size_t>(2));

    std::wstring text;
    CHECK(format_taskbar_screen_list({L"a", L"b"}, text));
    CHECK_EQ(text, std::wstring(L"a;b"));
    CHECK(format_taskbar_screen_list({}, text));
    CHECK(text.empty());
    CHECK(!format_taskbar_screen_list({L"a", L"A"}, text));  // 大小写重复
    CHECK(!format_taskbar_screen_list({L"a;b"}, text));     // 分隔符
    CHECK(!format_taskbar_screen_list({std::wstring(kMaxTaskbarScreenIdChars + 1, L'x')}, text));

    CHECK(taskbar_screen_id_valid(L"\\\\?\\DISPLAY#A#1"));
    CHECK(!taskbar_screen_id_valid(L""));
    CHECK(!taskbar_screen_id_valid(L"a;b"));
    CHECK(!taskbar_screen_id_valid(L"a\nb"));
}

TEST(taskbar_screen_selection_defaults_and_degradation) {
    // 没有配置：只在主屏显示（向后兼容）。
    TaskbarScreenSelection selected = select_taskbar_screens(
        TaskbarScreenSelectionInput{false, {}, L"primary", {L"primary", L"secondary"}});
    CHECK_EQ(selected.selected.size(), static_cast<std::size_t>(1));
    CHECK_EQ(selected.selected[0], std::wstring(L"primary"));
    CHECK(!selected.degraded);
    CHECK(selected.requested_any);

    // 明确配置为空：不在任何任务栏显示；配置本身没有要求，所以绝不退化到主屏。
    selected = select_taskbar_screens(
        TaskbarScreenSelectionInput{true, {}, L"primary", {L"primary", L"secondary"}});
    CHECK(selected.selected.empty());
    CHECK(!selected.degraded);
    CHECK(!selected.requested_any);

    // 只配置了一块已经不可用的副屏：退化到主屏，配置本身仍非空。
    selected = select_taskbar_screens(
        TaskbarScreenSelectionInput{true, {L"gone"}, L"primary", {L"primary"}});
    CHECK_EQ(selected.selected.size(), static_cast<std::size_t>(1));
    CHECK_EQ(selected.selected[0], std::wstring(L"primary"));
    CHECK(selected.degraded);
    CHECK(selected.requested_any);

    // 配置了多块，只有部分不可用：可用的那一块照常显示，不退化。
    selected = select_taskbar_screens(TaskbarScreenSelectionInput{
        true, {L"primary", L"gone"}, L"primary", {L"primary"}});
    CHECK_EQ(selected.selected.size(), static_cast<std::size_t>(1));
    CHECK_EQ(selected.selected[0], std::wstring(L"primary"));
    CHECK(!selected.degraded);

    // 配置了两块副屏，一块可用：只显示可用的那块，不把主屏加进来。
    selected = select_taskbar_screens(TaskbarScreenSelectionInput{
        true, {L"secondary-a", L"secondary-b"}, L"primary",
        {L"primary", L"secondary-a"}});
    CHECK_EQ(selected.selected.size(), static_cast<std::size_t>(1));
    CHECK_EQ(selected.selected[0], std::wstring(L"secondary-a"));
    CHECK(!selected.degraded);

    // 配置的主屏自己也不可用：没有可退化的目标，selected 为空。
    selected = select_taskbar_screens(
        TaskbarScreenSelectionInput{true, {L"gone"}, L"primary", {L"other"}});
    CHECK(selected.selected.empty());
    CHECK(!selected.degraded);

    // 多块可用时按 available 的顺序输出（物理顺序），不是配置顺序。
    selected = select_taskbar_screens(TaskbarScreenSelectionInput{
        true, {L"b", L"a"}, L"primary", {L"a", L"b", L"c"}});
    CHECK_EQ(selected.selected.size(), static_cast<std::size_t>(2));
    CHECK_EQ(selected.selected[0], std::wstring(L"a"));
    CHECK_EQ(selected.selected[1], std::wstring(L"b"));
    CHECK(!selected.degraded);
}

TEST(frame_roundtrip_preserves_every_field) {
    Frame frame = sample_frame();
    frame.screens_configured = true;
    frame.screens = {"\\\\?\\DISPLAY#AAA#1#{guid}", "\\\\?\\DISPLAY#BBB#2#{guid}"};
    std::vector<unsigned char> payload;
    CHECK(frame_encode(frame, payload));
    CHECK(payload.size() <= kFramePayloadCapacity);

    Frame decoded;
    CHECK(frame_decode(payload.data(), payload.size(), decoded));
    CHECK(decoded.kind == ViewKind::Content);
    CHECK(decoded.alarm);
    CHECK(decoded.failure == FailureKind::Timeout);
    CHECK_EQ(decoded.reason, frame.reason);
    CHECK(decoded.screens_configured);
    CHECK_EQ(decoded.screens.size(), static_cast<std::size_t>(2));
    CHECK_EQ(decoded.screens[0], frame.screens[0]);
    CHECK_EQ(decoded.screens[1], frame.screens[1]);
    CHECK_EQ(decoded.items.size(), static_cast<std::size_t>(2));
    CHECK_EQ(decoded.items[0].id, std::string("cpu"));
    CHECK_EQ(decoded.items[0].label, std::string("CPU"));
    CHECK_EQ(decoded.items[0].value, std::string("54℃"));
    CHECK(decoded.items[0].state == ItemState::Warn);
    CHECK_EQ(decoded.items[0].detail, std::string("第一行\n第二行"));
    CHECK_EQ(decoded.items[1].id, std::string("disk"));
    CHECK(decoded.items[1].state == ItemState::Error);
    CHECK_EQ(decoded.items[1].detail, std::string());
}

TEST(frame_decode_rejects_inconsistent_payloads) {
    Frame const frame = sample_frame();
    std::vector<unsigned char> payload;
    CHECK(frame_encode(frame, payload));

    Frame out;
    CHECK(!frame_decode(nullptr, 0, out));
    CHECK(!frame_decode(payload.data(), 0, out));
    CHECK(!frame_decode(payload.data(), payload.size() - 1, out));  // 截断

    std::vector<unsigned char> padded = payload;
    padded.push_back(0);
    CHECK(!frame_decode(padded.data(), padded.size(), out));  // 尾巴多出字节

    // 条目数谎报：头里写 65，实际没有内容。
    std::vector<unsigned char> lying;
    put_u32_le(lying, static_cast<std::uint32_t>(kMaxItems + 1));
    put_u32_le(lying, 1);
    put_u32_le(lying, 0);
    put_u32_le(lying, 0);
    put_u32_le(lying, 0);
    put_u32_le(lying, 0);
    put_u32_le(lying, 0);
    CHECK(!frame_decode(lying.data(), lying.size(), out));

    // 失败原因长度超过上限。
    std::vector<unsigned char> long_reason;
    put_u32_le(long_reason, 0);
    put_u32_le(long_reason, 1);
    put_u32_le(long_reason, 0);
    put_u32_le(long_reason, 0);
    put_u32_le(long_reason, static_cast<std::uint32_t>(kFrameMaxReasonBytes + 1));
    long_reason.push_back('x');
    CHECK(!frame_decode(long_reason.data(), long_reason.size(), out));

    // id 长度超过协议上限。
    std::vector<unsigned char> long_id;
    put_u32_le(long_id, 1);
    put_u32_le(long_id, 1);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, static_cast<std::uint32_t>(kMaxIdBytes + 1));
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    put_u32_le(long_id, 0);
    CHECK(!frame_decode(long_id.data(), long_id.size(), out));

    // 枚举越界。
    std::vector<unsigned char> bad_kind;
    put_u32_le(bad_kind, 0);
    put_u32_le(bad_kind, 99);
    put_u32_le(bad_kind, 0);
    put_u32_le(bad_kind, 0);
    put_u32_le(bad_kind, 0);
    put_u32_le(bad_kind, 0);
    put_u32_le(bad_kind, 0);
    CHECK(!frame_decode(bad_kind.data(), bad_kind.size(), out));

    // 屏幕配置：configured 只允许 0/1；未配置时不许夹带屏幕；数量与标识都要有界。
    std::vector<unsigned char> bad_screens;
    put_u32_le(bad_screens, 0);  // items
    put_u32_le(bad_screens, 1);  // kind
    put_u32_le(bad_screens, 0);  // alarm
    put_u32_le(bad_screens, 0);  // failure
    put_u32_le(bad_screens, 0);  // reason bytes
    put_u32_le(bad_screens, 2);  // configured: only 0/1
    put_u32_le(bad_screens, 0);
    CHECK(!frame_decode(bad_screens.data(), bad_screens.size(), out));

    bad_screens.clear();
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);  // configured=false
    put_u32_le(bad_screens, 1);  // but one screen
    put_u32_le(bad_screens, 0);
    CHECK(!frame_decode(bad_screens.data(), bad_screens.size(), out));

    bad_screens.clear();
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, static_cast<std::uint32_t>(kMaxTaskbarScreens + 1));
    CHECK(!frame_decode(bad_screens.data(), bad_screens.size(), out));

    bad_screens.clear();
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 0);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, 1);
    put_u32_le(bad_screens, 1);
    bad_screens.push_back(';');  // 配置分隔符不能出现在标识里
    CHECK(!frame_decode(bad_screens.data(), bad_screens.size(), out));

    // 空载荷是合法的一帧（waiting、没有条目）。
    std::vector<unsigned char> empty_frame;
    CHECK(frame_encode(Frame{}, empty_frame));
    CHECK(frame_decode(empty_frame.data(), empty_frame.size(), out));
    CHECK(out.kind == ViewKind::Waiting);
    CHECK(out.items.empty());
}

TEST(display_mailbox_reports_missing_and_foreign_producers) {
    DisplayFrameConsumer consumer;
    std::string error;
    CHECK(!consumer.attach(L"mbox-missing", error));
    CHECK(!error.empty());
    CHECK(!consumer.attached());
    CHECK(consumer.ready_event() == nullptr);
}

TEST(display_mailbox_roundtrip_generations_and_close) {
    DisplayFrameProducer producer;
    UserSecurity security;
    security.build();
    std::string error;
    CHECK(producer.create(L"mbox-roundtrip", &security, error));
    CHECK(producer.created());

    DisplayFrameConsumer consumer;
    CHECK(consumer.attach(L"mbox-roundtrip", error));
    CHECK(consumer.attached());
    CHECK(consumer.producer_running());  // 生产者就是本进程
    CHECK(consumer.ready_event() != nullptr);

    Frame const frame = sample_frame();
    CHECK(producer.publish(frame, 1234));
    CHECK(::WaitForSingleObject(consumer.ready_event(), 0) == WAIT_OBJECT_0);  // 通知到了

    Frame read;
    CHECK(consumer.read(read));
    CHECK_EQ(read.generation, static_cast<std::uint64_t>(1));
    CHECK_EQ(read.produced_at_ms, static_cast<std::uint64_t>(1234));
    CHECK(!read.closed);
    CHECK(read.kind == ViewKind::Content);
    CHECK_EQ(read.items.size(), static_cast<std::size_t>(2));
    CHECK_EQ(read.items[0].value, std::string("54℃"));

    // 覆盖语义：只保留最新一帧。
    Frame smaller;
    smaller.kind = ViewKind::Content;
    FrameItem only;
    only.id = "one";
    only.label = "L";
    only.value = "V";
    smaller.items.push_back(only);
    CHECK(producer.publish(smaller, 5678));
    CHECK(consumer.read(read));
    CHECK_EQ(read.generation, static_cast<std::uint64_t>(2));
    CHECK_EQ(read.produced_at_ms, static_cast<std::uint64_t>(5678));
    CHECK_EQ(read.items.size(), static_cast<std::size_t>(1));
    CHECK_EQ(read.items[0].id, std::string("one"));

    // 正常收尾：保留内容并置 closed。
    CHECK(producer.publish_closed(9999));
    CHECK(consumer.read(read));
    CHECK_EQ(read.generation, static_cast<std::uint64_t>(3));
    CHECK(read.closed);
    CHECK_EQ(read.items.size(), static_cast<std::size_t>(1));

    consumer.detach();
    CHECK(!consumer.attached());
    producer.close();
    CHECK(!producer.created());
}

TEST(display_mailbox_concurrent_publish_never_yields_a_torn_frame) {
    // 写端连续发布、读端同时读：读到的每一帧都必须是自洽的完整帧（序号 + 长度 + 校验和）。
    // detail 的长度与 value 的数值必须与同一条 item 一起变化，撕裂会立刻被这个断言抓住。
    DisplayFrameProducer producer;
    std::string error;
    CHECK(producer.create(L"mbox-concurrent", nullptr, error));
    DisplayFrameConsumer consumer;
    CHECK(consumer.attach(L"mbox-concurrent", error));

    constexpr int kPublishes = 200;
    std::atomic<bool> writer_done{false};
    std::thread writer([&] {
        for (int index = 1; index <= kPublishes; ++index) {
            Frame frame;
            frame.kind = ViewKind::Content;
            FrameItem item;
            item.id = "n";
            item.label = "N";
            item.value = std::to_string(index);
            item.detail = std::string(static_cast<std::size_t>(index % 64) + 1, 'x');
            frame.items.push_back(item);
            producer.publish(frame, index);
        }
        writer_done.store(true);
    });

    int reads = 0;
    int empty_reads = 0;
    while (!writer_done.load() || reads < 40) {
        Frame frame;
        if (consumer.read(frame)) {
            ++reads;
            if (frame.items.empty()) {
                ++empty_reads;
                continue;
            }
            CHECK_EQ(frame.items.size(), static_cast<std::size_t>(1));
            CHECK_EQ(frame.items[0].id, std::string("n"));
            // value 是十进制数，detail 的长度是 value%64+1：两者必须来自同一帧。
            int const value = std::atoi(frame.items[0].value.c_str());
            CHECK(value >= 1 && value <= kPublishes);
            CHECK_EQ(frame.items[0].detail.size(),
                     static_cast<std::size_t>(value % 64) + 1);
        }
        if (reads > 20000) {
            break;
        }
    }
    writer.join();
    CHECK(reads > 0);
    consumer.detach();
    producer.close();
}

// ---- 显示邮箱：attach 之后的帧头篡改 / 生产者退出 / 身份被换 -----------------
//
// 这三条补的都是“只在 attach 时校验一次”留下的空洞：attach 通过之后，同一块 section 仍可能被
// 改写（旧生产者没退干净，或者同名 section 被新生产者重新建起来）。三条都必须在**读**这一侧挡住，
// 而不是靠 attach 时的历史结论。

namespace {

// 按名字把同一块 section 映射成可写，用来在本进程里伪造“帧头被改过”。
class RawMailboxView {
public:
    ~RawMailboxView() { close(); }

    bool open(std::wstring const& scope, std::string& error) {
        close();
        std::wstring const name = display_frame_map_name(scope);
        mapping_ = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (mapping_ == nullptr) {
            error = "打不开显示邮箱共享内存";
            return false;
        }
        view_ =
            ::MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kFrameSharedBytes);
        if (view_ == nullptr) {
            close();
            error = "映射不了显示邮箱";
            return false;
        }
        return true;
    }

    FrameSharedHeader* header() const { return reinterpret_cast<FrameSharedHeader*>(view_); }

    void close() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};

// 本用例程序自己的路径：子模式用它再起一个真正的“别的生产者进程”。
std::wstring test_executable_path() {
    wchar_t buffer[4096];
    DWORD const size = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, size);
}

// 用本程序自己的 `--mailbox-owner` 子模式起一个独立生产者进程。
// “生产者退出”与“身份被换”必须是真的跨进程，同一个进程里造不出来。
class ChildProducer {
public:
    ~ChildProducer() { stop(); }

    bool start(std::wstring const& scope, unsigned hold_ms, std::string& error) {
        stop();
        std::wstring const exe = test_executable_path();
        std::wstring const command = L"\"" + exe + L"\" --mailbox-owner " + scope +
                                     L" --hold-ms " + std::to_wstring(hold_ms);
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (::CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) == FALSE) {
            error = "起不了子生产者进程";
            return false;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        return true;
    }

    bool wait(unsigned timeout_ms) {
        if (process_ == nullptr) {
            return true;
        }
        return ::WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
    }

    // 模拟崩溃：不发任何收尾信号，直接结束。
    void kill() {
        if (process_ != nullptr) {
            ::TerminateProcess(process_, 1);
            ::WaitForSingleObject(process_, 5000);
        }
    }

    void stop() {
        if (process_ != nullptr) {
            ::TerminateProcess(process_, 0);
            ::WaitForSingleObject(process_, 5000);
            ::CloseHandle(process_);
            process_ = nullptr;
        }
    }

private:
    HANDLE process_ = nullptr;
};

// 子模式本体：建邮箱、发一帧、按住不放。崩溃由父进程直接结束本进程来模拟。
int mailbox_owner_child(std::wstring const& scope, unsigned hold_ms) {
    DisplayFrameProducer producer;
    std::string error;
    if (!producer.create(scope, nullptr, error)) {
        return 3;
    }
    if (!producer.publish(sample_frame(), 111)) {
        return 4;
    }
    ::Sleep(hold_ms);
    return 0;
}

// 生产者进程刚起来时帧头可能还没写完，attach 允许重试。
bool attach_with_retry(DisplayFrameConsumer& consumer, std::wstring const& scope, int tries) {
    std::string error;
    for (int attempt = 0; attempt < tries; ++attempt) {
        if (consumer.attach(scope, error)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

TEST(display_mailbox_rejects_malformed_header_after_attach) {
    // attach 之后把帧头逐项改坏，每一次 read() 都必须拒绝；改回来之后必须重新读到。
    // 依据是“版本与一致性在**每次读**上成立”，不是只在 attach 上成立。
    DisplayFrameProducer producer;
    std::string error;
    CHECK(producer.create(L"mbox-tamper", nullptr, error));
    CHECK(producer.publish(sample_frame(), 4321));

    DisplayFrameConsumer consumer;
    CHECK(consumer.attach(L"mbox-tamper", error));

    Frame read;
    CHECK(consumer.read(read));  // 基线：未篡改时读得到
    CHECK_EQ(read.items.size(), static_cast<std::size_t>(2));

    RawMailboxView raw;
    CHECK(raw.open(L"mbox-tamper", error));
    FrameSharedHeader* const header = raw.header();
    CHECK(header != nullptr);

    std::uint32_t const item_count = header->item_count;
    std::uint32_t const kind = header->kind;
    std::uint32_t const alarm = header->alarm;
    std::uint32_t const failure = header->failure;
    std::uint32_t const flags = header->flags;

    auto expect_rejected = [&](char const* what) {
        Frame out;
        if (consumer.read(out)) {
            ::testing::report_failure(std::string("篡改帧头后仍被接受：") + what);
        }
    };

    header->version = 99;
    expect_rejected("version");
    header->version = kFrameVersion;
    header->magic = 0;
    expect_rejected("magic");
    header->magic = kFrameMagic;
    header->header_bytes = 8;
    expect_rejected("header_bytes");
    header->header_bytes = static_cast<std::uint32_t>(sizeof(FrameSharedHeader));
    header->capacity = 12345;
    expect_rejected("capacity");
    header->capacity = static_cast<std::uint32_t>(kFrameSharedBytes);
    header->payload_capacity = 1;
    expect_rejected("payload_capacity");
    header->payload_capacity = static_cast<std::uint32_t>(kFramePayloadCapacity);
    header->flags = flags | 0x8000u;
    expect_rejected("未知 flag 位");
    header->flags = flags;
    // 下面四条是**重复元数据**：数值都留在各自合法范围内，所以只有“与帧体一致”这一条能挡住它。
    header->item_count = item_count + 1;
    expect_rejected("item_count 与 items 不符");
    header->item_count = item_count;
    header->kind = kind == 0 ? 1u : 0u;
    expect_rejected("kind 与帧体不符");
    header->kind = kind;
    header->alarm = alarm ^ 1u;
    expect_rejected("alarm 与帧体不符");
    header->alarm = alarm;
    header->failure = failure == 3 ? 4u : 3u;
    expect_rejected("failure 与帧体不符");
    header->failure = failure;

    // 全部改回来之后必须重新可读：否则上面那些 false 可能只是因为邮箱被彻底弄坏了。
    CHECK(consumer.read(read));
    CHECK_EQ(read.generation, static_cast<std::uint64_t>(1));

    raw.close();
    consumer.detach();
    producer.close();
}

TEST(display_mailbox_refuses_frames_after_producer_exit) {
    // 生产者退出之后 section 还活着（这边的句柄还在），里面也仍有一帧合法内容。
    // read() 必须拒绝：已经死掉的主进程不能再提供可绘制的帧。
    ChildProducer child;
    std::string error;
    CHECK(child.start(L"mbox-dead", 30000, error));

    DisplayFrameConsumer consumer;
    CHECK(attach_with_retry(consumer, L"mbox-dead", 250));
    CHECK(consumer.producer_running());
    Frame read;
    CHECK(consumer.read(read));
    CHECK_EQ(read.items.size(), static_cast<std::size_t>(2));

    child.kill();
    CHECK(child.wait(5000));
    CHECK(!consumer.producer_running());

    Frame stale;
    CHECK(!consumer.read(stale));  // 陈旧帧不许再交出去

    consumer.detach();
}

TEST(display_mailbox_stays_bound_to_the_original_owner) {
    // 原生产者**还活着**，但同名的 section 被另一个进程重新建起来并改写了帧头里的身份。
    // 消费者绑定的必须是 attach 时那个活动句柄对应的身份，不能接受新主人的帧。
    ChildProducer first;
    std::string error;
    CHECK(first.start(L"mbox-rebind", 30000, error));

    DisplayFrameConsumer consumer;
    CHECK(attach_with_retry(consumer, L"mbox-rebind", 250));
    Frame read;
    CHECK(consumer.read(read));

    // 第二个进程用同一个 scope 建同一块 section：create() 会覆盖帧头里的身份。
    ChildProducer second;
    CHECK(second.start(L"mbox-rebind", 8000, error));

    bool refused = false;
    for (int attempt = 0; attempt < 250 && !refused; ++attempt) {
        Frame out;
        if (!consumer.read(out)) {
            refused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(refused);
    // 关键：原主人仍然活着，所以挡下这一帧的必须是**身份校验**，而不是“生产者已退出”。
    CHECK(consumer.producer_running());

    consumer.detach();
    second.stop();
    first.stop();
}

// ---- 回环 mock 生产端 ------------------------------------------------------
//
// 这一组用例只碰 127.0.0.1：给 HTTP 层量“整次墙钟预算”与取消。慢头部与慢滴流正是旧实现
// 能拖过预算的两条路径（旧实现每次阻塞调用前重设 WinHTTP 超时，但两段都可能各自花掉预算）。
// 不连外网、不读凭据、不需要外部进程。

class LoopbackServer {
public:
    ~LoopbackServer() { stop(); }
    LoopbackServer() = default;
    LoopbackServer(LoopbackServer const&) = delete;
    LoopbackServer& operator=(LoopbackServer const&) = delete;

    bool start(std::string& error) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "WSAStartup 失败";
            return false;
        }
        winsock_ready_ = true;
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            error = "socket 失败";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            error = "bind 失败";
            return false;
        }
        int length = static_cast<int>(sizeof(address));
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            error = "getsockname 失败";
            return false;
        }
        port_ = ::ntohs(address.sin_port);
        if (::listen(listener_, 8) == SOCKET_ERROR) {
            error = "listen 失败";
            return false;
        }
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    std::wstring url(std::wstring const& path) const {
        return L"http://127.0.0.1:" + std::to_wstring(port_) + path;
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ != INVALID_SOCKET) {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (winsock_ready_) {
            ::WSACleanup();
            winsock_ready_ = false;
        }
    }

private:
    static std::string ok_body() {
        return "{\"version\":1,\"items\":[{\"id\":\"cpu\",\"label\":\"CPU\",\"value\":\"54\\u2103\"}]}";
    }

    static bool send_all(SOCKET client, std::string const& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            int const sent = ::send(client, bytes.data() + offset,
                                    static_cast<int>(bytes.size() - offset), 0);
            if (sent <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    static void send_response(SOCKET client, std::string const& body, char const* status) {
        std::string const head = std::string("HTTP/1.1 ") + status +
                                 "\r\nContent-Type: application/json; charset=utf-8\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n";
        if (send_all(client, head)) {
            send_all(client, body);
        }
    }

    void serve() {
        while (!stop_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100 * 1000;
            int const ready = ::select(0, &set, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }
            SOCKET const client = ::accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            if (stop_.load()) {
                ::closesocket(client);
                break;
            }
            handle(client);
            ::closesocket(client);
        }
    }

    void handle(SOCKET client) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            int const got = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (got <= 0) {
                return;
            }
            request.append(buffer, static_cast<std::size_t>(got));
        }
        std::string path;
        std::size_t const first = request.find(' ');
        if (first != std::string::npos) {
            std::size_t const second = request.find(' ', first + 1);
            if (second != std::string::npos) {
                path = request.substr(first + 1, second - first - 1);
            }
        }

        if (path == "/ok") {
            send_response(client, ok_body(), "200 OK");
            return;
        }
        if (path == "/redirect") {
            std::string const head =
                "HTTP/1.1 302 Found\r\nLocation: /ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, head);
            return;
        }
        if (path == "/redirect-loop") {
            std::string const head = "HTTP/1.1 302 Found\r\nLocation: /redirect-loop\r\n"
                                     "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, head);
            return;
        }
        if (path == "/status204") {
            std::string const head =
                "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, head);
            return;
        }
        if (path == "/slow-headers") {
            // 慢头部：连接建得起来，响应头迟迟不来（旧实现就是在这里拖过预算的）。
            for (int slept = 0; slept < 1200 && !stop_.load(); slept += 50) {
                ::Sleep(50);
            }
            send_response(client, ok_body(), "200 OK");
            return;
        }
        if (path == "/slow-drip") {
            // 慢滴流：头先给全，正文一个字节一个字节地挤。
            std::string const body = ok_body();
            std::string const head =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            if (!send_all(client, head)) {
                return;
            }
            for (char const byte : body) {
                if (stop_.load()) {
                    return;
                }
                if (!send_all(client, std::string(1, byte))) {
                    return;
                }
                ::Sleep(300);
            }
            return;
        }
        if (path == "/drip-fast") {
            // 快滴流：每 25 ms 一个字节，取消时“正有一次 WinHttpReadData 在途”的概率很高。
            std::string const body = ok_body();
            std::string const head =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            if (!send_all(client, head)) {
                return;
            }
            for (char const byte : body) {
                if (stop_.load()) {
                    return;
                }
                if (!send_all(client, std::string(1, byte))) {
                    return;
                }
                ::Sleep(25);
            }
            return;
        }
        if (path == "/expires-soon") {
            // 一份**刚收到就已经定好很快到期**的合法快照：取数本身成功，到期由主进程按绝对
            // 时间独立判定。用例用它验“到期那一刻显示面就该看到整体失效”，不等下一次轮询。
            long long const expires_at = unix_ms() + 800;
            std::string const body =
                "{\"version\":1,\"expiresAtMs\":" + std::to_string(expires_at) +
                ",\"items\":[{\"id\":\"cpu\",\"label\":\"CPU\",\"value\":\"54\"}]}";
            send_response(client, body, "200 OK");
            return;
        }
        if (path == "/hang") {
            // 收下请求但永远不回答：整次取数只能由主进程的墙钟预算叫停。用例用它验
            // “截止失败那一刻显示面就该看到整体失效”，不等下一次重试。
            for (int slept = 0; slept < 20000 && !stop_.load(); slept += 50) {
                ::Sleep(50);
            }
            return;
        }
        if (path == "/oversized") {
            // 正文超过 1 MiB 上限：客户端必须边读边拒绝，不等读完。
            std::size_t const size = kMaxBodyBytes + 4096;
            std::string const head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                     "Content-Length: " +
                                     std::to_string(size) + "\r\nConnection: close\r\n\r\n";
            if (!send_all(client, head)) {
                return;
            }
            std::string const chunk(64 * 1024, 'x');
            std::size_t sent = 0;
            while (sent < size && !stop_.load()) {
                std::size_t const take = (std::min)(chunk.size(), size - sent);
                if (!send_all(client, chunk.substr(0, take))) {
                    return;
                }
                sent += take;
            }
            return;
        }
        std::string const not_found =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client, not_found);
    }

    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread thread_{};
    std::atomic<bool> stop_{false};
    bool winsock_ready_ = false;
};

TEST(http_get_accepts_only_200_and_keeps_the_body) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 4000;

    HttpResult const ok = http_get(server.url(L"/ok"), limits);
    CHECK(ok.ok);
    CHECK_EQ(ok.status, 200L);
    CHECK(ok.body.find("\"version\":1") != std::string::npos);

    HttpResult const no_content = http_get(server.url(L"/status204"), limits);
    CHECK(!no_content.ok);
    CHECK(no_content.failure == HttpFailure::Status);
    CHECK_EQ(no_content.status, 204L);
}

TEST(http_get_follows_redirects_but_stops_at_the_limit) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 4000;

    HttpResult const hopped = http_get(server.url(L"/redirect"), limits);
    CHECK(hopped.ok);
    CHECK(hopped.body.find("\"version\":1") != std::string::npos);

    HttpResult const looped = http_get(server.url(L"/redirect-loop"), limits);
    CHECK(!looped.ok);
    CHECK(looped.failure == HttpFailure::Redirect);
}

TEST(http_get_rejects_an_oversized_body_while_reading) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 6000;

    HttpResult const big = http_get(server.url(L"/oversized"), limits);
    CHECK(!big.ok);
    CHECK(big.failure == HttpFailure::TooLarge);
}

TEST(http_get_whole_budget_holds_on_slow_headers) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 600;

    long long const started = tick_ms();
    HttpResult const result = http_get(server.url(L"/slow-headers"), limits);
    long long const elapsed = tick_ms() - started;

    CHECK(!result.ok);
    CHECK(result.failure == HttpFailure::Timeout);
    CHECK(elapsed >= 500);   // 不能在预算之前就早退
    CHECK(elapsed <= 2000);  // 也不能拖过预算（旧实现在这里是约 4 s 的固定天花板）
}

TEST(http_get_whole_budget_holds_on_slow_drip) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 600;

    long long const started = tick_ms();
    HttpResult const result = http_get(server.url(L"/slow-drip"), limits);
    long long const elapsed = tick_ms() - started;

    CHECK(!result.ok);
    CHECK(result.failure == HttpFailure::Timeout);
    CHECK(elapsed >= 500);
    CHECK(elapsed <= 2000);
}

TEST(http_get_abort_cancels_promptly_without_waiting_for_the_budget) {
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 20000;  // 预算故意开得很大：取消必须自己把它掐断
    auto abort = std::make_shared<std::atomic<bool>>(false);

    HttpResult result;
    long long elapsed = 0;
    std::thread worker([&] {
        long long const started = tick_ms();
        result = http_get(server.url(L"/slow-drip"), limits, abort.get());
        elapsed = tick_ms() - started;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    abort->store(true);
    worker.join();

    CHECK(!result.ok);
    CHECK(result.failure == HttpFailure::Aborted);
    CHECK(elapsed <= 1500);
    abort->store(false);
}

TEST(http_get_cancelled_fetches_do_not_pile_up) {
    // 连续五次被预算取消的取数：总耗时必须接近 5×预算。旧实现里每次都会拖到自己的
    // 固定天花板，被放弃的作业会占着名额让新源排不上队。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 300;

    long long const started = tick_ms();
    int timeouts = 0;
    for (int index = 0; index < 5; ++index) {
        HttpResult const result = http_get(server.url(L"/slow-headers"), limits);
        if (result.failure == HttpFailure::Timeout) {
            ++timeouts;
        }
    }
    long long const elapsed = tick_ms() - started;

    CHECK_EQ(timeouts, 5);
    CHECK(elapsed <= 5 * 300 + 2500);
}

TEST(http_get_abort_mid_read_keeps_context_and_buffer_alive) {
    // 慢滴流下中途取消：取消时通常正有一次 WinHttpReadData 在途，它写入的是上下文持有的
    // 缓冲区。反复取消、并让迟到的完成回调在函数返回之后到达，用来暴露“缓冲区或上下文
    // 被提前释放”。Debug 构建带堆校验，这类越界写会在这里直接暴露。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    HttpLimits limits;
    limits.timeout_ms = 4000;
    for (int round = 0; round < 16; ++round) {
        auto abort = std::make_shared<std::atomic<bool>>(false);
        HttpResult result;
        std::thread worker([&] {
            result = http_get(server.url(L"/drip-fast"), limits, abort.get());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30 + (round % 4) * 35));
        abort->store(true);
        worker.join();

        CHECK(!result.ok);
        CHECK(result.failure == HttpFailure::Aborted || result.failure == HttpFailure::Timeout);
        // 给迟到的回调一点时间：它们必须落在还活着的上下文上。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---- 检查器：机器可读输出与 stdin 管道 -------------------------------------
//
// 这两个用例都**只认实际输出**，不认退出码：缺陷 1 是接受路径多出一个 `}`（退出码本来就是 0），
// 缺陷 2 是管道 stdin 被当成读取失败。两者都只能在真实子进程上验——也正是它们当初漏掉的原因。

namespace {

int g_temp_file_counter = 0;

std::wstring check_executable_path() {
    std::wstring const self = test_executable_path();
    std::size_t const slash = self.find_last_of(L"\\/");
    std::wstring const dir =
        slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1);
    return dir + L"deskfeed-check.exe";
}

class TempFile {
public:
    ~TempFile() { remove(); }

    bool write(std::string const& bytes, char const* suffix, std::string& error) {
        remove();
        wchar_t directory[MAX_PATH]{};
        if (::GetTempPathW(static_cast<DWORD>(std::size(directory)), directory) == 0) {
            error = "拿不到临时目录";
            return false;
        }
        path_ = std::wstring(directory) + L"deskfeed-tests-" +
                std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(++g_temp_file_counter) + L"-" +
                std::wstring(suffix, suffix + std::strlen(suffix));
        HANDLE const file =
            ::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "建不了临时文件";
            path_.clear();
            return false;
        }
        DWORD written = 0;
        bool const ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                    nullptr) != FALSE &&
                        written == bytes.size();
        ::CloseHandle(file);
        if (!ok) {
            error = "写不了临时文件";
            return false;
        }
        return true;
    }

    std::wstring const& path() const { return path_; }

    void remove() {
        if (!path_.empty()) {
            ::DeleteFileW(path_.c_str());
            path_.clear();
        }
    }

private:
    std::wstring path_;
};

struct CheckResult {
    DWORD exit_code = 0;
    std::string out;
    std::string err;
};

// 起一次真实的 deskfeed-check.exe。stdin 可以是管道（程序化调用者的常规形态）或文件句柄
// （等价于 shell 的 `< file`）——两条路都必须能用。
class CheckRun {
public:
    enum class Stdin { None, Pipe, File };

    ~CheckRun() { close(); }

    bool start(std::vector<std::wstring> const& args, Stdin stdin_mode,
               std::wstring const& stdin_file, std::string& error) {
        close();
        std::wstring const exe = check_executable_path();
        std::wstring command = L"\"" + exe + L"\"";
        for (std::wstring const& arg : args) {
            command += arg.find(L' ') == std::wstring::npos ? (L" " + arg)
                                                            : (L" \"" + arg + L"\"");
        }
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');

        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        HANDLE child_stdout = nullptr;
        HANDLE child_stderr = nullptr;
        HANDLE child_stdin = nullptr;
        if (::CreatePipe(&stdout_read_, &child_stdout, &inheritable, 0) == FALSE) {
            error = "CreatePipe(stdout) 失败";
            close();
            return false;
        }
        ::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        if (::CreatePipe(&stderr_read_, &child_stderr, &inheritable, 0) == FALSE) {
            error = "CreatePipe(stderr) 失败";
            close();
            return false;
        }
        ::SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0);

        if (stdin_mode == Stdin::Pipe) {
            HANDLE parent_write = nullptr;
            if (::CreatePipe(&child_stdin, &parent_write, &inheritable, 0) == FALSE) {
                error = "CreatePipe(stdin) 失败";
                close();
                return false;
            }
            ::SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
            stdin_write_ = parent_write;
        } else if (stdin_mode == Stdin::File) {
            child_stdin = ::CreateFileW(stdin_file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (child_stdin == INVALID_HANDLE_VALUE) {
                error = "打不开当 stdin 的文件";
                close();
                return false;
            }
        } else {
            child_stdin = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inheritable, OPEN_EXISTING, 0, nullptr);
            if (child_stdin == INVALID_HANDLE_VALUE) {
                error = "打不开 NUL";
                close();
                return false;
            }
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin;
        startup.hStdOutput = child_stdout;
        startup.hStdError = child_stderr;

        PROCESS_INFORMATION info{};
        BOOL const created =
            ::CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        // 这几端已经交给子进程，父进程不该再持有它们——否则管道永远等不到 EOF。
        ::CloseHandle(child_stdout);
        ::CloseHandle(child_stderr);
        ::CloseHandle(child_stdin);
        if (created == FALSE) {
            error = "起不了 deskfeed-check.exe（" + utf8_from_wide(exe) + "）";
            close();
            return false;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        return true;
    }

    bool write_stdin(std::string const& bytes) {
        if (stdin_write_ == nullptr) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (::WriteFile(stdin_write_, bytes.data() + offset,
                            static_cast<DWORD>(bytes.size() - offset), &written, nullptr) == FALSE ||
                written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    void close_stdin() {
        if (stdin_write_ != nullptr) {
            ::CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
    }

    bool finish(CheckResult& result, unsigned timeout_ms) {
        close_stdin();
        drain(stdout_read_, result.out);
        drain(stderr_read_, result.err);
        if (process_ == nullptr) {
            return false;
        }
        if (::WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        if (::GetExitCodeProcess(process_, &code) == FALSE) {
            return false;
        }
        result.exit_code = code;
        return true;
    }

    void close() {
        close_stdin();
        for (HANDLE* handle : {&stdout_read_, &stderr_read_}) {
            if (*handle != nullptr) {
                ::CloseHandle(*handle);
                *handle = nullptr;
            }
        }
        if (process_ != nullptr) {
            if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
                ::TerminateProcess(process_, 1);
                ::WaitForSingleObject(process_, 5000);
            }
            ::CloseHandle(process_);
            process_ = nullptr;
        }
    }

private:
    static void drain(HANDLE handle, std::string& out) {
        if (handle == nullptr) {
            return;
        }
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            if (::ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) == FALSE || read == 0) {
                break;
            }
            out.append(buffer, read);
        }
    }

    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
};

// 一条合法快照：接受路径的 report json 必须能被称为严格 JSON 的解析器吃下去。
std::string accept_body() {
    return "{\"version\":1,\"items\":[{\"id\":\"a\",\"label\":\"A\",\"value\":\"V\"}]}";
}

// 主进程产物路径：与用例在同一个 bin 目录。
std::wstring master_executable_path() {
    std::wstring const self = test_executable_path();
    std::size_t const slash = self.find_last_of(L"\\/");
    std::wstring const dir =
        slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1);
    return dir + L"deskfeed.exe";
}

// 同目录的产物路径（假注入器就在用例同一个 bin 目录里）。
std::wstring sibling_executable_path(wchar_t const* file_name) {
    std::wstring const self = test_executable_path();
    std::size_t const slash = self.find_last_of(L"\\/");
    std::wstring const dir =
        slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1);
    return dir + file_name;
}

// 假注入器产物：任务栏编排的离线量具（见 tests/fake_injector.cpp）。
std::wstring fake_injector_path() { return sibling_executable_path(L"deskfeed-fake-injector.exe"); }

bool copy_file(std::wstring const& from, std::wstring const& to, std::wstring& error) {
    if (::CopyFileW(from.c_str(), to.c_str(), FALSE) == FALSE) {
        error = L"复制失败：" + from + L" -> " + to;
        return false;
    }
    return true;
}

// 读一个**正在被写**的文件。
//
// 不能直接用 read_file_bytes：平台日志用 FILE_APPEND_DATA + (SHARE_READ|SHARE_WRITE) 打开，
// 而 read_file_bytes 的共享模式只有 FILE_SHARE_READ，不许可写者继续持有写访问，于是对活着的
// 实例日志一律 ERROR_SHARING_VIOLATION。这里按写者的共享模式兼容地打开，才能在实例运行期间
// 轮询日志。文件不存在时 exists=false 且返回 true。
bool read_shared_file(std::wstring const& path, std::string& bytes, bool& exists) {
    bytes.clear();
    exists = false;
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD const code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        return false;
    }
    exists = true;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE) {
        ::CloseHandle(file);
        return false;
    }
    if (size.QuadPart > 4 * 1024 * 1024) {
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        DWORD const want = static_cast<DWORD>(bytes.size() - offset);
        if (::ReadFile(file, bytes.data() + offset, want, &read, nullptr) == FALSE) {
            ::CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    bytes.resize(offset);
    ::CloseHandle(file);
    return true;
}

// 读一个文件；不存在时 exists=false 且返回 true。
bool read_file_if_present(std::wstring const& path, std::string& bytes, bool& exists) {
    return read_shared_file(path, bytes, exists);
}

// 轮询日志文件，等到出现某段标记为止。隔离实例在轮询期间一直在跑，所以这是“等到主进程真的
// 走到那一步”的确定性同步点，而不是靠固定 sleep 猜它走没走到。
bool wait_for_log_marker(std::wstring const& log_path, std::string const& marker,
                         unsigned timeout_ms) {
    long long const deadline = tick_ms() + timeout_ms;
    while (tick_ms() < deadline) {
        std::string bytes;
        bool exists = false;
        if (read_file_if_present(log_path, bytes, exists) && exists &&
            bytes.find(marker) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// 断言日志里出现了某个标记；失败时把日志存在性与尾部一并报出来，便于诊断。
void check_log_marker(std::wstring const& log_path, std::wstring const& marker,
                      unsigned timeout_ms) {
    if (wait_for_log_marker(log_path, utf8_from_wide(marker), timeout_ms)) {
        return;
    }
    std::string bytes;
    bool exists = false;
    read_file_if_present(log_path, bytes, exists);
    std::string const tail = bytes.size() > 1500 ? bytes.substr(bytes.size() - 1500) : bytes;
    testing::report_failure("日志里没等到标记「" + testing::to_display(marker) + "」；exists=" +
                            (exists ? "yes" : "no") + "；日志尾部：\n" + tail);
}

// 等到隔离实例的控制窗口真的建出来（= 互斥体已经被它持有）。用例必须在启动任何**客户端**之前
// 先等这一步：否则那个客户端会在实例就绪前抢到互斥体、把自己变成主实例（一条常驻进程），
// 后面的断言全部失去意义。
bool wait_for_control_window(std::wstring const& scope, unsigned timeout_ms) {
    std::wstring const class_name = scoped_name(kControlWindowClassBase, scope);
    long long const deadline = tick_ms() + timeout_ms;
    while (tick_ms() < deadline) {
        if (control_window_exists(class_name)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// 隔离运行目录：放一份 deskfeed.exe 副本与同目录的假 deskfeed-inject.exe。
//
// 为什么要复制：编排层按“与主进程模块同目录的 deskfeed-inject.exe”找注入器
// （taskbar_supervisor.cpp 的 directory_of(module_self_path())），所以监视器必须与那份**副本**
// 同目录，不能占用构建树里真正的 deskfeed-inject.exe。收尾只删自己复制进去的东西。
class IsolatedRunDirectory {
public:
    ~IsolatedRunDirectory() { remove(); }

    bool create(std::string& error) {
        remove();
        wchar_t base[MAX_PATH]{};
        if (::GetTempPathW(static_cast<DWORD>(std::size(base)), base) == 0) {
            error = "拿不到临时目录";
            return false;
        }
        path_ = std::wstring(base) + L"deskfeed-tests-" +
                std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(++g_temp_file_counter) + L"-run";
        if (::CreateDirectoryW(path_.c_str(), nullptr) == FALSE) {
            error = "建不了隔离运行目录";
            path_.clear();
            return false;
        }
        return true;
    }

    std::wstring const& path() const { return path_; }

    void remove() {
        if (path_.empty()) {
            return;
        }
        ::DeleteFileW((path_ + L"\\deskfeed.exe").c_str());
        ::DeleteFileW((path_ + L"\\deskfeed-inject.exe").c_str());
        ::DeleteFileW((path_ + L"\\fake-injector.log").c_str());
        ::RemoveDirectoryW(path_.c_str());
        path_.clear();
    }

private:
    std::wstring path_;
};

// 一个活的“生产者进程”替身：真实存在、可以用 SYNCHRONIZE 打开、可以被结束（用来触发绑定退出）。
// 用系统自带的 ping 撑住时间，不引入任何新产物；绝不结束别人的进程。
class DummyProducer {
public:
    ~DummyProducer() { stop(); }

    bool start(std::string& error) {
        stop();
        wchar_t command[] = L"cmd.exe /c ping -n 120 127.0.0.1 > nul";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (::CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &startup, &info) == FALSE) {
            error = "起不了生产者替身进程";
            return false;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        pid_ = info.dwProcessId;
        return true;
    }

    unsigned long pid() const { return pid_; }

    // 结束替身进程（模拟“生产者退出”）。
    void kill() {
        if (process_ != nullptr && ::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            ::TerminateProcess(process_, 0);
            ::WaitForSingleObject(process_, 5000);
        }
    }

    void stop() {
        if (process_ != nullptr) {
            kill();
            ::CloseHandle(process_);
            process_ = nullptr;
            pid_ = 0;
        }
    }

private:
    HANDLE process_ = nullptr;
    unsigned long pid_ = 0;
};


// 隔离实例自己用的临时目录。收尾只删自己写进去的东西。
class TempDirectory {
public:
    ~TempDirectory() { remove(); }

    bool create(std::string& error) {
        remove();
        wchar_t base[MAX_PATH]{};
        if (::GetTempPathW(static_cast<DWORD>(std::size(base)), base) == 0) {
            error = "拿不到临时目录";
            return false;
        }
        path_ = std::wstring(base) + L"deskfeed-tests-" +
                std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(++g_temp_file_counter) + L"-dir";
        if (::CreateDirectoryW(path_.c_str(), nullptr) == FALSE) {
            error = "建不了临时目录";
            path_.clear();
            return false;
        }
        return true;
    }

    std::wstring const& path() const { return path_; }

    void remove() {
        if (!path_.empty()) {
            ::DeleteFileW((path_ + L"\\settings.ini").c_str());
            ::RemoveDirectoryW(path_.c_str());  // 非空会失败，正好不会误删
            path_.clear();
        }
    }

private:
    std::wstring path_;
};

// 临时给**本进程**设一个环境变量，析构时恢复原值（原来没有就删掉）。
//
// 子进程用 CreateProcessW 的默认行为继承本进程的环境块，所以隔离实例的失败注入点可以这样
// 精确地只作用在这一次 start() 上，不必给命令行加一个生产上也会存在的开关。用例是串行跑的，
// 恢复发生在子进程已经起来之后，不会串到别的用例。
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::wstring name, std::wstring const& value) : name_(std::move(name)) {
        wchar_t buffer[64]{};
        DWORD const chars =
            ::GetEnvironmentVariableW(name_.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
        if (chars > 0 && chars < std::size(buffer)) {
            had_old_ = true;
            old_ = std::wstring(buffer, chars);
        }
        ::SetEnvironmentVariableW(name_.c_str(), value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        ::SetEnvironmentVariableW(name_.c_str(), had_old_ ? old_.c_str() : nullptr);
    }

    ScopedEnvironmentVariable(ScopedEnvironmentVariable const&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable const&) = delete;

private:
    std::wstring name_;
    std::wstring old_;
    bool had_old_ = false;
};

// 起一个子进程并持有它的句柄（用来跑真实的 deskfeed.exe）。
class ChildProcess {
public:
    ~ChildProcess() { stop(); }

    bool start(std::wstring const& exe, std::vector<std::wstring> const& args, std::string& error) {
        stop();
        std::wstring command = L"\"" + exe + L"\"";
        for (std::wstring const& arg : args) {
            command += arg.find(L' ') == std::wstring::npos ? (L" " + arg)
                                                            : (L" \"" + arg + L"\"");
        }
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (::CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) == FALSE) {
            error = "起不了子进程：" + utf8_from_wide(exe);
            return false;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        return true;
    }

    bool wait(unsigned timeout_ms) {
        return process_ != nullptr &&
               ::WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
    }

    // 已经退出时给出退出码（诊断用）；还在跑时返回 -1。
    long long exit_code() const {
        if (process_ == nullptr || ::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            return -1;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process_, &code);
        return static_cast<long long>(code);
    }

    void stop() {
        if (process_ != nullptr) {
            if (::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
                ::TerminateProcess(process_, 1);
                ::WaitForSingleObject(process_, 5000);
            }
            ::CloseHandle(process_);
            process_ = nullptr;
        }
    }

private:
    HANDLE process_ = nullptr;
};

// 收掉隔离实例留下的两处痕迹：--state-dir 里的设置文件，以及隔离日志目录里的日志。
// 两处都只在**变空之后**才删目录，所以不会误删别人的东西。
void cleanup_isolated_instance(std::wstring const& scope, std::wstring const& state_dir) {
    if (!state_dir.empty()) {
        ::DeleteFileW((state_dir + L"\\settings.ini").c_str());
        ::RemoveDirectoryW(state_dir.c_str());
    }
    std::wstring const log = deskfeed_log_path(scope);
    if (!log.empty()) {
        ::DeleteFileW(log.c_str());
        std::size_t const slash = log.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            ::RemoveDirectoryW(log.substr(0, slash).c_str());
        }
    }
}

}  // namespace

TEST(check_report_json_accept_path_is_strict_json) {
    std::string error;
    TempFile file;
    CHECK(file.write(accept_body(), "accept.json", error));

    CheckRun run;
    CHECK(run.start({L"--file", file.path(), L"--report", L"json"}, CheckRun::Stdin::None, L"",
                    error));
    CheckResult result;
    CHECK(run.finish(result, 30000));
    CHECK_EQ(result.exit_code, static_cast<DWORD>(0));
    CHECK_EQ(result.err, std::string(""));

    // 关键断言：多余的那个 `}` 会让严格解析器整份拒绝。退出码本来就是 0，所以只看退出码
    // 永远抓不到这个缺陷。
    deskfeed::JsonValue parsed;
    std::string parse_error;
    CHECK(deskfeed::json_parse(result.out, parsed, parse_error));
    CHECK(parsed.is_object());
    deskfeed::JsonValue const* accept = parsed.member("accept");
    CHECK(accept != nullptr && accept->is_bool() && accept->boolean);
    deskfeed::JsonValue const* kind = parsed.member("kind");
    CHECK(kind != nullptr && kind->is_string());
    CHECK_EQ(kind->text, std::string("none"));
    deskfeed::JsonValue const* items = parsed.member("items");
    CHECK(items != nullptr && items->is_array());
    CHECK_EQ(items->items.size(), static_cast<std::size_t>(1));
}

TEST(check_report_json_reject_path_is_strict_json) {
    std::string error;
    TempFile file;
    CHECK(file.write("{\"version\":2,\"items\":[]}", "reject.json", error));

    CheckRun run;
    CHECK(run.start({L"--file", file.path(), L"--report", L"json"}, CheckRun::Stdin::None, L"",
                    error));
    CheckResult result;
    CHECK(run.finish(result, 30000));
    CHECK_EQ(result.exit_code, static_cast<DWORD>(2));

    deskfeed::JsonValue parsed;
    std::string parse_error;
    CHECK(deskfeed::json_parse(result.out, parsed, parse_error));
    deskfeed::JsonValue const* accept = parsed.member("accept");
    CHECK(accept != nullptr && accept->is_bool() && !accept->boolean);
    deskfeed::JsonValue const* kind = parsed.member("kind");
    CHECK(kind != nullptr && kind->is_string());
    CHECK_EQ(kind->text, std::string("version"));
}

TEST(check_stdin_accepts_a_pipe) {
    // 程序化调用者（Node / Python 起子进程、任何 `... | deskfeed-check`）给的就是管道。
    // 旧实现把管道写端关闭后的 ERROR_BROKEN_PIPE 当成读取失败，这里必须是 0 且输出可解析。
    std::string error;
    CheckRun run;
    CHECK(run.start({L"--stdin", L"--report", L"json"}, CheckRun::Stdin::Pipe, L"", error));
    CHECK(run.write_stdin(accept_body()));

    CheckResult result;
    CHECK(run.finish(result, 30000));
    CHECK_EQ(result.exit_code, static_cast<DWORD>(0));
    CHECK_EQ(result.err, std::string(""));

    deskfeed::JsonValue parsed;
    std::string parse_error;
    CHECK(deskfeed::json_parse(result.out, parsed, parse_error));
    deskfeed::JsonValue const* accept = parsed.member("accept");
    CHECK(accept != nullptr && accept->is_bool() && accept->boolean);
}

TEST(check_stdin_accepts_a_file_handle) {
    // 另一半：shell 的 `< file` 是文件句柄，不能因为修管道而弄坏它。
    std::string error;
    TempFile file;
    CHECK(file.write(accept_body(), "stdin.json", error));

    CheckRun run;
    CHECK(run.start({L"--stdin", L"--report", L"json"}, CheckRun::Stdin::File, file.path(), error));
    CheckResult result;
    CHECK(run.finish(result, 30000));
    CHECK_EQ(result.exit_code, static_cast<DWORD>(0));

    deskfeed::JsonValue parsed;
    std::string parse_error;
    CHECK(deskfeed::json_parse(result.out, parsed, parse_error));
    deskfeed::JsonValue const* accept = parsed.member("accept");
    CHECK(accept != nullptr && accept->is_bool() && accept->boolean);
}

TEST(master_publishes_frames_into_a_scoped_mailbox) {
    // “主进程 → 显示邮箱”这条 seam 的离线验收：真实 master 进程 + 隔离 scope + 回环 mock。
    // 这正是 --frame-mailbox 存在的理由——不建窗口、不注入、不碰生产命名空间，也能验这条通道。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mbframe-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/ok"), L"--poll-ms", L"1000"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    // 启动时 force 发布的那一帧是 waiting；回环取数回来之后必须变成内容帧。
    Frame frame;
    bool got_content = false;
    for (int attempt = 0; attempt < 300 && !got_content; ++attempt) {
        if (consumer.read(frame) && frame.kind == ViewKind::Content && !frame.items.empty()) {
            got_content = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got_content);
    CHECK_EQ(frame.items.size(), static_cast<std::size_t>(1));
    CHECK_EQ(frame.items[0].id, std::string("cpu"));
    CHECK(!frame.closed);

    // 正常退出走控制通道。master 退出之后 read() 必须拒绝：已经死掉的生产者不再提供可绘制帧。
    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    CHECK(!consumer.producer_running());
    Frame after;
    CHECK(!consumer.read(after));

    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

// 读到一帧的**生产者时刻**（frame.produced_at_ms）——比“用例这边什么时候读到”更接近真实
// 显示面看到它的时刻，也去掉了用例轮询间隔带来的抖动。
long long produced_at_ms(Frame const& frame) { return frame.produced_at_ms; }

// 显示面“已经整体失效”的判据：内容帧要求 alarm（有值且全红）；无内容的帧要求 kind == Error。
bool frame_shows_alarm(Frame const& frame) {
    if (frame.kind == ViewKind::Content) {
        return frame.alarm;
    }
    if (frame.kind == ViewKind::Error) {
        return true;
    }
    return false;
}

// 等到“显示面已经看到整体失效”，返回它被生产出来的时刻；超时报 -1。
long long wait_for_alarm(DisplayFrameConsumer const& consumer, long long timeout_ms) {
    long long const deadline = tick_ms() + timeout_ms;
    Frame frame;
    for (;;) {
        if (consumer.read(frame) && frame_shows_alarm(frame)) {
            return produced_at_ms(frame);
        }
        if (tick_ms() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// 等到第一帧“有条目的内容帧”，返回它被生产出来的时刻；超时报 -1。
long long wait_for_content(DisplayFrameConsumer const& consumer, long long timeout_ms) {
    long long const deadline = tick_ms() + timeout_ms;
    Frame frame;
    for (;;) {
        if (consumer.read(frame) && frame.kind == ViewKind::Content && !frame.items.empty()) {
            return produced_at_ms(frame);
        }
        if (tick_ms() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

TEST(master_publishes_taskbar_screen_config_and_persists_changes) {
    // v2 显示帧里的托盘屏幕配置：设置文件 → 帧；控制通道 SetScreens → 落盘 → 重发帧。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"screens-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const settings_path = state_dir.path() + L"\\settings.ini";
    std::string const settings_bytes =
        "[taskbar]\r\ncomponent=true\r\nscreens=\\\\?\\DISPLAY#TESTA#1#{guid}\r\n";
    {
        HANDLE const file = ::CreateFileW(settings_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        CHECK(::WriteFile(file, settings_bytes.data(),
                          static_cast<DWORD>(settings_bytes.size()), &written, nullptr) != FALSE);
        CHECK_EQ(written, static_cast<DWORD>(settings_bytes.size()));
        ::CloseHandle(file);
    }

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/ok"), L"--poll-ms", L"1000"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    Frame frame;
    bool got = false;
    for (int attempt = 0; attempt < 300 && !got; ++attempt) {
        if (consumer.read(frame) && frame.kind == ViewKind::Content) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got);
    CHECK(frame.screens_configured);
    CHECK_EQ(frame.screens.size(), static_cast<std::size_t>(1));
    CHECK_EQ(frame.screens[0], std::string("\\\\?\\DISPLAY#TESTA#1#{guid}"));

    // 控制通道改成另一块屏幕：主进程必须落盘，并重发一帧（health revision 没变）。
    std::wstring const class_name = scoped_name(kControlWindowClassBase, scope);
    HWND const window = control_find_window(class_name);
    CHECK(window != nullptr);
    ControlRequest request;
    request.op = ControlOp::SetScreens;
    request.sender_pid = ::GetCurrentProcessId();
    request.args = {L"\\\\?\\DISPLAY#TESTB#2#{guid}"};
    ControlReply reply = ControlReply::BadRequest;
    CHECK(control_send_window(window, request, 3000, reply, error));
    CHECK(reply == ControlReply::Accepted);

    bool updated = false;
    for (int attempt = 0; attempt < 300 && !updated; ++attempt) {
        if (consumer.read(frame) && frame.screens_configured && frame.screens.size() == 1 &&
            frame.screens[0] == "\\\\?\\DISPLAY#TESTB#2#{guid}") {
            updated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(updated);

    SettingsFile disk(settings_path);
    std::wstring load_error;
    CHECK(disk.load(load_error));
    std::wstring value;
    CHECK(disk.get(L"taskbar", L"screens", value));
    CHECK_EQ(value, std::wstring(L"\\\\?\\DISPLAY#TESTB#2#{guid}"));

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_publishes_expiry_into_the_mailbox_before_the_next_poll) {
    // 到期是**主进程按绝对时间独立判定**的：一份很快到期的快照被接纳后，显示面必须在到期
    // 那一刻看到整体失效，而不是等下一次轮询把主循环叫醒。
    //
    // 这条用例刻意不查 --inspect：那会往控制窗口投一条消息，等于替主循环“帮它把状态转换泵出来”，
    // 正好掩盖了这里要抓的缺陷。主循环只被到期自己唤醒。
    //
    // 抓的是发布时机，不是 HealthState 的内部标志：帧从邮箱里读出来，读端只认自己看到的那一帧。
    // 轮询给 30 s，所以“下一次轮询”远在到期之后——旧实现（只在循环末尾发布）要等它才会把
    // 到期发布出去，本用例的 1.5 s 上限会立刻失败。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mbexpire-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/expires-soon"), L"--poll-ms",
                        L"30000", L"--timeout-ms", L"10000"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    long long const content_produced = wait_for_content(consumer, 15000);
    CHECK(content_produced > 0);  // 首份快照已经被接纳：显示面看到的是有值的内容帧

    Frame content;
    CHECK(consumer.read(content));
    CHECK(content.kind == ViewKind::Content);
    CHECK(!content.alarm);  // 收到时还没到期：这一帧是正常色

    long long const alarm_produced = wait_for_alarm(consumer, 4000);
    CHECK(alarm_produced > 0);  // 到期必须被发布出来，而不是只在内存里翻了标志
    CHECK(alarm_produced - content_produced <= 1500);

    Frame alarm;
    CHECK(consumer.read(alarm));
    CHECK(frame_shows_alarm(alarm));
    CHECK_EQ(alarm.items.size(), content.items.size());  // 有效期到期不清内容：保留值、整体染红

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));

    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_publishes_the_fetch_timeout_into_the_mailbox_before_the_retry) {
    // 整次取数的墙钟截止由主进程守。这里验的是截止失败之后**有界**地到达显示面，以及
    // “从没取到过数 ⇒ 红色 ERROR + timeout”这一条语义。
    //
    // 说清楚它的边界：这条用例**不**区分“阻塞等待之前发布”与“循环末尾发布”。挂在目标进程里的
    // 取数线程结束时会让主循环的 MsgWaitForMultipleObjects 立刻返回，两次发布因此落在同一轮里。
    // 真正能抓住发布时机的是那条到期用例（到期之后没有别的句柄会叫醒主循环）。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mbtimeout-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/hang"), L"--poll-ms", L"30000",
                        L"--retry-ms", L"10000", L"--timeout-ms", L"1500"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    // 启动时那帧是 waiting（没有配置好之前不露空窗）；挂着的请求到点之后必须变成整体失效。
    // 计时全部落在帧自己的墙钟域（produced_at_ms），不拿单调时钟去减：两者不能换算。
    long long const started_wall = unix_ms();
    long long const alarm_produced = wait_for_alarm(consumer, 6000);
    CHECK(alarm_produced > 0);
    // 15 ms 之内起的子进程、1500 ms 预算、再加上进程启动与用例轮询的余量。
    if (alarm_produced - started_wall > 5000) {
        testing::report_failure("timeout alarm took " + std::to_string(alarm_produced - started_wall) +
                                " ms to reach the mailbox (budget was 1500 ms)");
    }

    Frame alarm;
    CHECK(consumer.read(alarm));
    CHECK(alarm.kind == ViewKind::Error);  // 从没取到过数：没有可保留的值，显示红 ERROR
    CHECK(alarm.failure == FailureKind::Timeout);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));

    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_keeps_publishing_during_a_nested_modal_dispatch) {
    // 右键菜单的 TrackPopupMenu 会在主进程线程上开一个**嵌套模态循环**：主循环那一次
    // PeekMessage 在这期间根本跑不到。这条用例就把那个情形复现出来——自己起一个模态派发循环，
    // 持续把消息派发给主进程的控制窗口。
    //
    // 要证两件事，都是 W4 里真实存在的缺陷：
    //   1) 取数结果不能丢。旧实现是 PostMessage 一个 new 出来的指针，模态循环里落到
    //      DefWindowProc 上就被静默吞掉（结果丢失 + 内存泄漏）。现在结果在有界队列里，
    //      窗口过程/心跳把它取走，所以“有值的内容帧”必须仍然出现。
    //   2) 到期不能被推迟。模态期间主循环不转，只有心跳那条 WM_TIMER 还能推进状态机；
    //      所以整体失效必须在有界时间内被发布出来。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mbmodal-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/expires-soon"), L"--poll-ms",
                        L"30000", L"--timeout-ms", L"10000"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    // 模态派发循环：2 秒，每次取一条消息就派发给主进程的控制窗口（TrackPopupMenu 内部
    // 就是这种节奏）。取数结果与心跳都在这个窗口里被处理。
    long long const started_wall = unix_ms();
    long long const modal_deadline = tick_ms() + 2000;
    long long content_produced = 0;
    long long alarm_produced = 0;
    while (tick_ms() < modal_deadline) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        Frame frame;
        if (consumer.read(frame)) {
            if (content_produced == 0 && frame.kind == ViewKind::Content && !frame.items.empty() &&
                !frame.alarm) {
                content_produced = produced_at_ms(frame);
            }
            if (alarm_produced == 0 && frame_shows_alarm(frame)) {
                alarm_produced = produced_at_ms(frame);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 结果没丢：模态期间仍然拿到了内容帧。
    CHECK(content_produced > 0);
    // 到期没被推迟：整体失效在有界时间内发布出来了（模态循环只有 2 秒，之后主循环恢复，
    // 所以这条断言真正证明的是“模态期间也在推进”，而不是“模态结束后补上”）。
    CHECK(alarm_produced > 0);
    CHECK(alarm_produced - content_produced <= 2500);
    CHECK(alarm_produced - started_wall <= 4000);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));

    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_publishes_a_start_job_failure_into_the_mailbox_before_the_wait) {
    // start_job() 自己也会当场改显示模型：CreateThread 失败时它立刻 apply_failure(Internal)。
    // 这次改动同样必须在阻塞等待**之前**发布出去，否则显示面会继续显示 waiting，直到下一个
    // 轮询点或别的消息把主循环叫醒。
    //
    // 失败是注入的：真实 CreateThread 几乎不会失败，所以这里用只在隔离实例里生效的开关
    // （scope 非空 + DESKFEED_TEST_FAIL_FETCH_THREAD=1）让 start_job() 走**同一条**收尾路径。
    // 生产实例的 scope 恒为空，这个开关在生产上打不开。
    //
    // 重试给 10 s：一旦这次失败没有被及时发布，下一件能叫醒主循环的事就是 10 s 之后的重试，
    // 用例的 3 s 上限会立刻失败。所以这条用例真的咬住“发布在阻塞等待之前”。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mbfailthread-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    {
        ScopedEnvironmentVariable const failpoint(L"DESKFEED_TEST_FAIL_FETCH_THREAD", L"1");
        CHECK(master.start(exe,
                           {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                            L"--frame-mailbox", L"--url", server.url(L"/ok"), L"--poll-ms", L"1000",
                            L"--retry-ms", L"10000", L"--timeout-ms", L"3000"},
                           error));
    }
    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!attached) {
        bool const exited = master.wait(0);
        testing::report_failure("attach failed: " + error + "; master exited=" +
                                (exited ? "yes" : "no") + "; map=" +
                                utf8_from_wide(display_frame_map_name(scope)));
    }
    CHECK(attached);

    long long const started_wall = unix_ms();
    long long const alarm_produced = wait_for_alarm(consumer, 6000);
    if (alarm_produced <= 0) {
        bool const exited = master.wait(0);
        testing::report_failure("no alarm frame in 6 s; master exited=" +
                                std::string(exited ? "yes" : "no") + "; producer_running=" +
                                std::string(consumer.producer_running() ? "yes" : "no"));
    }
    if (alarm_produced - started_wall > 3000) {
        testing::report_failure("start_job failure took " +
                                std::to_string(alarm_produced - started_wall) +
                                " ms to reach the mailbox (retry was 10000 ms)");
    }

    Frame alarm;
    CHECK(consumer.read(alarm));
    CHECK(alarm.kind == ViewKind::Error);
    CHECK(alarm.failure == FailureKind::Internal);
    CHECK(alarm.reason.find("无法启动取数线程") != std::string::npos);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));

    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

// ---- “先仲裁后副作用”与显示面的建立时机 --------------------------------------
//
// 契约要求：首份命令被拒绝时，这个进程除了一个控制窗口和一行日志之外什么都不该留下。
// 显示面（显示邮箱、悬浮窗、任务栏编排）只在首份状态**被接受之后**才建立
// （见 Master::start_display_face）。下面几条就是这条时序的门禁：全部在 --headless 下跑，
// 不建窗口、不注入、绝不碰生产命名空间。

TEST(master_rejects_an_invalid_first_command_without_creating_a_display_face) {
    std::string error;
    std::wstring const scope = L"noeffect-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    // ftp:// 不是 http/https：apply_command 的第一步（地址校验）就拒绝整条命令。
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", L"ftp://example.invalid/feed"},
                       error));
    CHECK(master.wait(15000));
    CHECK_EQ(master.exit_code(), static_cast<long long>(6));

    // 命令被拒绝 ⇒ 显示邮箱一个都不该建。显式要求了 --frame-mailbox 也没用：拒绝发生在
    // 建显示面之前。这条断言就是修掉“先建邮箱、再校验首份命令”那个副作用窗口的可验证形式。
    DisplayFrameConsumer consumer;
    CHECK(!consumer.attach(scope, error));

    // 也不该碰磁盘：整条命令被拒，设置文件必须原样不存在。
    CHECK(::GetFileAttributesW((state_dir.path() + L"\\settings.ini").c_str()) ==
          INVALID_FILE_ATTRIBUTES);

    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_starts_from_the_saved_url_without_arguments) {
    // 无参数启动：用保存的地址独立运行。它同样要在**状态被接受之后**才建显示面，
    // 而且要有真实读数——这是“缓存下来的地址也能起一份可用的显示面”的门禁。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"cachedurl-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const settings_path = state_dir.path() + L"\\settings.ini";
    std::string const settings = "[source]\r\nurl=" + utf8_from_wide(server.url(L"/ok")) +
                                 "\r\npoll_ms=1000\r\n";
    std::wstring file_error;
    CHECK(write_file_bytes(settings_path, settings, file_error));

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    Frame frame;
    bool got_content = false;
    for (int attempt = 0; attempt < 300 && !got_content; ++attempt) {
        if (consumer.read(frame) && frame.kind == ViewKind::Content && !frame.items.empty()) {
            got_content = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got_content);
    CHECK_EQ(frame.items[0].id, std::string("cpu"));

    // 保存的地址没有被改写（无参数启动不该动配置）。
    std::string written;
    bool exists = false;
    CHECK(read_file_bytes(settings_path, written, exists, file_error));
    CHECK(exists);
    CHECK(written.find(utf8_from_wide(server.url(L"/ok"))) != std::string::npos);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_headless_without_the_flag_creates_no_display_face) {
    // --headless 是“本次运行不碰界面、不碰任务栏”。它**不**建显示邮箱：没有负载会去读它，
    // 建一块生产命名的共享内存只是白留副作用。要这条 seam 必须显式 --frame-mailbox。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"headlessface-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--url", server.url(L"/ok"), L"--poll-ms", L"1000"},
                       error));

    // 给它一段真实运行时间：如果它会建邮箱，这段时间足够建起来了。
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    DisplayFrameConsumer consumer;
    CHECK(!consumer.attach(scope, error));

    // 但没有显示面不等于没在运行：实例照常取数、照常应答控制通道。
    CHECK_EQ(master.exit_code(), static_cast<long long>(-1));  // 还在跑
    ChildProcess inspect;
    CHECK(inspect.start(exe, {L"--scope", scope, L"--inspect"}, error));
    CHECK(inspect.wait(15000));
    CHECK_EQ(inspect.exit_code(), static_cast<long long>(0));

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_reports_a_fetch_result_that_could_not_be_queued) {
    // 工作线程拿不到结果通道时（队列拒绝 / 已经停止接受 / 线程内异常），主进程必须**立刻**
    // 按内部失败收尾，而不是干等整次取数的墙钟预算。注入点只在隔离实例里生效。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"noqueue-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    ScopedEnvironmentVariable const refuse_outcome(L"DESKFEED_TEST_REFUSE_OUTCOME", L"1");
    ChildProcess master;
    // 轮询与预算都放大到 30 s：这样“帧很快就变成整体失效”的原因只能是入队失败这条路，
    // 不可能是预算到点或正常轮询结果。
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", server.url(L"/ok"), L"--poll-ms", L"30000",
                        L"--timeout-ms", L"30000"},
                       error));

    DisplayFrameConsumer consumer;
    bool attached = false;
    for (int attempt = 0; attempt < 300 && !attached; ++attempt) {
        if (consumer.attach(scope, error)) {
            attached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(attached);

    Frame frame;
    bool got_failure = false;
    long long const deadline = tick_ms() + 6000;
    while (tick_ms() < deadline) {
        if (consumer.read(frame) && frame.kind == ViewKind::Error &&
            frame.failure == FailureKind::Internal) {
            got_failure = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got_failure);
    CHECK(frame.kind == ViewKind::Error);
    CHECK(frame.failure == FailureKind::Internal);
    CHECK(frame.reason.find("取数结果没能交回主进程") != std::string::npos);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    consumer.detach();
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_refuses_the_explicit_mailbox_when_the_ready_name_is_taken) {
    // 显式要求的显示邮箱建不起来时，必须**当场失败**：静默继续会让离线验收“跑通了但什么也没验”。
    // 这里不用任何测试注入点——用一个名字冲突的真实内核对象把它逼出来：Windows 的命名对象共用
    // 一个命名空间，CreateEventW 遇到同名但**类型不同**的对象会失败，而生产者是先建就绪事件、
    // 再建共享内存（见 display/mailbox.cpp），所以占住就绪事件的名字就足够。
    std::string error;
    std::wstring const scope = L"mboxclash-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));

    std::wstring const ready_name = deskfeed::display_frame_ready_name(scope);
    HANDLE const blocker = ::CreateMutexW(nullptr, FALSE, ready_name.c_str());
    CHECK(blocker != nullptr);

    std::wstring const exe = master_executable_path();
    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--frame-mailbox", L"--url", L"http://127.0.0.1:1/never"},
                       error));
    CHECK(master.wait(15000));
    CHECK_EQ(master.exit_code(), static_cast<long long>(7));

    // 失败路径也不能留下半个显示面。
    DisplayFrameConsumer consumer;
    CHECK(!consumer.attach(scope, error));

    ::CloseHandle(blocker);
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_without_a_display_mailbox_never_touches_the_injector) {
    // 生产路径的“显示邮箱建不起来 ⇒ 一个注入动作都不发”端到端用例。
    //
    // 邮箱失败是**真的**制造出来的：就绪事件名被一个异类型内核对象占住（Windows 命名对象共用
    // 一个命名空间）。生产者先建就绪事件、再建共享内存，所以占住它就足够，不需要任何测试注入点。
    //
    // 用 --hidden（**不是** --headless）：这样 taskbar_wanted 为真，才会真的去建生产邮箱；
    // 窗口建出来但从不显示，所以不会出现可见窗口或光标。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"mboxmissing-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    IsolatedRunDirectory run_dir;
    CHECK(run_dir.create(error));

    // 把主进程与假注入器复制到同一个隔离目录：编排层按“与主进程同目录的 deskfeed-inject.exe”
    // 找注入器，所以监视器必须与那份副本同目录，绝不能占用构建树里真正的注入器。
    std::wstring const copied_master = run_dir.path() + L"\\deskfeed.exe";
    std::wstring const copied_injector = run_dir.path() + L"\\deskfeed-inject.exe";
    std::wstring copy_error;
    CHECK(copy_file(master_executable_path(), copied_master, copy_error));
    if (!copy_file(fake_injector_path(), copied_injector, copy_error)) {
        testing::report_failure(
            "复制假注入器失败（deskfeed-fake-injector 是否已经构建？）：" +
            testing::to_display(copy_error));
    }
    std::wstring const spy_log = run_dir.path() + L"\\fake-injector.log";
    ScopedEnvironmentVariable const spy_log_env(L"DESKFEED_FAKE_LOG", spy_log);

    std::wstring const ready_name = deskfeed::display_frame_ready_name(scope);
    HANDLE const blocker = ::CreateMutexW(nullptr, FALSE, ready_name.c_str());
    CHECK(blocker != nullptr);

    ChildProcess master;
    CHECK(master.start(copied_master,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--hidden",
                        L"--url", server.url(L"/ok"), L"--poll-ms", L"1000"},
                       error));
    CHECK(wait_for_control_window(scope, 10000));

    // 悬浮窗真的建出来了（--hidden 只控制显示，不阻止建窗），而且不可见。
    std::wstring const widget_class = scoped_name(L"DeskFeedWidget", scope);
    HWND widget = nullptr;
    for (int attempt = 0; attempt < 300 && widget == nullptr; ++attempt) {
        widget = ::FindWindowW(widget_class.c_str(), nullptr);
        if (widget == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    CHECK(widget != nullptr);
    CHECK(!::IsWindowVisible(widget));

    // 取数照常：邮箱失败只影响任务栏那条显示面，不拦取数。
    std::wstring const log_path = deskfeed_log_path(scope);
    check_log_marker(log_path, L"fetch: 成功", 8000);

    // 生产分支确实走到了（不是“碰巧没建邮箱”）。
    check_log_marker(log_path, L"没有显示邮箱", 5000);

    // 实例仍在运行、还能应答控制通道。
    CHECK_EQ(master.exit_code(), -1LL);
    ChildProcess inspect;
    CHECK(inspect.start(copied_master, {L"--scope", scope, L"--inspect"}, error));
    CHECK(inspect.wait(15000));
    CHECK_EQ(inspect.exit_code(), 0LL);

    // 一个注入动作都没有：假注入器一次都没被 spawn（它连日志文件都没建出来）。
    std::string spy_bytes;
    bool spy_exists = false;
    CHECK(read_file_if_present(spy_log, spy_bytes, spy_exists));
    CHECK(!spy_exists);

    ChildProcess quit;
    CHECK(quit.start(copied_master, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));

    ::CloseHandle(blocker);
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_keeps_the_new_binding_when_a_conditional_quit_is_still_pending) {
    // `--quit-if-bound` 只应退出“当时那个绑定”。判定发生在窗口过程里、动作却要等消息被消费，
    // 中间可以有一次新接管。这里用测试注入点把**已投递**的退出消息延迟消费（只对 scope + headless
    // 生效），在“已投递、尚未生效”的确定窗口里提交一次新接管：
    //   * 条件退出此时**还没有**置 closing_，所以新接管必须被接受；
    //   * 到期的条件退出必须被丢弃，实例继续服务新绑定（契约：被新生产者接管则不退出）。
    // 去掉“退出消息带代次 + 消费时重核验”后，这条用例会红：实例会退出，或丢弃日志不会出现。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"quitpend-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    DummyProducer first;
    CHECK(first.start(error));
    DummyProducer second;
    CHECK(second.start(error));

    ChildProcess master;
    {
        ScopedEnvironmentVariable const defer(L"DESKFEED_TEST_DEFER_QUIT_MS", L"4000");
        CHECK(master.start(exe,
                           {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                            L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                            L"10000"},
                           error));
    }
    CHECK(wait_for_control_window(scope, 10000));

    std::wstring const log_path = deskfeed_log_path(scope);

    ChildProcess bind;
    CHECK(bind.start(exe,
                     {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                      std::to_wstring(first.pid())},
                     error));
    CHECK(bind.wait(15000));
    CHECK_EQ(bind.exit_code(), 0LL);
    check_log_marker(log_path, L"绑定生产者 PID", 5000);

    ChildProcess quit_if_bound;
    CHECK(quit_if_bound.start(
        exe, {L"--scope", scope, L"--quit-if-bound", std::to_wstring(first.pid())}, error));
    CHECK(quit_if_bound.wait(15000));
    CHECK_EQ(quit_if_bound.exit_code(), 0LL);
    // 同步点：主进程已经把这条条件退出投递进延迟队列。
    check_log_marker(log_path, L"延迟消费退出消息", 5000);

    ChildProcess takeover;
    CHECK(takeover.start(exe,
                         {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                          std::to_wstring(second.pid())},
                         error));
    CHECK(takeover.wait(15000));
    CHECK_EQ(takeover.exit_code(), 0LL);

    // 等过延迟窗口：到期的条件退出必须被丢弃，实例还活着。
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    CHECK_EQ(master.exit_code(), -1LL);
    check_log_marker(log_path, L"丢弃已被新绑定取代的条件退出", 5000);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_keeps_the_new_binding_when_the_old_producer_exit_is_still_pending) {
    // 旧生产者退出走的是同一条条件退出路径（on_binding_exit），也必须能被随后到达的新接管取消。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"bindingpend-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    DummyProducer first;
    CHECK(first.start(error));
    DummyProducer second;
    CHECK(second.start(error));

    ChildProcess master;
    {
        ScopedEnvironmentVariable const defer(L"DESKFEED_TEST_DEFER_QUIT_MS", L"4000");
        CHECK(master.start(exe,
                           {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                            L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                            L"10000"},
                           error));
    }
    CHECK(wait_for_control_window(scope, 10000));

    std::wstring const log_path = deskfeed_log_path(scope);

    ChildProcess bind;
    CHECK(bind.start(exe,
                     {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                      std::to_wstring(first.pid())},
                     error));
    CHECK(bind.wait(15000));
    CHECK_EQ(bind.exit_code(), 0LL);
    check_log_marker(log_path, L"绑定生产者 PID", 5000);

    // 旧生产者退出：等待线程投递退出回调，主进程据此投递一条**带代次**的退出消息（被延迟消费）。
    first.kill();
    check_log_marker(log_path, L"延迟消费退出消息", 8000);

    ChildProcess takeover;
    CHECK(takeover.start(exe,
                         {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                          std::to_wstring(second.pid())},
                         error));
    CHECK(takeover.wait(15000));
    CHECK_EQ(takeover.exit_code(), 0LL);

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    CHECK_EQ(master.exit_code(), -1LL);
    check_log_marker(log_path, L"丢弃已被新绑定取代的条件退出", 5000);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_rejects_a_takeover_after_an_unconditional_quit_was_queued) {
    // 无条件退出（`--quit`）在**返回接受之前**就置 closing_：随后到达的接管必须被拒（exit 6），
    // 不能拿到一个“已接受”的假确认、然后实例马上消失。条件退出是另一种语义（允许被新绑定取消），
    // 这里验的就是两者的区分。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"closingguard-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    DummyProducer producer;
    CHECK(producer.start(error));

    ChildProcess master;
    {
        ScopedEnvironmentVariable const defer(L"DESKFEED_TEST_DEFER_QUIT_MS", L"4000");
        CHECK(master.start(exe,
                           {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                            L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                            L"10000"},
                           error));
    }
    CHECK(wait_for_control_window(scope, 10000));
    std::wstring const log_path = deskfeed_log_path(scope);

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK_EQ(quit.exit_code(), 0LL);

    // 退出已排定（消息被延迟消费，但 closing_ 在确认之前就置上了）。
    ChildProcess takeover;
    CHECK(takeover.start(exe,
                         {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                          std::to_wstring(producer.pid())},
                         error));
    CHECK(takeover.wait(15000));
    CHECK_EQ(takeover.exit_code(), 6LL);
    check_log_marker(log_path, L"拒绝新的接管", 5000);

    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_waits_for_the_actual_exit_only_when_asked) {
    // `--wait-exit-ms`：确认被接受后还要等到实例进程**真的退出**。用延迟注入点把“退出被消费”
    // 推迟约 3 s：带等待的客户端必须活到那之后才返回；不带等待（缺省 0）时只等 IPC 确认。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"waitexit-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    ChildProcess master;
    {
        ScopedEnvironmentVariable const defer(L"DESKFEED_TEST_DEFER_QUIT_MS", L"3000");
        CHECK(master.start(exe,
                           {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                            L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                            L"10000"},
                           error));
    }
    CHECK(wait_for_control_window(scope, 10000));

    ChildProcess quit;
    long long const started = tick_ms();
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit", L"--wait-exit-ms", L"10000"}, error));
    CHECK(quit.wait(20000));
    long long const elapsed = tick_ms() - started;
    CHECK_EQ(quit.exit_code(), 0LL);
    // 退出被延迟了 3 s 才被消费，所以“等到实例真的退出”的客户端至少也要活到那之后。
    CHECK(elapsed >= 2500);
    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_does_not_wait_when_a_conditional_quit_does_not_match) {
    // 没有匹配的绑定：`--quit-if-bound` 报 NotBound（成功返回、什么都不做）。即使给了很大的
    // `--wait-exit-ms`，也必须立刻返回，绝不能错误地等一个根本不会退出的实例。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"notboundwait-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                        L"10000"},
                       error));
    CHECK(wait_for_control_window(scope, 10000));

    ChildProcess not_bound;
    long long const started = tick_ms();
    CHECK(not_bound.start(
        exe, {L"--scope", scope, L"--quit-if-bound", L"999999", L"--wait-exit-ms", L"60000"},
        error));
    CHECK(not_bound.wait(10000));
    long long const elapsed = tick_ms() - started;
    CHECK_EQ(not_bound.exit_code(), 0LL);
    CHECK(elapsed < 5000);
    CHECK_EQ(master.exit_code(), -1LL);  // 实例不受影响

    ChildProcess quit;
    CHECK(quit.start(exe, {L"--scope", scope, L"--quit"}, error));
    CHECK(quit.wait(15000));
    CHECK(master.wait(15000));
    cleanup_isolated_instance(scope, state_dir.path());
}

TEST(master_does_not_send_a_quit_it_cannot_confirm) {
    // `--wait-exit-ms` 的核心纪律：**只有**真的拿到实例进程的 SYNCHRONIZE 句柄、并等它真的
    // 触发过，才返回 0。拿不到句柄时 fail closed：返回 5（不确定）并且**不发送 IPC**——
    // 绝不能退化成“只等确认的 0”，那会让调用方把 ACK 当成“实例已经退出”。
    //
    // 句柄拿不到这件事用隔离实例的注入点确定性地制造出来（模拟 OpenProcess 失败）。
    LoopbackServer server;
    std::string error;
    CHECK(server.start(error));

    std::wstring const scope = L"failclosed-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                               std::to_wstring(++g_temp_file_counter);
    TempDirectory state_dir;
    CHECK(state_dir.create(error));
    std::wstring const exe = master_executable_path();

    DummyProducer producer;
    CHECK(producer.start(error));

    ChildProcess master;
    CHECK(master.start(exe,
                       {L"--scope", scope, L"--state-dir", state_dir.path(), L"--headless",
                        L"--url", server.url(L"/ok"), L"--poll-ms", L"30000", L"--timeout-ms",
                        L"10000"},
                       error));
    CHECK(wait_for_control_window(scope, 10000));

    std::wstring const log_path = deskfeed_log_path(scope);

    ChildProcess bind;
    CHECK(bind.start(exe,
                     {L"--scope", scope, L"--url", server.url(L"/ok"), L"--pid",
                      std::to_wstring(producer.pid())},
                     error));
    CHECK(bind.wait(15000));
    CHECK_EQ(bind.exit_code(), 0LL);
    check_log_marker(log_path, L"绑定生产者 PID", 5000);

    // 拿不到句柄：必须 exit 5，而且一个字节的 IPC 都没发出去（实例不受影响、还绑着）。
    {
        ScopedEnvironmentVariable const no_handle(L"DESKFEED_TEST_FAIL_WAIT_HANDLE", L"1");
        ChildProcess blocked;
        CHECK(blocked.start(exe,
                            {L"--scope", scope, L"--quit-if-bound",
                             std::to_wstring(producer.pid()), L"--wait-exit-ms", L"60000"},
                            error));
        CHECK(blocked.wait(15000));
        CHECK_EQ(blocked.exit_code(), 5LL);
    }
    CHECK_EQ(master.exit_code(), -1LL);  // 实例没有被“已接受然后等着”的假路径影响

    // 对照组：能拿到句柄时同一条命令被接受，并且等到实例真的退出才返回 0。
    ChildProcess confirmed;
    CHECK(confirmed.start(exe,
                          {L"--scope", scope, L"--quit-if-bound",
                           std::to_wstring(producer.pid()), L"--wait-exit-ms", L"60000"},
                          error));
    CHECK(confirmed.wait(20000));
    CHECK_EQ(confirmed.exit_code(), 0LL);
    CHECK(master.wait(15000));  // 实例真的退出了

    cleanup_isolated_instance(scope, state_dir.path());
}

// ---- 主入口 ----------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    // 子模式：本程序被自己再起一份，充当“另一个生产者进程”（见显示邮箱的身份用例）。
    // 必须在跑用例之前返回，否则子进程会把整套用例再递归跑一遍。
    if (argc >= 3 && std::wstring(argv[1]) == L"--mailbox-owner") {
        std::wstring const scope = argv[2];
        unsigned hold_ms = 30000;
        for (int index = 3; index + 1 < argc; ++index) {
            if (std::wstring(argv[index]) == L"--hold-ms") {
                hold_ms = static_cast<unsigned>(std::wcstoul(argv[index + 1], nullptr, 10));
            }
        }
        return mailbox_owner_child(scope, hold_ms);
    }

    std::string filter;
    for (int index = 1; index < argc; ++index) {
        std::wstring const arg(argv[index]);
        if (arg == L"--filter" && index + 1 < argc) {
            filter = utf8_from_wide(argv[++index]);
        }
    }

    int passed = 0;
    int failed = 0;
    for (testing::Case const& test : testing::cases()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) {
            continue;
        }
        try {
            test.body();
            ++passed;
            console_write_stdout(std::string("ok   ") + test.name + "\n");
        } catch (testing::Failure const& failure) {
            ++failed;
            console_write_stdout(std::string("FAIL ") + test.name + ": " + failure.message + "\n");
        } catch (std::exception const& error) {
            ++failed;
            console_write_stdout(std::string("FAIL ") + test.name + ": exception " + error.what() +
                                 "\n");
        }
    }
    console_write_stdout(std::to_string(passed) + " passed, " + std::to_string(failed) + " failed\n");
    return failed == 0 ? 0 : 1;
}
