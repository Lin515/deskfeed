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

TEST(frame_roundtrip_preserves_every_field) {
    Frame const frame = sample_frame();
    std::vector<unsigned char> payload;
    CHECK(frame_encode(frame, payload));
    CHECK(payload.size() <= kFramePayloadCapacity);

    Frame decoded;
    CHECK(frame_decode(payload.data(), payload.size(), decoded));
    CHECK(decoded.kind == ViewKind::Content);
    CHECK(decoded.alarm);
    CHECK(decoded.failure == FailureKind::Timeout);
    CHECK_EQ(decoded.reason, frame.reason);
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
    CHECK(!frame_decode(bad_kind.data(), bad_kind.size(), out));

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

// ---- 主入口 ----------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
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
