// deskfeed-tools-tests.exe —— 注入器与探针工具的**离线**用例。
//
// 覆盖的都是"不需要 explorer、不需要负载 DLL、不需要 XAML"的那部分：
//   * 严格 PE 解析：合成的坏文件 fixtures 必须被逐项拒绝（0 入口、越出 image、导出的是数据、
//     目录越界、PE32/x86、转发导出、名字不落在本节里的）。
//   * 卸载/装载门禁的纯决策表（超时不释放远程缓冲、线程没退出就不算装载成功）。
//   * 完整路径匹配：同名不同目录**不算**命中。
//   * 实例名白名单。
//   * 显示量具的纯排版口径（与窗口/任务栏同一份 display/reading）。
//   * 显示帧线格式的编解码与**逐项拒绝**（版本/尺寸/枚举/边界/尾巴）。
//   * 显示邮箱的收发一轮（隔离 scope，自己起生产者与消费者），以及"只读最新一帧、清空不复活
//     旧条目、所有者退出后拒绝交出旧帧"这三条语义。
//   * 探针宿主的 feature gate：缺必需导出必须 BLOCKED（不是"日志里说一句就跳过"）。
//
// 刻意不用 tests/test_support.h：那一份由 tests.cpp 定义符号，而这个可执行文件要保持独立，
// 免得两个用例集合链接到一起时符号打架（tests/native_tests.cpp 也是自带一套极简框架）。
#include "display/frame.h"
#include "display/mailbox.h"
#include "display/reading.h"
#include "inject/actions.h"
#include "inject/guard.h"
#include "inject/leased_call.h"
#include "inject/payload_status.h"
#include "inject/pe_export.h"
#include "inject/remote.h"
#include "inject/target.h"
#include "platform/paths.h"
#include "platform/win32_util.h"
#include "probe/host_gate.h"
#include "probe/host_wait.h"
#include "probe/mock_producer.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_filter;

void check(bool condition, char const* what) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("FAIL %s\n", what);
}

template <class A, class E>
void check_eq(A const& actual, E const& expected, char const* what) {
    ++g_checks;
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::ostringstream os;
    os << "FAIL " << what << " (" << actual << " vs " << expected << ")";
    std::printf("%s\n", os.str().c_str());
}

bool wanted(char const* name) {
    return g_filter.empty() || std::strstr(name, g_filter.c_str()) != nullptr;
}

std::wstring widen(char const* text) {
    std::wstring result;
    for (; text != nullptr && *text != '\0'; ++text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return result;
}

std::wstring temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    DWORD const length = ::GetTempPathW(MAX_PATH, buffer);
    std::wstring dir(buffer, length);
    dir += L"deskfeed-tools-tests-" + std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool write_file(std::wstring const& path, std::vector<unsigned char> const& bytes) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool const ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr) != FALSE &&
                    written == bytes.size();
    ::CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------------------
// 合成 PE：一个最小但**结构正确**的 AMD64 镜像，`.text` 可执行、`.rdata` 只读数据。
// 每个坏文件 fixture 只改一处，好让"拒绝"能精确归因。
// ---------------------------------------------------------------------------

struct PeKnobs {
    unsigned short machine = 0x8664;      // IMAGE_FILE_MACHINE_AMD64
    bool pe32 = false;                    // true = PE32 可选头（应当被拒）
    unsigned long size_of_image = 0x3000;
    unsigned long number_of_directories = 16;
    unsigned long export_dir_rva = 0x2000;
    unsigned long export_dir_size = 0x180;
    bool zero_entry = false;              // "NoEntry" 的函数 RVA 写 0
    bool outside_image = false;           // "OutsideImage" 越出 SizeOfImage
    bool data_export = false;             // "DataExport" 落在不可执行的 .rdata
    bool forwarded = false;               // "Forwarded" 落在导出目录内
    bool unterminated_name = false;       // "Probe" 的名字串不在本节里收尾
    unsigned long time_date_stamp = 0;    // COFF TimeDateStamp（换 build 的区分位）
};

// 列布局（都在 .rdata 的原始数据里）：
//   0x600 导出目录(40)  0x628 函数表  0x640 名字表  0x660 序号表  0x680 名字串  0x700 dll 名
std::vector<unsigned char> build_pe(PeKnobs const& knobs) {
    constexpr unsigned long kFileSize = 0xA00;
    constexpr unsigned long kRawText = 0x200;
    constexpr unsigned long kRawRdata = 0x600;
    constexpr unsigned long kTextRva = 0x1000;
    constexpr unsigned long kRdataRva = 0x2000;
    constexpr unsigned long kFunctionsRva = kRdataRva + 0x28;
    constexpr unsigned long kNamesRva = kRdataRva + 0x40;
    constexpr unsigned long kOrdinalsRva = kRdataRva + 0x60;
    constexpr unsigned long kNamesBaseRva = kRdataRva + 0x80;
    constexpr unsigned long kDllNameRva = kRdataRva + 0x100;

    std::vector<unsigned char> file(kFileSize, 0);
    auto put16 = [&file](std::size_t at, unsigned short value) {
        std::memcpy(file.data() + at, &value, sizeof(value));
    };
    auto put32 = [&file](std::size_t at, unsigned long value) {
        std::memcpy(file.data() + at, &value, sizeof(value));
    };
    auto put_text = [&file](std::size_t at, char const* text) {
        std::memcpy(file.data() + at, text, std::strlen(text));
    };

    // DOS 头
    put16(0, 0x5A4D);
    put32(0x3C, 0x80);

    std::size_t const nt = 0x80;
    std::memcpy(file.data() + nt, "PE\0\0", 4);
    put16(nt + 4, knobs.machine);
    put16(nt + 6, 2);                                  // NumberOfSections
    put32(nt + 8, knobs.time_date_stamp);              // TimeDateStamp
    unsigned short const optional_size = knobs.pe32 ? 0xE0 : 0xF0;
    put16(nt + 4 + 16, optional_size);
    put16(nt + 4 + 18, 0x0022);                        // Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    std::size_t const optional = nt + 4 + 20;
    put16(optional, knobs.pe32 ? 0x10B : 0x20B);
    put32(optional + 32, 0x1000);                      // SectionAlignment
    put32(optional + 36, 0x200);                       // FileAlignment
    put32(optional + 56, knobs.size_of_image);         // SizeOfImage
    put32(optional + 60, 0x200);                       // SizeOfHeaders
    if (knobs.pe32) {
        put32(optional + 92, knobs.number_of_directories);
    } else {
        put32(optional + 108, knobs.number_of_directories);
    }
    // 数据目录第 0 项 = 导出目录（PE32+ 从 112 起，PE32 从 96 起）
    std::size_t const directories = optional + (knobs.pe32 ? 96 : 112);
    put32(directories, knobs.export_dir_rva);
    put32(directories + 4, knobs.export_dir_size);

    // 节表
    std::size_t const sections = optional + optional_size;
    auto section = [&](std::size_t index, char const* name, unsigned long rva, unsigned long vsize,
                       unsigned long raw, unsigned long raw_size, unsigned long characteristics) {
        std::size_t const at = sections + index * 40;
        put_text(at, name);
        put32(at + 8, vsize);
        put32(at + 12, rva);
        put32(at + 16, raw_size);
        put32(at + 20, raw);
        put32(at + 36, characteristics);
    };
    section(0, ".text", kTextRva, 0x1000, kRawText, 0x400, 0x60000020);   // CODE|EXECUTE|READ
    section(1, ".rdata", kRdataRva, 0x1000, kRawRdata, 0x400, 0x40000040);  // DATA|READ

    // 导出目录
    std::size_t const dir = kRawRdata;
    put32(dir + 12, kDllNameRva);   // Name
    put32(dir + 16, 1);             // Base
    put32(dir + 20, 5);             // NumberOfFunctions
    put32(dir + 24, 5);             // NumberOfNames
    put32(dir + 28, kFunctionsRva);
    put32(dir + 32, kNamesRva);
    put32(dir + 36, kOrdinalsRva);

    unsigned long const probe_rva = kTextRva;
    unsigned long const no_entry_rva = knobs.zero_entry ? 0u : kTextRva;
    unsigned long const outside_rva = knobs.outside_image ? 0x5000u : kTextRva;
    // 0x2200 在 .rdata 里、但在导出目录（0x2000..0x2180）之外：它是数据，不是转发。
    unsigned long const data_rva = knobs.data_export ? 0x2200u : kTextRva;
    unsigned long const forwarded_rva = knobs.forwarded ? knobs.export_dir_rva : kTextRva;

    std::size_t const functions = kRawRdata + 0x28;
    unsigned long const function_rvas[5] = {probe_rva, no_entry_rva, outside_rva, data_rva,
                                           forwarded_rva};
    for (int index = 0; index < 5; ++index) {
        put32(functions + index * 4, function_rvas[index]);
    }

    char const* const names[5] = {"Probe", "NoEntry", "OutsideImage", "DataExport", "Forwarded"};
    std::size_t const name_table = kRawRdata + 0x40;
    std::size_t const ordinal_table = kRawRdata + 0x60;
    std::size_t cursor = kRawRdata + 0x80;
    for (int index = 0; index < 5; ++index) {
        if (index == 0 && knobs.unterminated_name) {
            // 把 "Probe" 写在 .rdata 原始数据的最后 5 个字节，且**没有** NUL：
            // 严格解析器只能在它所在那一节里找结尾，于是这个名字读不出来。
            std::size_t const last = kRawRdata + 0x400 - 5;
            std::memcpy(file.data() + last, "Probe", 5);
            put32(name_table, kRdataRva + static_cast<unsigned long>(last - kRawRdata));
        } else {
            put32(name_table + index * 4,
                  kRdataRva + static_cast<unsigned long>(cursor - kRawRdata));
            std::memcpy(file.data() + cursor, names[index], std::strlen(names[index]) + 1);
            cursor += std::strlen(names[index]) + 1;
        }
        put16(ordinal_table + index * 2, static_cast<unsigned short>(index));
    }
    put_text(kRawRdata + 0x100, "fixture.dll");

    // .text 里放一条 ret：内容不重要，重要的是它落在可执行节里。
    file[kRawText] = 0xC3;
    return file;
}

struct PeCase {
    char const* name;
    PeKnobs knobs;
    char const* export_name;
    bool expect_ok;
    deskfeed::inject::PeExportError expect_error;
};

void run_pe_cases(std::wstring const& dir) {
    using namespace deskfeed::inject;
    std::vector<PeCase> const cases = {
        {"pe/accept-executable", {}, "Probe", true, PeExportError::None},
        {"pe/reject-zero-entry", {.zero_entry = true}, "NoEntry", false, PeExportError::NotExecutable},
        {"pe/reject-outside-image", {.outside_image = true}, "OutsideImage", false,
         PeExportError::NotExecutable},
        {"pe/reject-data-export", {.data_export = true}, "DataExport", false,
         PeExportError::NotExecutable},
        {"pe/reject-forwarder", {.forwarded = true}, "Forwarded", false, PeExportError::Forwarded},
        {"pe/reject-pe32", {.machine = 0x14C, .pe32 = true}, "Probe", false,
         PeExportError::UnsupportedMachine},
        {"pe/reject-x86", {.machine = 0x14C}, "Probe", false, PeExportError::UnsupportedMachine},
        {"pe/reject-no-directories", {.number_of_directories = 0}, "Probe", false,
         PeExportError::NoExportDirectory},
        {"pe/reject-directory-overflow", {.export_dir_size = 0xFFFFF000ul}, "Probe", false,
         PeExportError::BadHeader},
        {"pe/reject-unterminated-name", {.unterminated_name = true}, "Probe", false,
         PeExportError::NotFound},
        {"pe/reject-missing-export", {}, "NotExported", false, PeExportError::NotFound},
    };

    for (PeCase const& item : cases) {
        if (!wanted(item.name)) {
            continue;
        }
        std::wstring const path = dir + L"\\" + widen(item.name + 5) + L".dll";
        if (!write_file(path, build_pe(item.knobs))) {
            check(false, "pe fixture write");
            continue;
        }
        PeExportInfo info;
        std::wstring error;
        PeExportError kind = PeExportError::None;
        bool const ok = pe_export_rva(path, item.export_name, info, error, &kind);
        check(ok == item.expect_ok, item.name);
        if (ok != item.expect_ok) {
            std::wprintf(L"  (%hs: %s; error=%s)\n", item.name,
                         ok ? L"unexpectedly accepted" : L"unexpectedly refused", error.c_str());
            continue;
        }
        if (item.expect_ok) {
            check(info.rva == 0x1000, "pe/accept-executable rva");
            check(info.image_size == 0x3000, "pe/accept-executable image size");
            check(info.machine == 0x8664, "pe/accept-executable machine");
        } else {
            if (kind != item.expect_error) {
                std::wprintf(L"  (%hs: rejection kind %hs, expected %hs; error=%s)\n", item.name,
                             deskfeed::inject::pe_export_error_name(kind),
                             deskfeed::inject::pe_export_error_name(item.expect_error),
                             error.c_str());
            }
            check(kind == item.expect_error, item.name);
        }
    }

    // 磁盘镜像与目标模块的交叉核对：尺寸不同 = 不同 build，必须拒绝。
    PeExportInfo const matching{0x1000, 0x3000, 0x8664};
    std::wstring error;
    check(pe_image_matches_module(matching, 0x3000, error), "pe/module-size-matches");
    check(!pe_image_matches_module(matching, 0x4000, error), "pe/module-size-mismatch");
    check(!pe_image_matches_module(matching, 0, error), "pe/module-size-zero");
}

// ---------------------------------------------------------------------------
// 门禁与匹配的纯决策表
// ---------------------------------------------------------------------------

void run_decision_cases() {
    using namespace deskfeed::inject;

    if (wanted("remote/release-buffer")) {
        check(may_release_remote_buffer(false, false), "remote/release-buffer never-created");
        check(may_release_remote_buffer(false, true), "remote/release-buffer never-created-signaled");
        check(may_release_remote_buffer(true, true), "remote/release-buffer exited");
        // 超时（线程建了、没退出）：目标进程可能还在读那块缓冲，绝不释放。
        check(!may_release_remote_buffer(true, false), "remote/release-buffer timeout-keeps");
    }

    if (wanted("remote/load-conclusive")) {
        check(!load_result_conclusive(true, false), "remote/load-conclusive timeout");
        check(!load_result_conclusive(false, false), "remote/load-conclusive not-created");
        check(load_result_conclusive(true, true), "remote/load-conclusive exited");
    }

    if (wanted("inject/unload-decision")) {
        // 只有 stop 报 1 **且**调用它的远程线程确实退出，才允许 FreeLibrary。
        check_eq(static_cast<int>(unload_decision(1, true, false)), static_cast<int>(UnloadDecision::Unload),
                 "inject/unload-decision unload");
        check_eq(static_cast<int>(unload_decision(1, true, true)),
                 static_cast<int>(UnloadDecision::KeepNoUnload),
                 "inject/unload-decision no-unload");
        check_eq(static_cast<int>(unload_decision(0, true, false)),
                 static_cast<int>(UnloadDecision::KeepLoaded),
                 "inject/unload-decision not-retired");
        check_eq(static_cast<int>(unload_decision(1, false, false)),
                 static_cast<int>(UnloadDecision::KeepLoaded),
                 "inject/unload-decision timeout");
        check_eq(static_cast<int>(unload_decision(0, false, true)),
                 static_cast<int>(UnloadDecision::KeepLoaded),
                 "inject/unload-decision timeout-no-unload");
    }

    if (wanted("remote/module-path")) {
        // 同名不同目录**不算**命中（否则会拿 A 的 RVA 去 B 的基址上执行）。
        check(module_path_matches(L"C:\\a\\x.dll", L"C:\\a\\x.dll"), "remote/module-path exact");
        check(module_path_matches(L"C:\\a\\X.DLL", L"c:\\A\\x.dll"), "remote/module-path case");
        check(module_path_matches(L"C:\\a\\x.dll\\", L"C:\\a\\x.dll"), "remote/module-path trailing");
        check(!module_path_matches(L"C:\\b\\x.dll", L"C:\\a\\x.dll"), "remote/module-path other-dir");
        check(!module_path_matches(L"x.dll", L"C:\\a\\x.dll"), "remote/module-path bare-name");
        check(!module_path_matches(L"", L"C:\\a\\x.dll"), "remote/module-path empty-loaded");
        check(!module_path_matches(L"C:\\a\\x.dll", L""), "remote/module-path empty-wanted");
    }

    if (wanted("target/scope")) {
        std::wstring scope;
        std::wstring error;
        check(resolve_scope(L"", scope, error), "target/scope production");
        check(scope.empty(), "target/scope production-empty");
        check(resolve_scope(L"acc1", scope, error), "target/scope accepted");
        check(scope == L"acc1", "target/scope value");
        check(!resolve_scope(L"a/b", scope, error), "target/scope rejects slash");
        check(!resolve_scope(L"a b", scope, error), "target/scope rejects space");
        check(!resolve_scope(std::wstring(33, L'a'), scope, error), "target/scope rejects long");
    }
}

// ---------------------------------------------------------------------------
// 显示量具的纯排版（与窗口/任务栏共用 display/reading）
// ---------------------------------------------------------------------------

deskfeed::Frame content_frame() {
    deskfeed::Frame frame;
    frame.kind = deskfeed::ViewKind::Content;
    deskfeed::FrameItem cpu;
    cpu.id = "cpu";
    cpu.label = "CPU";
    cpu.value = "54C";
    cpu.state = deskfeed::ItemState::Ok;
    cpu.detail = "sampled";
    frame.items.push_back(cpu);
    deskfeed::FrameItem mem;
    mem.id = "mem";
    mem.label = "MEM";
    mem.value = "61%";
    mem.state = deskfeed::ItemState::Warn;
    mem.detail = "warning level";
    frame.items.push_back(mem);
    return frame;
}

void run_reading_cases() {
    using namespace deskfeed;

    if (wanted("reading/content")) {
        ReadingContent const content = reading_from_frame(content_frame());
        check_eq(content.rows.size(), static_cast<std::size_t>(2), "reading/content rows");
        check_eq(content.rows[0].label, std::string("CPU"), "reading/content row0 label");
        check_eq(content.rows[0].value, std::string("54C"), "reading/content row0 value");
        check_eq(static_cast<int>(content.rows[1].state), static_cast<int>(ItemState::Warn),
                 "reading/content warn state");
        std::string const rows = reading_rows_text(content, 30000);
        check(rows.find("reading: rows=2") != std::string::npos, "reading/content header");
        check(rows.find("row 0: error=0 prefix=<CPU> value=<54C>") != std::string::npos,
              "reading/content row line");
        check(rows.find("row 1: error=0 prefix=<MEM> value=<61%>") != std::string::npos,
              "reading/content warn is not an error");
        check(rows.find("refresh_ms=30000") != std::string::npos, "reading/content refresh field");
    }

    if (wanted("reading/waiting")) {
        Frame waiting;
        waiting.kind = ViewKind::Waiting;
        ReadingContent const content = reading_from_frame(waiting);
        check(content.rows.empty(), "reading/waiting has no rows");
        check(reading_rows_text(content, 30000).find("rows=0") != std::string::npos,
              "reading/waiting header");
    }

    if (wanted("reading/error")) {
        Frame error;
        error.kind = ViewKind::Error;
        error.failure = FailureKind::NoUrl;
        error.reason = "no url";
        ReadingContent const content = reading_from_frame(error);
        check_eq(content.rows.size(), static_cast<std::size_t>(1), "reading/error one row");
        check_eq(content.rows[0].value, std::string("ERROR"), "reading/error value");
        check_eq(static_cast<int>(content.rows[0].tone), static_cast<int>(ReadingTone::Error),
                 "reading/error tone");
        std::string const rows = reading_rows_text(content, 30000);
        check(rows.find("row 0: error=1 prefix=<> value=<ERROR>") != std::string::npos,
              "reading/error row line");
    }

    if (wanted("reading/alarm")) {
        Frame alarmed = content_frame();
        alarmed.alarm = true;
        alarmed.failure = FailureKind::Transport;
        alarmed.reason = "connect failed";
        ReadingContent const content = reading_from_frame(alarmed);
        check_eq(content.rows.size(), static_cast<std::size_t>(2), "reading/alarm keeps rows");
        check_eq(static_cast<int>(content.rows[0].tone), static_cast<int>(ReadingTone::Error),
                 "reading/alarm row is red");
        check_eq(static_cast<int>(content.rows[0].state), static_cast<int>(ItemState::Ok),
                 "reading/alarm keeps producer state");
        check(content.rows[0].tip.find("connect failed") == 0, "reading/alarm reason leads the tip");
    }
}

// ---------------------------------------------------------------------------
// 显示邮箱：本进程里起生产者与消费者，收发一轮（隔离 scope，不影响生产实例）
// ---------------------------------------------------------------------------

void run_mailbox_case() {
    if (!wanted("mailbox/round-trip")) {
        return;
    }
    using namespace deskfeed;

    // scope 带 PID：同一台机器上两个测试进程并发跑时不会撞到同一块命名共享内存。
    std::wstring const scope = L"toolstest-" + std::to_wstring(::GetCurrentProcessId());
    DisplayFrameProducer producer;
    std::string error;
    if (!producer.create(scope, nullptr, error)) {
        check(false, "mailbox/round-trip producer create");
        std::printf("  (%s)\n", error.c_str());
        return;
    }

    Frame published = content_frame();
    check(producer.publish(published, 1000), "mailbox/round-trip publish");
    check(producer.publish_closed(2000), "mailbox/round-trip publish-closed");

    DisplayFrameConsumer consumer;
    // 生产者在本进程里仍然活着，所以 attach 与 read 都必须成功，并且要能读到 closed 标志。
    if (!consumer.attach(scope, error)) {
        check(false, "mailbox/round-trip consumer attach");
        std::printf("  (%s)\n", error.c_str());
        return;
    }
    Frame read{};
    check(consumer.read(read), "mailbox/round-trip read");
    check(read.closed, "mailbox/round-trip closed flag");
    check_eq(read.items.size(), static_cast<std::size_t>(2), "mailbox/round-trip item count");
    if (read.items.size() == 2) {
        check_eq(read.items[0].id, std::string("cpu"), "mailbox/round-trip item id");
    }

    // 生产者收掉之后，消费者必须拒绝再交出这一帧（绝不把最后一帧当活数据）。
    producer.close();
    consumer.detach();
    DisplayFrameConsumer after;
    bool const attached = after.attach(scope, error);
    check(!attached, "mailbox/round-trip refuses after producer exit");
    after.detach();
}

// ---------------------------------------------------------------------------
// 探针宿主的 feature gate：缺必需导出必须 BLOCKED，而不是"日志里跳过、退出码 0"
// ---------------------------------------------------------------------------

deskfeed::probe::ExportPresence all_probe_exports() {
    using deskfeed::probe::ExportPresence;
    ExportPresence presence;
    presence.start = true;
    presence.sites = true;
    presence.watch = true;
    presence.run = true;
    presence.set_host = true;
    presence.stop = true;
    presence.set_slow_apply = true;
    presence.post_apply = true;
    presence.retire = true;
    presence.hook = true;
    presence.teardown = true;
    presence.dangling = true;
    presence.hook_status = true;
    presence.set_observer = true;
    presence.observer_stage = true;
    presence.set_install_delay = true;
    presence.set_completion_mutation = true;
    presence.set_tray_window = true;
    presence.feed_start = true;
    presence.feed_stop = true;
    presence.get_status = true;
    return presence;
}

bool gate_misses_symbol(deskfeed::probe::GateDecision const& decision, wchar_t const* symbol) {
    for (deskfeed::probe::GateMiss const& miss : decision.missing) {
        if (miss.symbol != nullptr && std::wcscmp(miss.symbol, symbol) == 0) {
            return true;
        }
    }
    return false;
}

void run_gate_cases() {
    using deskfeed::probe::ExportPresence;
    using deskfeed::probe::gate_decision;
    using deskfeed::probe::GateDecision;
    using deskfeed::probe::HostOptions;

    ExportPresence const full = all_probe_exports();
    HostOptions mock;  // --mock 基线

    if (wanted("gate/mock-full")) {
        GateDecision const decision = gate_decision(mock, full);
        check(!decision.blocked, "gate/mock-full not blocked");
        check_eq(decision.missing.size(), static_cast<std::size_t>(0), "gate/mock-full no misses");
    }
    if (wanted("gate/mock-empty")) {
        GateDecision const decision = gate_decision(mock, ExportPresence{});
        check(decision.blocked, "gate/mock-empty blocked");
        check_eq(decision.missing.size(), static_cast<std::size_t>(6), "gate/mock-empty six misses");
        check(gate_misses_symbol(decision, L"DeskFeedProbeStart"), "gate/mock-empty names start");
        check(gate_misses_symbol(decision, L"DeskFeedProbeStop"), "gate/mock-empty names stop");
        check(!decision.missing.empty() && !decision.missing[0].feature.empty(),
              "gate/mock-empty names the feature");
    }
    if (wanted("gate/mock-teardown-missing")) {
        HostOptions options = mock;
        options.teardown = true;
        ExportPresence presence = full;
        presence.teardown = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/mock-teardown-missing blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeTeardown"),
              "gate/mock-teardown-missing names teardown");

        presence = full;
        presence.dangling = false;
        GateDecision const no_dangling = gate_decision(options, presence);
        check(no_dangling.blocked, "gate/mock-teardown-needs-dangling blocked");
        check(gate_misses_symbol(no_dangling, L"DeskFeedProbeDangling"),
              "gate/mock-teardown-needs-dangling names dangling");
        // 只有 --teardown 时，缺 dangling 之外不能顺带要求 feed 的入口。
        check(!gate_misses_symbol(no_dangling, L"DeskFeedStart"),
              "gate/mock-teardown does not ask for feed exports");
    }
    if (wanted("gate/mock-slow-missing")) {
        HostOptions options = mock;
        options.slow_apply_ms = 250;
        ExportPresence presence = full;
        presence.set_slow_apply = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/mock-slow-missing blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeSetSlowApply"),
              "gate/mock-slow-missing names slow-apply");
    }
    if (wanted("gate/balance-needs-stop")) {
        HostOptions options = mock;
        options.balance_check = true;
        ExportPresence presence = full;
        presence.stop = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/balance-needs-stop blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeStop"),
              "gate/balance-needs-stop names stop");
    }
    if (wanted("gate/hook-needs-hook")) {
        HostOptions options = mock;
        options.hook = true;
        ExportPresence presence = full;
        presence.hook = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/hook-needs-hook blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeHook"),
              "gate/hook-needs-hook names hook");
    }
    if (wanted("gate/ordering-needs-stage")) {
        HostOptions options = mock;
        options.third_party = true;
        ExportPresence presence = full;
        presence.observer_stage = false;  // R3 的拉取入口
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/ordering-needs-stage blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeObserverStage"),
              "gate/ordering-needs-stage names observer stage");
    }
    if (wanted("gate/ordering-needs-status")) {
        HostOptions options = mock;
        options.third_party = true;
        ExportPresence presence = full;
        presence.hook_status = false;  // R2
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/ordering-needs-status blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeHookStatus"),
              "gate/ordering-needs-status names hook status");
    }
    if (wanted("gate/ordering-full")) {
        HostOptions options = mock;
        options.third_party = true;
        options.mutation_early_done = true;
        options.install_delay_ms = 300;
        GateDecision const decision = gate_decision(options, full);
        check(!decision.blocked, "gate/ordering-full not blocked");
    }
    if (wanted("gate/mutation-needs-r4")) {
        HostOptions options = mock;
        options.third_party = true;
        options.mutation_early_done = true;
        ExportPresence presence = full;
        presence.set_completion_mutation = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/mutation-needs-r4 blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeSetCompletionMutation"),
              "gate/mutation-needs-r4 names mutation export");
    }
    if (wanted("gate/install-race-needs-r1")) {
        HostOptions options = mock;
        options.install_delay_ms = 300;  // --install-race 也是对抗序列，隐式要求 R3 那一组
        ExportPresence presence = full;
        presence.set_install_delay = false;
        presence.post_apply = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/install-race-needs-r1 blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeSetInstallDelay"),
              "gate/install-race-needs-r1 names delay export");
        check(gate_misses_symbol(decision, L"DeskFeedProbePostApply"),
              "gate/install-race-needs-r1 also asks for the ordering exports");
    }
    if (wanted("gate/hang-needs-post")) {
        HostOptions options = mock;
        options.hang_ui_ms = 400;
        ExportPresence presence = full;
        presence.post_apply = false;
        presence.retire = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/hang-needs-post blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbePostApply"),
              "gate/hang-needs-post names post");
        check(gate_misses_symbol(decision, L"DeskFeedProbeRetire"),
              "gate/hang-needs-post names retire");
    }
    if (wanted("gate/feed-full")) {
        HostOptions options;
        options.feed = true;
        GateDecision const decision = gate_decision(options, full);
        check(!decision.blocked, "gate/feed-full not blocked");
    }
    if (wanted("gate/feed-needs-status")) {
        HostOptions options;
        options.feed = true;
        ExportPresence presence = full;
        presence.get_status = false;  // 144 字节状态 POD
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/feed-needs-status blocked");
        check(gate_misses_symbol(decision, L"DeskFeedGetStatus"),
              "gate/feed-needs-status names get-status");
    }
    if (wanted("gate/feed-needs-tray")) {
        HostOptions options;
        options.feed = true;
        ExportPresence presence = full;
        presence.set_tray_window = false;  // R5
        presence.feed_start = false;
        GateDecision const decision = gate_decision(options, presence);
        check(decision.blocked, "gate/feed-needs-tray blocked");
        check(gate_misses_symbol(decision, L"DeskFeedProbeSetTrayWindow"),
              "gate/feed-needs-tray names tray window");
        check(gate_misses_symbol(decision, L"DeskFeedStart"), "gate/feed-needs-tray names start");
    }
    if (wanted("gate/feed-ignores-mock")) {
        // 一个只实现了生产入口、完全没有探针结构自测导出的 DLL：feed 仍然可以判定为不缺。
        HostOptions options;
        options.feed = true;
        ExportPresence presence;
        presence.set_tray_window = true;
        presence.feed_start = true;
        presence.feed_stop = true;
        presence.get_status = true;
        presence.dangling = true;
        check(!gate_decision(options, presence).blocked, "gate/feed-ignores-mock not blocked");
    }
}

// ---------------------------------------------------------------------------
// 显示帧线格式：编解码往返 + 逐项拒绝（不需要窗口、DLL 或共享内存）
// ---------------------------------------------------------------------------

void patch_u32(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (8 * index)) & 0xFFu);
    }
}

deskfeed::Frame frame_with_states() {
    using namespace deskfeed;
    Frame frame;
    frame.kind = ViewKind::Content;
    frame.alarm = false;
    frame.failure = FailureKind::None;
    frame.items.push_back(FrameItem{"cpu", "CPU", "54C", ItemState::Ok, "sampled"});
    frame.items.push_back(FrameItem{"mem", "MEM", "61%", ItemState::Warn, "warning level"});
    frame.items.push_back(FrameItem{"disk", "DISK", "ERROR", ItemState::Error, "unreadable"});
    return frame;
}

void run_frame_cases() {
    using namespace deskfeed;

    if (!wanted("frame/")) {
        return;
    }

    // 往返：三种条目状态、kind / failure / reason 都逐字段带回来。
    Frame const original = frame_with_states();
    std::vector<unsigned char> payload;
    check(frame_encode(original, payload), "frame/encode-content");
    Frame decoded{};
    check(frame_decode(payload.data(), payload.size(), decoded), "frame/decode-content");
    check_eq(static_cast<int>(decoded.kind), static_cast<int>(ViewKind::Content),
             "frame/decode-content kind");
    check_eq(decoded.items.size(), static_cast<std::size_t>(3), "frame/decode-content items");
    check_eq(static_cast<int>(decoded.items[1].state), static_cast<int>(ItemState::Warn),
             "frame/decode-content warn state");
    check_eq(decoded.items[2].value, std::string("ERROR"), "frame/decode-content error value");
    check_eq(decoded.items[0].detail, std::string("sampled"), "frame/decode-content detail");

    // 合法清空：items 为空、没有 alarm/failure —— 它必须被接纳（不是"取数失败"）。
    Frame clear;
    clear.kind = ViewKind::Content;
    std::vector<unsigned char> clear_payload;
    check(frame_encode(clear, clear_payload), "frame/encode-clear");
    Frame clear_back{};
    check(frame_decode(clear_payload.data(), clear_payload.size(), clear_back), "frame/decode-clear");
    check(clear_back.items.empty(), "frame/decode-clear has no items");
    check(!clear_back.alarm, "frame/decode-clear is not an alarm");
    check_eq(static_cast<int>(clear_back.failure), static_cast<int>(FailureKind::None),
             "frame/decode-clear has no failure");

    // 整体失效帧：alarm + failure + reason，条目原样保留。
    Frame alarm = frame_with_states();
    alarm.alarm = true;
    alarm.failure = FailureKind::Transport;
    alarm.reason = "connect failed";
    std::vector<unsigned char> alarm_payload;
    check(frame_encode(alarm, alarm_payload), "frame/encode-alarm");
    Frame alarm_back{};
    check(frame_decode(alarm_payload.data(), alarm_payload.size(), alarm_back), "frame/decode-alarm");
    check(alarm_back.alarm, "frame/decode-alarm alarm flag");
    check_eq(static_cast<int>(alarm_back.failure), static_cast<int>(FailureKind::Transport),
             "frame/decode-alarm failure");
    check_eq(alarm_back.reason, std::string("connect failed"), "frame/decode-alarm reason");
    check_eq(alarm_back.items.size(), static_cast<std::size_t>(3), "frame/decode-alarm keeps items");

    // 没有可显示条目时的红色 ERROR：kind=Error + reason。
    Frame error;
    error.kind = ViewKind::Error;
    error.failure = FailureKind::Parse;
    error.reason = "bad json";
    std::vector<unsigned char> error_payload;
    check(frame_encode(error, error_payload), "frame/encode-error");
    Frame error_back{};
    check(frame_decode(error_payload.data(), error_payload.size(), error_back), "frame/decode-error");
    check_eq(static_cast<int>(error_back.kind), static_cast<int>(ViewKind::Error),
             "frame/decode-error kind");
    check(error_back.items.empty(), "frame/decode-error has no items");

    // 逐项拒绝：空指针、空载荷、每一个截断点、尾巴上多一个字节。
    Frame sink{};
    check(!frame_decode(nullptr, 0, sink), "frame/decode rejects null");
    check(!frame_decode(payload.data(), 0, sink), "frame/decode rejects empty");
    bool truncation_accepted = false;
    for (std::size_t cut = 1; cut < payload.size(); ++cut) {
        if (frame_decode(payload.data(), cut, sink)) {
            truncation_accepted = true;
        }
    }
    check(!truncation_accepted, "frame/decode rejects every truncation");
    std::vector<unsigned char> trailing = payload;
    trailing.push_back(0);
    check(!frame_decode(trailing.data(), trailing.size(), sink),
          "frame/decode rejects trailing bytes");

    // 逐字段越界：条目数、alarm、kind、failure、state、字符串长度上限、原因长度上限。
    std::vector<unsigned char> bad = payload;
    patch_u32(bad, 0, static_cast<std::uint32_t>(kMaxItems) + 1);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects too many items");

    bad = payload;
    patch_u32(bad, 8, 2);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects alarm=2");

    bad = payload;
    patch_u32(bad, 4, 3);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects unknown kind");

    bad = payload;
    patch_u32(bad, 12, 11);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects unknown failure");

    bad = payload;
    patch_u32(bad, 16, static_cast<std::uint32_t>(kFrameMaxReasonBytes) + 1);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects oversized reason");

    std::size_t const first_item = 20 + original.reason.size();
    bad = payload;
    patch_u32(bad, first_item, 3);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects unknown state");

    bad = payload;
    patch_u32(bad, first_item + 4, static_cast<std::uint32_t>(kMaxIdBytes) + 1);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects oversized id");

    bad = payload;
    patch_u32(bad, first_item + 12, static_cast<std::uint32_t>(kMaxValueBytes) + 1);
    check(!frame_decode(bad.data(), bad.size(), sink), "frame/decode rejects oversized value");

    // 编码侧的容量拒绝：原因超限、单字段超限、条目数超限。
    Frame too_reason;
    too_reason.reason.assign(kFrameMaxReasonBytes + 1, 'x');
    std::vector<unsigned char> scratch;
    check(!frame_encode(too_reason, scratch), "frame/encode rejects oversized reason");

    Frame too_label;
    too_label.items.push_back(FrameItem{"id", std::string(kMaxLabelBytes + 1, 'L'), "v",
                                        ItemState::Ok, ""});
    check(!frame_encode(too_label, scratch), "frame/encode rejects oversized label");

    Frame too_many;
    for (std::size_t index = 0; index <= kMaxItems; ++index) {
        too_many.items.push_back(FrameItem{"id", "L", "v", ItemState::Ok, ""});
    }
    check(!frame_encode(too_many, scratch), "frame/encode rejects too many items");

    // frame_from_view 只搬字段，不解释：View → Frame 的映射要逐项一致。
    View view;
    view.kind = ViewKind::Content;
    view.alarm = true;
    view.failure = FailureKind::Timeout;
    view.reason = "timed out";
    view.items.push_back(ViewItem{"cpu", "CPU", "54C", ItemState::Ok, "sampled"});
    Frame const mapped = frame_from_view(view);
    check_eq(mapped.items.size(), static_cast<std::size_t>(1), "frame/from-view items");
    check_eq(static_cast<int>(mapped.failure), static_cast<int>(FailureKind::Timeout),
             "frame/from-view failure");
    check(mapped.alarm, "frame/from-view alarm");
    check_eq(mapped.reason, std::string("timed out"), "frame/from-view reason");
    check_eq(mapped.items[0].id, std::string("cpu"), "frame/from-view item id");
}

// ---------------------------------------------------------------------------
// 显示邮箱语义：只读最新一帧、清空不复活、所有者退出后拒绝旧帧
// ---------------------------------------------------------------------------

void run_mailbox_latest_cases() {
    using namespace deskfeed;
    if (!wanted("mailbox/latest")) {
        return;
    }

    std::wstring const scope = L"toolstest-latest-" + std::to_wstring(::GetCurrentProcessId());
    DisplayFrameProducer producer;
    std::string error;
    if (!producer.create(scope, nullptr, error)) {
        check(false, "mailbox/latest producer create");
        std::printf("  (%s)\n", error.c_str());
        return;
    }

    // 发三帧（2 条 → 空 → 1 条）。邮箱只保留最新一帧，绝不排队积压：最后一次读必须只看到 1 条。
    Frame first = frame_with_states();
    Frame cleared;
    cleared.kind = ViewKind::Content;
    Frame last;
    last.kind = ViewKind::Content;
    last.items.push_back(FrameItem{"cpu", "CPU", "55C", ItemState::Ok, "latest"});

    check(producer.publish(first, 1000), "mailbox/latest publish first");
    check(producer.publish(cleared, 2000), "mailbox/latest publish clear");
    check(producer.publish(last, 3000), "mailbox/latest publish last");

    DisplayFrameConsumer consumer;
    if (!consumer.attach(scope, error)) {
        check(false, "mailbox/latest consumer attach");
        std::printf("  (%s)\n", error.c_str());
        producer.close();
        return;
    }
    Frame read{};
    check(consumer.read(read), "mailbox/latest read");
    check_eq(read.items.size(), static_cast<std::size_t>(1), "mailbox/latest keeps only the newest");
    check_eq(read.generation, static_cast<std::uint64_t>(3), "mailbox/latest generation is the last");
    if (read.items.size() == 1) {
        check_eq(read.items[0].value, std::string("55C"), "mailbox/latest newest value");
    }

    // 清空之后取数失败：邮箱里是"零条目 + 整体失效"，绝不复活清空前的条目。
    Frame failed_after_clear;
    failed_after_clear.kind = ViewKind::Error;
    failed_after_clear.failure = FailureKind::Transport;
    failed_after_clear.reason = "fetch failed after the clear";
    check(producer.publish(failed_after_clear, 4000), "mailbox/clear-failure publish");
    Frame after_failure{};
    check(consumer.read(after_failure), "mailbox/clear-failure read");
    check(after_failure.items.empty(), "mailbox/clear-failure resurrects nothing");
    check_eq(static_cast<int>(after_failure.failure), static_cast<int>(FailureKind::Transport),
             "mailbox/clear-failure keeps the failure reason");
    check_eq(after_failure.generation, static_cast<std::uint64_t>(4),
             "mailbox/clear-failure generation advanced");

    // 正常收尾：closed 置位，但最后一帧内容保留（读端据此清理任务栏组件）。
    check(producer.publish_closed(5000), "mailbox/latest publish closed");
    Frame closed{};
    check(consumer.read(closed), "mailbox/latest read closed");
    check(closed.closed, "mailbox/latest closed flag");

    consumer.detach();
    producer.close();
}

// 子进程模式：只建一个生产者、发一帧，然后停在那里等父进程结束它。
int run_mailbox_owner_child(std::wstring const& scope) {
    using namespace deskfeed;
    DisplayFrameProducer producer;
    std::string error;
    if (!producer.create(scope, nullptr, error)) {
        std::printf("child: producer create failed (%s)\n", error.c_str());
        return 3;
    }
    Frame frame = frame_with_states();
    if (!producer.publish(frame, 1000)) {
        std::printf("child: publish failed\n");
        return 4;
    }
    if (!producer.publish_closed(2000)) {
        std::printf("child: publish_closed failed\n");
        return 5;
    }
    // 有界停留：父进程崩溃时也不留下孤儿进程。
    ::Sleep(20000);
    producer.close();
    return 0;
}

void run_mailbox_owner_death_case() {
    using namespace deskfeed;
    if (!wanted("mailbox/owner-death")) {
        return;
    }

    wchar_t self[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        check(false, "mailbox/owner-death self path");
        return;
    }
    std::wstring const scope = L"toolstest-owner-" + std::to_wstring(::GetCurrentProcessId());
    std::wstring command = L"\"" + std::wstring(self) + L"\" --mailbox-owner \"" + scope + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &process) == FALSE) {
        check(false, "mailbox/owner-death spawn child");
        return;
    }

    // 等子进程把生产者建起来（有界重试，不做无界等待）。
    DisplayFrameConsumer consumer;
    std::string error;
    bool attached = false;
    for (int attempt = 0; attempt < 100 && !attached; ++attempt) {
        ::Sleep(50);
        attached = consumer.attach(scope, error);
    }
    check(attached, "mailbox/owner-death attach while the owner lives");
    if (!attached) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 5000);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return;
    }

    Frame read{};
    check(consumer.read(read), "mailbox/owner-death read while the owner lives");
    check(consumer.producer_running(), "mailbox/owner-death owner reported running");

    // 所有者死亡（attach 之后）：绝不把最后一帧当活数据交出去。
    check(::TerminateProcess(process.hProcess, 1) != FALSE, "mailbox/owner-death terminate owner");
    DWORD const waited = ::WaitForSingleObject(process.hProcess, 5000);
    check_eq(waited, static_cast<DWORD>(WAIT_OBJECT_0), "mailbox/owner-death owner exited");
    check(!consumer.producer_running(), "mailbox/owner-death owner no longer running");
    Frame stale{};
    check(!consumer.read(stale), "mailbox/owner-death refuses the stale frame");
    check_eq(stale.items.size(), static_cast<std::size_t>(0),
             "mailbox/owner-death leaves the output untouched");

    consumer.detach();
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
}

// ---------------------------------------------------------------------------
// 远程镜像身份核验（T7）：纯判定表 + 真·装载模块的受限读取
// ---------------------------------------------------------------------------

void run_identity_cases(std::wstring const& dir) {
    using namespace deskfeed::inject;
    if (!wanted("identity/")) {
        return;
    }

    // 纯判定：磁盘那份与远程那份必须逐项一致。
    PeExportInfo disk;
    disk.machine = IMAGE_FILE_MACHINE_AMD64;
    disk.is_pe32_plus = true;
    disk.image_size = 0x3000;
    disk.time_date_stamp = 0x5F5E1000;
    disk.rva = 0x1000;
    disk.section_va = 0x1000;
    disk.section_end = 0x2000;

    RemoteImageIdentity remote;
    remote.machine = IMAGE_FILE_MACHINE_AMD64;
    remote.pe32_plus = true;
    remote.image_size = 0x3000;
    remote.time_date_stamp = 0x5F5E1000;
    remote.export_rva = 0x1000;
    remote.export_in_exec_section = true;

    std::wstring error;
    check(image_identity_matches(disk, remote, error), "identity/matches");

    RemoteImageIdentity drift = remote;
    drift.time_date_stamp ^= 1;
    check(!image_identity_matches(disk, drift, error), "identity/rejects different timestamp");

    drift = remote;
    drift.image_size += 0x1000;  // 同尺寸检查：这里刻意只改尺寸，证明它不只比尺寸也仍然会拒绝
    check(!image_identity_matches(disk, drift, error), "identity/rejects different size");

    drift = remote;
    drift.export_rva = 0x1100;
    check(!image_identity_matches(disk, drift, error), "identity/rejects different export RVA");

    drift = remote;
    drift.export_in_exec_section = false;
    check(!image_identity_matches(disk, drift, error), "identity/rejects unproven exec section");

    drift = remote;
    drift.machine = 0x014C;
    check(!image_identity_matches(disk, drift, error), "identity/rejects non-AMD64");

    drift = remote;
    drift.pe32_plus = false;
    check(!image_identity_matches(disk, drift, error), "identity/rejects non-PE32+");

    // 磁盘侧自己那份 RVA 必须落在它自己的可执行节里（否则解析结果本身就不可信）。
    PeExportInfo outside = disk;
    outside.rva = 0x2500;
    RemoteImageIdentity moved = remote;
    moved.export_rva = 0x2500;
    check(!image_identity_matches(outside, moved, error),
          "identity/rejects RVA outside the disk executable section");

    // 磁盘侧新字段：真 fixture（TimeDateStamp 非零 + 导出在 .text 里）。
    PeKnobs knobs;
    knobs.time_date_stamp = 0x5F5E1000;
    std::wstring const fixture = dir + L"\\identity.dll";
    if (!write_file(fixture, build_pe(knobs))) {
        check(false, "identity/disk fixture write");
    } else {
        PeExportInfo info;
        std::wstring disk_error;
        PeExportError kind = PeExportError::None;
        check(pe_export_rva(fixture, "Probe", info, disk_error, &kind), "identity/disk export parsed");
        check_eq(info.time_date_stamp, 0x5F5E1000ul, "identity/disk timestamp");
        check(info.is_pe32_plus, "identity/disk pe32+");
        check(static_cast<unsigned long long>(info.rva) >= info.section_va &&
                  static_cast<unsigned long long>(info.rva) < info.section_end,
              "identity/disk RVA inside the executable section");
    }

    // 真·装载模块：把 user32.dll 装进**本进程**（无注入、无 explorer、无窗口），再用远程读取路径
    // 读它自己的 PE 元数据/导出表 —— 这一条真的执行了 T7 的读取与判定代码。
    HMODULE const user32 = ::LoadLibraryW(L"user32.dll");
    if (user32 == nullptr) {
        check(false, "identity/live user32 load");
        return;
    }
    wchar_t module_path[MAX_PATH]{};
    if (::GetModuleFileNameW(user32, module_path, MAX_PATH) == 0) {
        check(false, "identity/live module path");
        ::FreeLibrary(user32);
        return;
    }
    unsigned long long base = reinterpret_cast<unsigned long long>(user32);
    unsigned long long size = 0;
    std::vector<ModuleEntry> modules;
    std::wstring list_error;
    if (list_modules(::GetCurrentProcessId(), modules, list_error)) {
        for (ModuleEntry const& entry : modules) {
            if (module_path_matches(entry.path, module_path)) {
                base = entry.base;
                size = entry.size;
            }
        }
    }
    check(size != 0, "identity/live module table reports a size");

    char const* const candidates[] = {"MessageBoxW", "GetSystemMetrics", "LoadStringW"};
    char const* chosen = nullptr;
    PeExportInfo live_disk;
    std::wstring live_error;
    for (char const* const candidate : candidates) {
        PeExportError kind = PeExportError::None;
        std::wstring attempt_error;
        if (pe_export_rva(module_path, candidate, live_disk, attempt_error, &kind)) {
            chosen = candidate;
            break;
        }
    }
    check(chosen != nullptr, "identity/live user32 export parsed");
    if (chosen != nullptr) {
        unsigned long long const reported = size != 0 ? size : live_disk.image_size;
        RemoteImageIdentity live;
        bool const read =
            remote_image_identity(::GetCurrentProcessId(), base, reported, chosen, live, live_error);
        check(read, "identity/live remote identity read");
        if (read) {
            check_eq(live.export_rva, live_disk.rva, "identity/live remote RVA equals the disk RVA");
            check_eq(live.time_date_stamp, live_disk.time_date_stamp,
                     "identity/live timestamp equals the disk one");
            check(live.export_in_exec_section, "identity/live RVA proven executable");
            check(image_identity_matches(live_disk, live, live_error), "identity/live matches the disk");
        } else {
            std::printf("  (live read: %ls)\n", live_error.c_str());
        }

        // 读不到时必须 fail closed：给一个坏基址，必须返回 false 并带原因，而不是"看起来像同一个"。
        RemoteImageIdentity bogus;
        std::wstring bogus_error;
        check(!remote_image_identity(::GetCurrentProcessId(), base + 0x40000000ULL, reported, chosen,
                                     bogus, bogus_error),
              "identity/live rejects a bad image base");
        check(!bogus_error.empty(), "identity/live bad base explains itself");
    }
    ::FreeLibrary(user32);
}

// 测试专用延迟写 DLL 与"自己的目录"：由构建把 DLL 放在与本可执行文件同一个目录。
std::wstring test_writer_dll_path() {
    std::wstring const self = deskfeed::module_self_path();
    std::wstring::size_type const slash = self.find_last_of(L"\\/");
    std::wstring const dir = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash + 1);
    return dir + L"deskfeed-test-remote-writer.dll";
}

// 子进程模式：只装载延迟写 DLL 并停在那里，供父进程制造真实的远程调用超时。
int run_remote_writer_child() {
    std::wstring const dll = test_writer_dll_path();
    HMODULE const module = ::LoadLibraryW(dll.c_str());
    if (module == nullptr) {
        std::printf("child: cannot load %ls\n", dll.c_str());
        return 3;
    }
    // 有界自毁：父进程正常会 TerminateProcess，这里只防止父进程崩溃时留下孤儿。
    ::Sleep(20000);
    ::FreeLibrary(module);
    return 0;
}

// HARD2：远程线程超时之后仍在写调用方缓冲。用真子进程 + 真延迟写 DLL 制造这个窗口，断言
// "超时后绝不释放"，并且迟到的 144 字节写入完整落地（没有被复用或破坏）。
void run_remote_late_write_case() {
    using namespace deskfeed::inject;
    if (!wanted("remote/late-write")) {
        return;
    }

    std::wstring const dll = test_writer_dll_path();
    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        check(false, "remote/late-write the delayed-writer test DLL is missing");
        return;
    }

    wchar_t self[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        check(false, "remote/late-write self path");
        return;
    }
    std::wstring command = L"\"" + std::wstring(self) + L"\" --remote-writer";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &process) == FALSE) {
        check(false, "remote/late-write spawn child");
        return;
    }
    unsigned long const child_pid = process.dwProcessId;

    unsigned long long dll_base = 0;
    for (int attempt = 0; attempt < 100 && dll_base == 0; ++attempt) {
        ::Sleep(50);
        std::vector<ModuleEntry> modules;
        std::wstring list_error;
        if (list_modules(child_pid, modules, list_error)) {
            for (ModuleEntry const& entry : modules) {
                if (module_path_matches(entry.path, dll)) {
                    dll_base = entry.base;
                }
            }
        }
    }
    check(dll_base != 0, "remote/late-write the child loaded the writer DLL");

    PeExportInfo info;
    std::wstring error;
    PeExportError kind = PeExportError::None;
    bool const parsed =
        dll_base != 0 && pe_export_rva(dll, "DeskFeedTestDelayedWrite", info, error, &kind);
    check(parsed, "remote/late-write the delayed writer export was parsed");

    if (parsed) {
        RemoteBuffer buffer;
        std::wstring buffer_error;
        check(remote_alloc(child_pid, 144, buffer, buffer_error), "remote/late-write buffer allocated");

        unsigned char header[144]{};
        std::uint32_t size = 144;
        std::uint32_t abi = 1;
        std::uint32_t width = 64;
        std::memcpy(header + 0, &size, 4);
        std::memcpy(header + 4, &abi, 4);
        std::memcpy(header + 8, &width, 4);
        check(remote_write(child_pid, buffer.address, header, sizeof(header), buffer_error),
              "remote/late-write POD header written");

        // 真实的远程调用超时：线程已创建、250ms 内不退出，而写发生在 800ms 之后。
        RemoteCall call = call_remote_export(child_pid, dll_base, info.rva,
                                             reinterpret_cast<void*>(buffer.address), 250);
        check(call.thread_created, "remote/late-write remote thread created");
        check(!call.signaled, "remote/late-write the call really timed out");
        check(call.thread != nullptr, "remote/late-write the unresolved call keeps its thread handle");
        check(!may_release_remote_buffer(call.thread_created, call.signaled),
              "remote/late-write the condition policy retains the buffer after a timeout");
        release_remote_call(call);

        // 直接走被修的那个函数：短超时必须返回 false，并且如实报告"缓冲区保留"。
        RemoteCall pod_call;
        std::wstring pod_error;
        check(!remote_fetch_pod(child_pid, dll_base, info.rva, header, sizeof(header), 250, pod_call,
                                pod_error),
              "remote/late-write remote_fetch_pod times out");
        check(pod_error.find(L"stays allocated") != std::wstring::npos,
              "remote/late-write remote_fetch_pod reports the retained buffer");
        release_remote_call(pod_call);

        ::Sleep(1200);  // 让两个远程写者都完成它们那 144 字节
        unsigned char read_back[144]{};
        bool const read_ok =
            remote_read(child_pid, buffer.address, read_back, sizeof(read_back), buffer_error);
        check(read_ok, "remote/late-write the retained buffer is still readable after the late write");
        unsigned char const magic[8] = {0x44, 0x46, 0x4C, 0x41, 0x54, 0x45, 0x01, 0x00};
        bool const intact = read_ok && std::memcmp(read_back, magic, sizeof(magic)) == 0;
        check(intact, "remote/late-write the late 144-byte write landed intact (no reuse/corruption)");
        if (intact) {
            std::printf("  late-write: retired=yes allocated=144 bytes pattern=intact\n");
        }
        if (read_ok) {
            remote_free(child_pid, buffer);
        }

        // 写者返回之后再调一次：结论必须变成"成功"，证明没有留下半释放状态。
        RemoteCall settled;
        std::wstring settled_error;
        check(remote_fetch_pod(child_pid, dll_base, info.rva, header, sizeof(header), 5000, settled,
                               settled_error),
              "remote/late-write remote_fetch_pod succeeds once the writer returns");
        check(settled.signaled && remote_call_actual(settled),
              "remote/late-write the settled fetch is an actual result");
        release_remote_call(settled);
    }

    ::TerminateProcess(process.hProcess, 0);
    ::WaitForSingleObject(process.hProcess, 5000);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
}

// ---------------------------------------------------------------------------
// M4 原语：超时**不** CloseHandle；park 到真实 signal 或真实目标死亡
//
// 这一组用例咬的是"不丢事实"：未决调用必须把真实线程句柄留住，并且只能在 park 拿到**实际**
// 结论（线程真退出 / 目标真死亡）之后才允许释放那块远程缓冲。全部用自己的 hidden 子进程与
// 交付的延迟写 DLL，不碰 explorer、不起窗口。
// ---------------------------------------------------------------------------

// 一个"装着延迟写 DLL 并停在那里"的子进程，连同父进程要用的模块基址与导出 RVA。
struct WriterChild {
    PROCESS_INFORMATION process{};
    unsigned long pid = 0;
    unsigned long long dll_base = 0;
    deskfeed::inject::PeExportInfo export_info;
    bool ready = false;
};

bool spawn_writer_child(WriterChild& child) {
    using namespace deskfeed::inject;
    child = WriterChild{};

    std::wstring const dll = test_writer_dll_path();
    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    wchar_t self[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return false;
    }

    std::wstring command = L"\"" + std::wstring(self) + L"\" --remote-writer";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &child.process) == FALSE) {
        return false;
    }
    child.pid = child.process.dwProcessId;

    for (int attempt = 0; attempt < 100 && child.dll_base == 0; ++attempt) {
        ::Sleep(50);
        std::vector<ModuleEntry> modules;
        std::wstring list_error;
        if (list_modules(child.pid, modules, list_error)) {
            for (ModuleEntry const& entry : modules) {
                if (module_path_matches(entry.path, dll)) {
                    child.dll_base = entry.base;
                }
            }
        }
    }
    if (child.dll_base == 0) {
        return false;
    }

    std::wstring error;
    PeExportError kind = PeExportError::None;
    child.ready = pe_export_rva(dll, "DeskFeedTestDelayedWrite", child.export_info, error, &kind);
    return child.ready;
}

// 只终止**本用例自己** CreateProcessW 出来的那个 PID，绝不按镜像名杀进程。
void kill_writer_child(WriterChild& child) {
    if (child.process.hProcess != nullptr) {
        ::TerminateProcess(child.process.hProcess, 0);
        ::WaitForSingleObject(child.process.hProcess, 5000);
    }
    if (child.process.hThread != nullptr) {
        ::CloseHandle(child.process.hThread);
    }
    if (child.process.hProcess != nullptr) {
        ::CloseHandle(child.process.hProcess);
    }
    child = WriterChild{};
}

unsigned char const kLateWriteMagic[8] = {0x44, 0x46, 0x4C, 0x41, 0x54, 0x45, 0x01, 0x00};

// 在一个短命线程里等一会儿再结束目标：制造"park 期间目标死亡"这条真实路径。
struct KillerArgs {
    HANDLE process = nullptr;
    DWORD delay_ms = 0;
};

DWORD WINAPI kill_target_after_delay(void* raw) {
    KillerArgs* const args = static_cast<KillerArgs*>(raw);
    ::Sleep(args->delay_ms);
    ::TerminateProcess(args->process, 0);
    return 0;
}

void run_park_cases() {
    using namespace deskfeed::inject;

    // 每个子用例各自可过滤（`--filter park/target-death`）：出问题时要能把范围缩到一段。
    if (wanted("park/identity")) {
        TargetProcess target;
        std::wstring error;
        check(!open_target_process(::GetCurrentProcessId(), 1, target, error),
              "park/identity rejects a mismatched creation time");
        check(!error.empty(), "park/identity explains the identity mismatch");
        check(target.process == nullptr, "park/identity leaves no handle behind on rejection");

        error.clear();
        check(open_target_process(::GetCurrentProcessId(), 0, target, error),
              "park/identity captures the current process");
        check(target.creation_time != 0, "park/identity captured a creation-time identity");
        check(target_process_alive(target), "park/identity the captured process is alive");
        close_target_process(target);
        check(target.process == nullptr, "park/identity close resets the captured handle");
    }

    if (wanted("park/no-target-handle")) {
        RemoteCall fake;
        fake.thread_created = true;
        check(park_remote_call(fake, nullptr, 10) == ParkResult::Error,
              "park/no-target-handle refuses to pretend it can observe the outcome");
        check(remote_call_unresolved(fake), "park/no-target-handle leaves the call unresolved");
    }

    if (wanted("park/never-executed")) {
        // 真实的"从未执行"：目标是**已经退出的**进程，CreateRemoteThread 被拒（实测 ERROR_ACCESS_DENIED）。
        // 刻意不用"权限不足的句柄"来构造失败：本机实测只有
        // PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE 的句柄**照样能** CreateRemoteThread
        // （对其他进程也一样），线程会在目标里跳到坏地址——那不是"必然失败"的构造。
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/never-executed could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/never-executed captured the target process handle");
            ::TerminateProcess(child.process.hProcess, 0);
            ::WaitForSingleObject(child.process.hProcess, 5000);
            check(!target_process_alive(target), "park/never-executed the target is really gone");

            RemoteCall call = call_remote_export_on(target.process, child.dll_base,
                                                    child.export_info.rva, nullptr, 100);
            check(!call.thread_created, "park/never-executed the remote thread was never created");
            check(!call.completed_late, "park/never-executed is not a late completion");
            check(!call.target_died, "park/never-executed is not a target death (no call happened)");
            check(remote_call_actual(call), "park/never-executed is an actual (never executed) result");
            check(!remote_call_unresolved(call), "park/never-executed is not unresolved");
            check(may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/never-executed may release its buffer (nothing ever ran)");
            check(park_remote_call(call, target.process, 10) == ParkResult::NotPending,
                  "park/never-executed has nothing to park");

            close_target_process(target);
            kill_writer_child(child);
        }
    }

    if (wanted("park/late-write")) {
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/late-write could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/late-write captured the target process handle");

            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc(child.pid, 144, buffer, buffer_error), "park/late-write buffer allocated");

            RemoteCall call = call_remote_export_on(target.process, child.dll_base, child.export_info.rva,
                                                    reinterpret_cast<void*>(buffer.address), 250);
            check(call.thread_created, "park/late-write the remote thread was created");
            check(!call.signaled, "park/late-write the call really timed out first");
            check(remote_call_unresolved(call), "park/late-write the call is unresolved after the timeout");
            check(call.thread != nullptr,
                  "park/late-write the thread handle is kept open (not CloseHandle'd)");
            check(call.thread_id != 0, "park/late-write the real remote tid was captured");
            check(!remote_call_actual(call), "park/late-write an unresolved call is not an actual result");
            check(!may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/late-write the buffer must stay while the call is unresolved");

            ParkResult const parked = park_remote_call(call, target.process, 100);
            check(parked == ParkResult::ThreadSignaled,
                  "park/late-write parking observed the real signal");
            check(call.signaled && call.completed_late,
                  "park/late-write the completion is actual and marked as late");
            check(call.exit_code == 1, "park/late-write the real exit code was read");
            check(remote_call_actual(call), "park/late-write the parked call is an actual result");
            check(may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/late-write the buffer may be released only now");

            unsigned char read_back[144]{};
            bool const read_ok =
                remote_read(child.pid, buffer.address, read_back, sizeof(read_back), buffer_error);
            check(read_ok && std::memcmp(read_back, kLateWriteMagic, sizeof(kLateWriteMagic)) == 0,
                  "park/late-write the late 144-byte write landed intact");

            remote_free(child.pid, buffer);
            release_remote_call(call);
            check(call.thread == nullptr, "park/late-write release closes the kept thread handle");
            close_target_process(target);
            kill_writer_child(child);
        }
    }

    if (wanted("park/interrupted")) {
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/interrupted could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/interrupted captured the target process handle");
            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc(child.pid, 144, buffer, buffer_error), "park/interrupted buffer allocated");

            HANDLE stop_event = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
            RemoteCall call = call_remote_export_on(target.process, child.dll_base, child.export_info.rva,
                                                    reinterpret_cast<void*>(buffer.address), 250);
            check(call.thread_created && !call.signaled, "park/interrupted the call timed out first");
            ParkResult const parked = park_remote_call(call, target.process, 50, stop_event);
            check(parked == ParkResult::Interrupted, "park/interrupted reports the interruption");
            check(remote_call_unresolved(call), "park/interrupted leaves the call unresolved");
            check(!call.target_died, "park/interrupted is not a target death");
            check(!may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/interrupted keeps the buffer (unknown is not clean)");

            release_remote_call(call);
            ::CloseHandle(stop_event);
            remote_free(child.pid, buffer);
            close_target_process(target);
            kill_writer_child(child);
        }
    }

    if (wanted("park/target-death")) {
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/target-death could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/target-death captured the target process handle");
            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc(child.pid, 144, buffer, buffer_error), "park/target-death buffer allocated");

            RemoteCall call = call_remote_export_on(target.process, child.dll_base, child.export_info.rva,
                                                    reinterpret_cast<void*>(buffer.address), 250);
            check(call.thread_created && !call.signaled,
                  "park/target-death the call is still unresolved when the target is killed");

            // 在写者那 800ms 睡完之前杀掉**自己 spawn 的**目标进程，并等进程对象真的 signal：
            // 这样 park 的第一轮判定就同时看到"线程句柄 signal + 进程句柄 signal"，用来钉住
            // "目标死亡优先于入口返回"这条规则（不靠时间竞态取胜）。
            ::TerminateProcess(child.process.hProcess, 0);
            ::WaitForSingleObject(child.process.hProcess, 5000);

            ParkResult const parked = park_remote_call(call, target.process, 100);
            check(parked == ParkResult::TargetDied,
                  "park/target-death parking reports the target death");
            check(call.target_died && !call.signaled,
                  "park/target-death is not reported as a clean thread exit");
            check(!remote_call_actual(call), "park/target-death is not an actual per-call result");
            check(!may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/target-death keeps the buffer rule closed");
            check(!target_process_alive(target), "park/target-death the captured target is dead");

            release_remote_call(call);
            close_target_process(target);
            kill_writer_child(child);
        }
    }

    // 边等边死：park 正在进行时由另一个线程结束目标——"线程先 signal、进程后收尾"的窗口。
    if (wanted("park/target-death-mid-park")) {
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/target-death-mid-park could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/target-death-mid-park captured the target process handle");
            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc(child.pid, 144, buffer, buffer_error),
                  "park/target-death-mid-park buffer allocated");

            RemoteCall call = call_remote_export_on(target.process, child.dll_base,
                                                    child.export_info.rva,
                                                    reinterpret_cast<void*>(buffer.address), 250);
            check(call.thread_created && !call.signaled,
                  "park/target-death-mid-park the call is unresolved before the kill");

            KillerArgs args{child.process.hProcess, 200};
            HANDLE killer = ::CreateThread(nullptr, 0, kill_target_after_delay, &args, 0, nullptr);
            check(killer != nullptr, "park/target-death-mid-park spawned the killer thread");

            ParkResult const parked = park_remote_call(call, target.process, 50);
            if (killer != nullptr) {
                ::WaitForSingleObject(killer, 5000);
                ::CloseHandle(killer);
            }
            check(parked == ParkResult::TargetDied,
                  "park/target-death-mid-park reports the target death, not a clean thread exit");
            check(call.target_died && !call.signaled,
                  "park/target-death-mid-park is not reported as an actual thread exit");
            check(!may_release_remote_buffer(call.thread_created, call.signaled),
                  "park/target-death-mid-park keeps the buffer rule closed");

            release_remote_call(call);
            close_target_process(target);
            kill_writer_child(child);
        }
    }

    if (wanted("park/handle-no-leak")) {
        WriterChild child;
        if (!spawn_writer_child(child)) {
            check(false, "park/handle-no-leak could not spawn the delayed-writer child");
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(child.pid, 0, target, error),
                  "park/handle-no-leak captured the target process handle");
            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc(child.pid, 144, buffer, buffer_error), "park/handle-no-leak buffer allocated");

            DWORD before = 0;
            DWORD after = 0;
            int created = 0;
            // 第一轮当预热：进程里第一次 CreateRemoteThread 可能触发一次性的内部结构分配，把它
            // 排除在计数窗口之外，断言才能保持"精确相等"，而不是靠一个容差把真泄漏也放过去。
            for (int index = 0; index < 6; ++index) {
                RemoteCall call = call_remote_export_on(target.process, child.dll_base, child.export_info.rva,
                                                        reinterpret_cast<void*>(buffer.address), 30);
                if (call.thread_created) {
                    ++created;
                }
                release_remote_call(call);
                if (index == 0) {
                    ::GetProcessHandleCount(::GetCurrentProcess(), &before);
                }
            }
            ::GetProcessHandleCount(::GetCurrentProcess(), &after);
            check(created == 6, "park/handle-no-leak all six remote calls were created");
            check(after <= before, "park/handle-no-leak releasing unresolved calls leaks no handle");

            // 缓冲不释放：那五个写者可能还在写它（进程马上会被结束）。
            close_target_process(target);
            kill_writer_child(child);
        }
    }
}

// ---------------------------------------------------------------------------
// 状态门禁（N2）：不可信的查询结论绝不允许流入 Stop / eject / 重装
// ---------------------------------------------------------------------------

void run_state_gate_cases() {
    using namespace deskfeed::inject;
    if (!wanted("stategate/")) {
        return;
    }

    PayloadState const all[] = {PayloadState::NotMapped,       PayloadState::MappedUnknownAbi,
                                PayloadState::MappedUnverifiable, PayloadState::MappedInactive,
                                PayloadState::MappedActive,    PayloadState::MappedRetained};

    // 结论不可信：任何状态都不允许 Stop，也不允许"先停再装"。
    PayloadStatus status;
    status.conclusive = false;
    for (PayloadState const state : all) {
        status.state = state;
        check(!state_allows_stop(status), "stategate/inconclusive never allows stop");
        check(!state_allows_reload_stop(status), "stategate/inconclusive never allows reload");
    }

    // 结论可信：只有 Inactive 允许"先停再装"；Inactive/Active/Retained 允许 Stop。
    status.conclusive = true;
    status.state = PayloadState::MappedInactive;
    check(state_allows_stop(status), "stategate/inactive allows stop");
    check(state_allows_reload_stop(status), "stategate/inactive allows reload");

    status.state = PayloadState::MappedActive;
    check(state_allows_stop(status), "stategate/active allows stop");
    check(!state_allows_reload_stop(status), "stategate/active does not allow reload");

    status.state = PayloadState::MappedRetained;
    check(state_allows_stop(status), "stategate/retained allows a stop attempt");
    check(!state_allows_reload_stop(status), "stategate/retained does not allow reload");

    status.state = PayloadState::MappedUnverifiable;
    check(!state_allows_stop(status), "stategate/unverifiable never allows stop");
    check(!state_allows_reload_stop(status), "stategate/unverifiable never allows reload");

    status.state = PayloadState::MappedUnknownAbi;
    check(!state_allows_stop(status), "stategate/unknown-abi never allows stop");
    check(!state_allows_reload_stop(status), "stategate/unknown-abi never allows reload");

    // 新状态的名字要能被日志如实读出来。
    check_eq(std::string(payload_state_name(PayloadState::MappedUnverifiable)),
             std::string("mapped-unverifiable"), "stategate/unverifiable name");
}

// ---------------------------------------------------------------------------
// N3 租约（guard）：记录状态机、abandoned 恢复、单一释放谓词
//
// 全部离线：三个命名内核对象由**本进程**（keeper，扮演"负载 DLL 长期持有这三份引用"的角色）
// 持有；actor 跑在自己的短命线程里——线程在持锁时退出，内核就把它标成 abandoned，这是真的
// `WAIT_ABANDONED`，不是模拟。TID 存活事实用真的 Toolhelp。
// ---------------------------------------------------------------------------

// 扮演"负载侧长期持有三份引用"的那一方：只 open/create + 持有句柄，不抢租约。
using namespace deskfeed::inject;  // 本段（到本匿名 namespace 结束）全部是工具侧用例

struct GuardObjects {
    HANDLE mutex = nullptr;
    HANDLE event = nullptr;
    HANDLE mapping = nullptr;
    deskfeed::payload::GuardRecord* record = nullptr;
    GuardTarget target{};

    ~GuardObjects() { close(); }

    bool open(GuardTarget const& wanted, bool init_record, std::wstring& error) {
        namespace payload = deskfeed::payload;
        target = wanted;
        wchar_t buffer[payload::kGuardNameCapacity]{};
        auto const name = [&](wchar_t const* suffix, std::wstring& out) {
            if (!payload::guard_format_object_name(buffer, std::size(buffer), suffix,
                                                   wanted.process_id, wanted.creation_time)) {
                error = L"cannot format a guard object name";
                return false;
            }
            out.assign(buffer);
            return true;
        };
        std::wstring mutex_name;
        std::wstring event_name;
        std::wstring record_name;
        if (!name(payload::kGuardSuffixMutex, mutex_name) ||
            !name(payload::kGuardSuffixEvent, event_name) ||
            !name(payload::kGuardSuffixRecord, record_name)) {
            return false;
        }

        mutex = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
        event = ::CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        ::SetLastError(0);
        mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       payload::kGuardRecordSize, record_name.c_str());
        bool const created_now = mapping != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS;
        if (mutex == nullptr || event == nullptr || mapping == nullptr) {
            error = L"cannot create the guard objects";
            return false;
        }
        void* const view =
            ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, payload::kGuardRecordSize);
        if (view == nullptr) {
            error = L"cannot map the guard record";
            return false;
        }
        record = static_cast<payload::GuardRecord*>(view);
        if (created_now && init_record) {
            // 扮演"actor 先 create + init 再 LoadLibraryW"那条规约。不 init 的路径留给
            // `guard/zero-header`：那时的零头必须被当成未知。
            payload::guard_init_new_record(*record, 1);
        }
        return true;
    }

    void close() {
        if (record != nullptr) {
            ::UnmapViewOfFile(record);
            record = nullptr;
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
        if (event != nullptr) {
            ::CloseHandle(event);
            event = nullptr;
        }
        if (mutex != nullptr) {
            ::CloseHandle(mutex);
            mutex = nullptr;
        }
    }

    bool event_signaled() const {
        return event != nullptr && ::WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    }

    void set_event() const {
        if (event != nullptr) {
            ::SetEvent(event);
        }
    }
};

struct ActorOutcome {
    GuardAcquire outcome = GuardAcquire::Error;
    std::wstring error;
    GuardSnapshot snapshot_after = GuardSnapshot::Unknown;
    bool event_set_after = false;
    bool concluded = false;
};

// actor 在拿到租约之后、收尾之前要做的事（记录它自己那次远程调用）。
using guard_actor_setup = std::function<void(GuardLease&, ActorOutcome&)>;

// 一个 actor 的完整生命周期，跑在自己的线程里：
//   acquired -> （可选）记录一个 pending 状态 -> 收尾。
// `conclude` 为真时用谓词 `conclude_clean` 收尾；为假时**什么都不做**（线程退出 ⇒ abandoned），
// 这正是"崩溃的 actor"在核心里留下的样子。
void run_actor_thread(GuardTarget const& target, bool conclude, bool conclusive,
                      guard_actor_setup setup, ActorOutcome& out) {
    std::thread worker([&]() {
        GuardLease lease;
        std::wstring error;
        out.outcome = lease.acquire(target, error);
        out.error = error;
        if (!guard_acquire_allows_action(out.outcome)) {
            // Unknown/Busy：绝不动作，也绝不释放。
            lease.abandon_unknown();
            out.snapshot_after = lease.snapshot();
            return;
        }
        setup(lease, out);
        lease.note_action_conclusive(conclusive);
        if (conclude) {
            std::wstring conclude_error;
            out.concluded = lease.conclude_clean(conclude_error);
            if (!out.concluded) {
                out.error = conclude_error;
            }
        } else {
            lease.abandon_unknown();
        }
        out.snapshot_after = lease.snapshot();
    });
    worker.join();
}

// 直接读一次命名事件（不经过租约）：给"粘性事件真的被置位/清掉"当独立证据。
bool guard_event_signaled(GuardTarget const& target) {
    namespace payload = deskfeed::payload;
    wchar_t buffer[payload::kGuardNameCapacity]{};
    if (!payload::guard_format_object_name(buffer, std::size(buffer), payload::kGuardSuffixEvent,
                                           target.process_id, target.creation_time)) {
        return false;
    }
    HANDLE event = ::OpenEventW(SYNCHRONIZE, FALSE, buffer);
    if (event == nullptr) {
        return false;
    }
    bool const signaled = ::WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    ::CloseHandle(event);
    return signaled;
}

std::uint64_t synthetic_creation(int index) {
    return 0x01DA000000000000ull + static_cast<std::uint64_t>(index) * 0x1000ull;
}

// 跑一个 actor（自己的线程）并返回它读到的记录形状。
ActorOutcome run_guard_actor(GuardTarget const& target, bool conclude, bool conclusive,
                             guard_actor_setup setup) {
    ActorOutcome out;
    run_actor_thread(target, conclude, conclusive, setup, out);
    out.event_set_after = guard_event_signaled(target);
    return out;
}

void run_guard_cases() {
    namespace payload = deskfeed::payload;

    // ---- 纯判定表：记录语义 + 名字格式 + 恢复映射 -------------------------------
    if (wanted("guard/table")) {
        // 名字：唯一出处生成，pid 十进制无前导零，creation 恰好 16 位小写十六进制。
        wchar_t buffer[payload::kGuardNameCapacity]{};
        check(payload::guard_format_object_name(buffer, std::size(buffer),
                                                payload::kGuardSuffixMutex, 4321,
                                                0x01DA123456789ABCull),
              "guard/table formats a mutex object name");
        std::wstring const expected =
            L"Local\\DeskFeed-payload-guard-4321-01da123456789abc-mutex";
        check(std::wstring(buffer) == expected, "guard/table the name is exactly the frozen format");
        check(payload::guard_format_object_name(buffer, 10, payload::kGuardSuffixMutex, 4321,
                                                0x01DA123456789ABCull) == false,
              "guard/table refuses to truncate a name into a different object");
        check(!payload::guard_format_object_name(nullptr, 64, payload::kGuardSuffixMutex, 1, 1),
              "guard/table rejects a null buffer");

        // 记录：init 之后是可信 Idle；三个写函数只改快照、头保持不变。
        payload::GuardRecord record{};
        payload::guard_init_new_record(record, 1);
        check(payload::guard_header_ok(record), "guard/table a new record has a good header");
        check(payload::guard_snapshot_ok(record), "guard/table a new record has a good snapshot");
        check(payload::guard_classify(record) == GuardSnapshot::Idle,
              "guard/table a new record classifies as Idle");

        payload::guard_make_pending_no_tid(record, 2);
        check(payload::guard_classify(record) == GuardSnapshot::PendingNoTid,
              "guard/table pending-no-tid classifies");
        check(record.tid == 0, "guard/table pending-no-tid carries no tid");
        payload::guard_make_pending_tid(record, 0x1234, 3);
        check(payload::guard_classify(record) == GuardSnapshot::PendingTid,
              "guard/table pending-tid classifies");
        check(record.tid == 0x1234, "guard/table pending-tid carries the tid");

        // 逐项拒绝：头、reserved、state、tid 自洽、checksum。
        payload::GuardRecord bad = record;
        bad.size = 39;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects a wrong record size");
        bad = record;
        bad.version = 2;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects an unknown record version");
        bad = record;
        bad.reserved = 1;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects a non-zero reserved field");
        bad = record;
        bad.reserved2 = 1;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects a non-zero reserved2 field");
        bad = record;
        bad.state = 3;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects an unknown state value");
        bad = record;
        bad.tid = 0;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects pending-tid without a tid");
        bad = record;
        bad.tid = payload::kGuardMaxTid + 1;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects a tid above 32 bits (cannot be probed honestly)");
        bad = record;
        bad.tid = 0x99;
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects a torn snapshot (checksum mismatch)");
        bad = record;
        payload::guard_make_idle(bad, 9);
        bad.tid = 7;
        bad.checksum = payload::guard_snapshot_fnv(bad);
        check(payload::guard_classify(bad) == GuardSnapshot::Unknown,
              "guard/table rejects Idle carrying a tid");
        payload::GuardRecord zero{};
        check(payload::guard_classify(zero) == GuardSnapshot::Unknown,
              "guard/table classifies an all-zero record as Unknown (fail closed)");

        // 恢复表：唯一出处 guard_recovery + actor 侧的获取映射。
        check(payload::guard_recovery(GuardSnapshot::Idle, GuardTidProbe::NotQueried) ==
                  GuardRecovery::ClearAndContinue,
              "guard/table Idle recovers");
        check(payload::guard_recovery(GuardSnapshot::PendingTid, GuardTidProbe::Absent) ==
                  GuardRecovery::ClearAndContinue,
              "guard/table a gone entry thread recovers");
        check(payload::guard_recovery(GuardSnapshot::PendingTid, GuardTidProbe::Present) ==
                  GuardRecovery::KeepUncertain,
              "guard/table a live entry thread stays uncertain");
        check(payload::guard_recovery(GuardSnapshot::PendingTid, GuardTidProbe::LookupFailed) ==
                  GuardRecovery::KeepUncertain,
              "guard/table a failed tid lookup stays uncertain");
        check(payload::guard_recovery(GuardSnapshot::PendingTid, GuardTidProbe::NotQueried) ==
                  GuardRecovery::KeepUncertain,
              "guard/table pending-tid without a probe stays uncertain");
        check(payload::guard_recovery(GuardSnapshot::PendingNoTid, GuardTidProbe::Absent) ==
                  GuardRecovery::KeepUncertain,
              "guard/table pending-no-tid never recovers by itself");
        check(payload::guard_recovery(GuardSnapshot::Unknown, GuardTidProbe::Absent) ==
                  GuardRecovery::KeepUncertain,
              "guard/table an unreadable record never recovers");

        check(guard_acquire_from_recovery(GuardSnapshot::Idle, GuardTidProbe::NotQueried) ==
                  GuardAcquire::Recovered,
              "guard/table Idle maps to Recovered");
        check(guard_acquire_from_recovery(GuardSnapshot::PendingNoTid, GuardTidProbe::Absent) ==
                  GuardAcquire::Unknown,
              "guard/table pending-no-tid maps to Unknown");
        check(guard_acquire_allows_action(GuardAcquire::Acquired) &&
                  guard_acquire_allows_action(GuardAcquire::Recovered),
              "guard/table only Acquired/Recovered allow the action");
        check(!guard_acquire_allows_action(GuardAcquire::Busy) &&
                  !guard_acquire_allows_action(GuardAcquire::Unknown) &&
                  !guard_acquire_allows_action(GuardAcquire::Error),
              "guard/table Busy/Unknown/Error never allow the action");

        // Toolhelp 事实：零/超限 TID 一律不可证；不存在的 TID 是 Absent。
        check(guard_probe_thread(::GetCurrentProcessId(), 0) == GuardTidProbe::LookupFailed,
              "guard/table a zero tid is not provably absent");
        check(guard_probe_thread(::GetCurrentProcessId(), payload::kGuardMaxTid + 1) ==
                  GuardTidProbe::LookupFailed,
              "guard/table an over-wide tid is not provably absent");
        check(guard_probe_thread(::GetCurrentProcessId(), 0xF0000000ull) == GuardTidProbe::Absent,
              "guard/table a tid that cannot exist is absent");
        check(guard_probe_thread(::GetCurrentProcessId(), ::GetCurrentThreadId()) ==
                  GuardTidProbe::Present,
              "guard/table the current thread is present in its own process");
    }

    // ---- 崩掉的 actor 留下 really-abandoned：Idle 可以解除 ----------------------
    if (wanted("guard/abandoned-idle")) {
        GuardTarget const target{39001, synthetic_creation(1)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error), "guard/abandoned-idle keeper holds the objects");

        ActorOutcome const crasher = run_guard_actor(
            target, false, false, [](GuardLease& lease, ActorOutcome& out) {
                std::wstring write_error;
                if (!lease.note_remote_call_settled(write_error)) {
                    out.error = write_error;
                }
            });
        check(crasher.outcome == GuardAcquire::Acquired,
              "guard/abandoned-idle the first actor acquired the lease");
        keeper.set_event();  // 模拟"上一次未知"残留：干净恢复必须把它清掉

        ActorOutcome const recovered = run_guard_actor(
            target, true, true, [](GuardLease&, ActorOutcome&) {});
        check(recovered.outcome == GuardAcquire::Recovered,
              "guard/abandoned-idle a crashed-actor Idle record recovers");
        check(recovered.concluded, "guard/abandoned-idle the recovering actor concludes clean");
        check(!guard_event_signaled(target),
              "guard/abandoned-idle recovery clears the sticky uncertainty event");
        check(keeper.record->state == payload::kGuardStateIdle,
              "guard/abandoned-idle the record is rewritten to Idle");
    }

    // ---- PendingNoTid：本会话永久不确定，事件保持 ------------------------------
    if (wanted("guard/abandoned-pending-no-tid")) {
        GuardTarget const target{39002, synthetic_creation(2)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error), "guard/abandoned-pending-no-tid keeper holds objects");

        ActorOutcome const crasher = run_guard_actor(
            target, false, false, [](GuardLease& lease, ActorOutcome& out) {
                std::wstring write_error;
                if (!lease.note_remote_call_pending(write_error)) {
                    out.error = write_error;
                }
            });
        check(crasher.outcome == GuardAcquire::Acquired,
              "guard/abandoned-pending-no-tid the first actor acquired");

        ActorOutcome const blocked = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(blocked.outcome == GuardAcquire::Unknown,
              "guard/abandoned-pending-no-tid a pending-no-tid crash stays Unknown");
        check(!guard_acquire_allows_action(blocked.outcome),
              "guard/abandoned-pending-no-tid the next actor must not touch the target");
        check(guard_event_signaled(target),
              "guard/abandoned-pending-no-tid the sticky uncertainty event is set");
        check(keeper.record->state == payload::kGuardStatePendingNoTid &&
                  keeper.record->tid == 0,
              "guard/abandoned-pending-no-tid the pending record is left untouched");
    }

    // ---- PendingTid + 该 TID 真的没了：可以解除 --------------------------------
    if (wanted("guard/abandoned-pending-tid-dead")) {
        GuardTarget const target{39003, synthetic_creation(3)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error),
              "guard/abandoned-pending-tid-dead keeper holds objects");

        // 记录的是 crasher 那条线程自己的 TID：线程退出后它必然不在。
        ActorOutcome const crasher = run_guard_actor(
            target, false, false, [](GuardLease& lease, ActorOutcome& out) {
                std::wstring write_error;
                if (!lease.note_remote_call_started(::GetCurrentThreadId(), write_error)) {
                    out.error = write_error;
                }
            });
        check(crasher.outcome == GuardAcquire::Acquired,
              "guard/abandoned-pending-tid-dead the first actor acquired");
        check(keeper.record->state == payload::kGuardStatePendingTid,
              "guard/abandoned-pending-tid-dead the record names the entry thread");

        ActorOutcome const recovered = run_guard_actor(
            target, true, true, [](GuardLease&, ActorOutcome&) {});
        check(recovered.outcome == GuardAcquire::Recovered,
              "guard/abandoned-pending-tid-dead a gone entry thread recovers");
        check(!guard_event_signaled(target),
              "guard/abandoned-pending-tid-dead recovery clears the sticky event");
    }

    // ---- PendingTid + 该 TID 仍然活着：拒绝，直到它真的没了 --------------------
    if (wanted("guard/abandoned-pending-tid-live")) {
        GuardTarget target{};
        std::wstring error;
        check(guard_target_identity(::GetCurrentProcessId(), target, error),
              "guard/abandoned-pending-tid-live captured this process identity");

        // 一条真的活着、而且稍后会被结束的线程：它扮演"还在模块里执行的入口线程"。
        HANDLE victim = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> stop_victim{false};
        std::thread victim_thread([&]() {
            while (!stop_victim.load()) {
                ::WaitForSingleObject(victim, 10);
            }
        });
        unsigned long const victim_tid = ::GetThreadId(victim_thread.native_handle());

        GuardObjects keeper;
        check(keeper.open(target, true, error),
              "guard/abandoned-pending-tid-live keeper holds objects");

        ActorOutcome const crasher = run_guard_actor(
            target, false, false, [victim_tid](GuardLease& lease, ActorOutcome& out) {
                std::wstring write_error;
                if (!lease.note_remote_call_started(victim_tid, write_error)) {
                    out.error = write_error;
                }
            });
        check(crasher.outcome == GuardAcquire::Acquired,
              "guard/abandoned-pending-tid-live the first actor acquired");

        ActorOutcome const blocked = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(blocked.outcome == GuardAcquire::Unknown,
              "guard/abandoned-pending-tid-live a live entry thread keeps the target uncertain");
        check(guard_event_signaled(target),
              "guard/abandoned-pending-tid-live the sticky event is set");

        // 让那条入口线程真的退出，再试一次：这一次记录能证明干净。
        stop_victim.store(true);
        victim_thread.join();
        ::CloseHandle(victim);

        ActorOutcome const recovered = run_guard_actor(
            target, true, true, [](GuardLease&, ActorOutcome&) {});
        check(recovered.outcome == GuardAcquire::Recovered,
              "guard/abandoned-pending-tid-live recovery happens once the entry thread is gone");
        check(!guard_event_signaled(target),
              "guard/abandoned-pending-tid-live recovery clears the sticky event");
    }

    // ---- 零 header：未知，绝不当成"没有不确定性"也不许覆盖 ---------------------
    if (wanted("guard/zero-header")) {
        GuardTarget const target{39004, synthetic_creation(4)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, false, error), "guard/zero-header keeper created a zero record");

        ActorOutcome const blocked = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(blocked.outcome == GuardAcquire::Unknown,
              "guard/zero-header a zero record is Unknown");
        check(guard_event_signaled(target),
              "guard/zero-header the sticky event is set for a zero record");
        check(keeper.record->size == 0 && keeper.record->version == 0,
              "guard/zero-header the unknown record is left exactly as it was");
    }

    // ---- 坏 version：未知，且不许自愈覆盖 --------------------------------------
    if (wanted("guard/bad-version")) {
        GuardTarget const target{39005, synthetic_creation(5)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error), "guard/bad-version keeper created a good record");
        keeper.record->version = 99;  // 模拟旧格式/别人写的记录
        keeper.record->checksum = payload::guard_snapshot_fnv(*keeper.record);

        ActorOutcome const blocked = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(blocked.outcome == GuardAcquire::Unknown,
              "guard/bad-version an unknown record version is Unknown");
        check(keeper.record->version == 99,
              "guard/bad-version the unknown record is never self-healed or overwritten");
        check(guard_event_signaled(target),
              "guard/bad-version the sticky event is set for a bad version");
    }

    // ---- 释放谓词：只有一个释放点，而且要有可观测量 ----------------------------
    if (wanted("guard/release-predicate")) {
        GuardTarget const target{39006, synthetic_creation(6)};
        GuardLease holder;
        std::wstring error;
        check(holder.acquire(target, error) == GuardAcquire::Acquired,
              "guard/release-predicate the first actor acquired");

        // 第二个 actor 用 0 超时去抢：拿不到就是"上一位还没释放"的可观测证据。
        ActorOutcome const busy = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(busy.outcome == GuardAcquire::Busy,
              "guard/release-predicate a second actor cannot enter while the lease is held");

        // 有未决远程调用：谓词不成立，绝不释放。
        std::wstring write_error;
        check(holder.note_remote_call_pending(write_error),
              "guard/release-predicate recorded a pending remote call");
        holder.note_action_conclusive(true);
        std::wstring conclude_error;
        check(!holder.conclude_clean(conclude_error),
              "guard/release-predicate refuses to release while a call is pending");
        ActorOutcome const still_busy = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(still_busy.outcome == GuardAcquire::Busy,
              "guard/release-predicate a refused release really did not release");

        // 动作没定论：同样不释放。
        check(holder.note_remote_call_settled(write_error),
              "guard/release-predicate the pending call settled");
        holder.note_action_conclusive(false);
        check(!holder.conclude_clean(conclude_error),
              "guard/release-predicate refuses to release an inconclusive action");
        ActorOutcome const busy_again = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(busy_again.outcome == GuardAcquire::Busy,
              "guard/release-predicate an inconclusive action keeps holding the lease");

        // 谓词成立：释放，第二个 actor 立刻能进来。
        holder.note_action_conclusive(true);
        check(holder.conclude_clean(conclude_error),
              "guard/release-predicate releases once the predicate holds");
        ActorOutcome const admitted = run_guard_actor(
            target, true, true, [](GuardLease&, ActorOutcome&) {});
        check(admitted.outcome == GuardAcquire::Acquired,
              "guard/release-predicate the next actor is admitted after a clean release");
    }

    // ---- 同线程递归 acquire 不能把粘性未知抹掉（Win32 mutex 是递归的） -------------
    if (wanted("guard/recursive-same-thread")) {
        GuardTarget const target{39007, synthetic_creation(7)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error), "guard/recursive-same-thread keeper holds objects");

        ActorOutcome first;
        GuardAcquire second_outcome = GuardAcquire::Error;
        std::wstring second_error;
        std::atomic<bool> ready{false};
        std::atomic<bool> proceed{false};

        std::thread t1([&]() {
            GuardLease lease1;
            std::wstring acquire_error;
            first.outcome = lease1.acquire(target, acquire_error);
            first.error = acquire_error;
            if (first.outcome != GuardAcquire::Acquired) {
                ready.store(true);  // 失败也要放主线程往下走，绝不挂住用例
                return;
            }
            // 记录留在 Idle，但这一次动作**没有定论**：这正是"watch 第一轮崩在收尾之前"的形状。
            lease1.abandon_unknown();
            ready.store(true);
            while (!proceed.load()) {
                ::Sleep(1);  // 先让主线程观察到"另一条线程被挡在外面"
            }
            // 同一条线程再来一次：这里拿到的是 WAIT_OBJECT_0（递归），绝不能被当成"良性残留"。
            GuardLease lease2;
            std::wstring second_acquire_error;
            second_outcome = lease2.acquire(target, second_acquire_error);
            second_error = second_acquire_error;
            if (lease2.held()) {
                lease2.abandon_unknown();
            }
        });

        while (!ready.load()) {
            ::Sleep(1);
        }
        ActorOutcome const held_out = run_guard_actor(
            target, false, false, [](GuardLease&, ActorOutcome&) {});
        check(held_out.outcome == GuardAcquire::Busy,
              "guard/recursive-same-thread another thread is held out while the owner thread lives");

        proceed.store(true);
        t1.join();  // 原线程真的结束：内核这才把 mutex 标成 abandoned

        check(first.outcome == GuardAcquire::Acquired,
              "guard/recursive-same-thread the first acquire succeeded");
        check(second_outcome == GuardAcquire::Unknown,
              "guard/recursive-same-thread a same-thread recursive acquire must not treat the sticky "
              "event as a benign leftover");
        check(guard_event_signaled(target),
              "guard/recursive-same-thread the sticky event survives the recursive acquire");
        check(keeper.record->state == payload::kGuardStateIdle,
              "guard/recursive-same-thread the record is still the abandoned Idle (no self-heal)");

        ActorOutcome const recovered = run_guard_actor(
            target, true, true, [](GuardLease&, ActorOutcome&) {});
        check(recovered.outcome == GuardAcquire::Recovered,
              "guard/recursive-same-thread recovery happens only after the owner thread really ended");
        check(!guard_event_signaled(target),
              "guard/recursive-same-thread recovery clears the sticky event");
    }

    // ---- abandon 过的实例绝不允许复用（复用会关掉事实句柄 + 同线程递归） -----------
    if (wanted("guard/no-instance-reuse")) {
        GuardTarget const target{39008, synthetic_creation(8)};
        GuardObjects keeper;
        std::wstring error;
        check(keeper.open(target, true, error), "guard/no-instance-reuse keeper holds objects");

        GuardLease instance;
        check(instance.acquire(target, error) == GuardAcquire::Acquired,
              "guard/no-instance-reuse acquired once");
        instance.abandon_unknown();

        std::wstring reuse_error;
        check(instance.acquire(target, reuse_error) == GuardAcquire::Error,
              "guard/no-instance-reuse refuses to reuse an abandoned instance");
        check(reuse_error.find(L"must not be reused") != std::wstring::npos,
              "guard/no-instance-reuse explains why it must not be reused");
        check(guard_event_signaled(target),
              "guard/no-instance-reuse the refused reuse left the fact handles (and the event) alone");
    }
}

// ---------------------------------------------------------------------------
// N3b：把租约与 parking 真的接进动作（load / stop / status / watch 的每一轮）
//
// 这一组咬的是**动作路径**，不是纯函数：真实的隐藏夹具主机（`tools_fixture_host.cpp`）+ 真实的夹具
// 负载 DLL（`fixture_payload.cpp`，导出 `DeskFeedStart` / `DeskFeedStop` / `DeskFeedGetStatus`）
// + 真实由 `CreateRemoteThread` 打进去的入口线程。用例一边跑动作，一边直接读**命名记录**，看它
// 从 `PendingTid` 走回 `Idle`；以及看"没有结论"时租约绝不被释放。
//
// 绝不碰真实 explorer.exe：目标恒为用例自己 spawn 的夹具主机。
// ---------------------------------------------------------------------------

std::wstring fixture_dir() {
    // 缺省：与本可执行文件同目录（ROOT 构建树里夹具与用例同目录）。
    // 覆盖：DESKFEED_TOOLS_FIXTURE_DIR（本地隔离构建树里夹具被放到别处时用）。
    wchar_t buffer[32768]{};
    DWORD const length = ::GetEnvironmentVariableW(L"DESKFEED_TOOLS_FIXTURE_DIR", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    std::wstring dir;
    if (length > 0 && length < std::size(buffer)) {
        dir.assign(buffer, length);
    } else {
        std::wstring const self = deskfeed::module_self_path();
        std::wstring::size_type const slash = self.find_last_of(L"\\/");
        dir = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    return dir;
}

std::wstring fixture_payload_path() {
    return fixture_dir() + L"\\deskfeed-test-fixture-payload.dll";
}

std::wstring fixture_host_path() {
    return fixture_dir() + L"\\deskfeed-tools-fixture-host.exe";
}

// 给夹具主机进程拼一份环境块（只覆盖夹具开关；其余照抄本进程，保证 PATH 等还在）。
std::wstring build_environment(std::vector<std::pair<std::wstring, std::wstring>> const& overrides) {
    std::vector<std::wstring> entries;
    LPWCH const block = ::GetEnvironmentStringsW();
    if (block != nullptr) {
        for (wchar_t const* entry = block; *entry != L'\0'; entry += ::wcslen(entry) + 1) {
            std::wstring text(entry);
            bool replaced = false;
            for (auto const& pair : overrides) {
                if (text.size() > pair.first.size() &&
                    ::_wcsnicmp(text.c_str(), pair.first.c_str(), pair.first.size()) == 0 &&
                    text[pair.first.size()] == L'=') {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                entries.push_back(text);
            }
        }
        ::FreeEnvironmentStringsW(block);
    }
    for (auto const& pair : overrides) {
        entries.push_back(pair.first + L"=" + pair.second);
    }
    std::wstring result;
    for (auto const& entry : entries) {
        result += entry;
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

struct FixtureHost {
    PROCESS_INFORMATION process{};
    unsigned long pid = 0;
    std::uint64_t creation_time = 0;
    unsigned long long dll_base = 0;
    deskfeed::inject::PeExportInfo status_export;
    deskfeed::inject::PeExportInfo start_export;
    deskfeed::inject::PeExportInfo stop_export;
    bool ready = false;

    ~FixtureHost() { kill(); }

    // 只终止**本用例自己** CreateProcessW 出来的那个 PID，绝不按镜像名杀进程。
    void kill() {
        if (process.hProcess != nullptr) {
            ::TerminateProcess(process.hProcess, 0);
            ::WaitForSingleObject(process.hProcess, 5000);
            ::CloseHandle(process.hProcess);
            process.hProcess = nullptr;
        }
        if (process.hThread != nullptr) {
            ::CloseHandle(process.hThread);
            process.hThread = nullptr;
        }
        pid = 0;
    }
};

bool spawn_fixture_host(FixtureHost& host,
                        std::vector<std::pair<std::wstring, std::wstring>> const& env,
                        bool load_fixture) {
    using namespace deskfeed::inject;
    host = FixtureHost{};
    std::wstring const dll = fixture_payload_path();
    std::wstring const exe = fixture_host_path();
    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES ||
        ::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    std::wstring command = L"\"" + exe + L"\"";
    if (load_fixture) {
        command += L" --load-fixture \"" + dll + L"\"";
    }
    command += L" --parent " + std::to_wstring(::GetCurrentProcessId());
    command += L" --lifetime-ms 120000";

    std::wstring const block = build_environment(env);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                         const_cast<wchar_t*>(block.c_str()), nullptr, &startup,
                         &host.process) == FALSE) {
        return false;
    }
    host.pid = host.process.dwProcessId;

    std::wstring time_error;
    if (!process_creation_time(host.pid, host.creation_time, time_error)) {
        return false;
    }

    if (load_fixture) {
        for (int attempt = 0; attempt < 100 && host.dll_base == 0; ++attempt) {
            ::Sleep(50);
            std::vector<ModuleEntry> modules;
            std::wstring list_error;
            if (list_modules(host.pid, modules, list_error)) {
                for (ModuleEntry const& entry : modules) {
                    if (module_path_matches(entry.path, dll)) {
                        host.dll_base = entry.base;
                    }
                }
            }
        }
        if (host.dll_base == 0) {
            return false;
        }
    }

    std::wstring error;
    PeExportError kind = PeExportError::None;
    host.ready = pe_export_rva(dll, "DeskFeedGetStatus", host.status_export, error, &kind) &&
                 pe_export_rva(dll, "DeskFeedStart", host.start_export, error, &kind) &&
                 pe_export_rva(dll, "DeskFeedStop", host.stop_export, error, &kind);
    return host.ready;
}

deskfeed::inject::ActionContext fixture_context(FixtureHost const& host) {
    deskfeed::inject::ActionContext context;
    context.process_id = host.pid;
    context.creation_time = host.creation_time;
    context.other_image_name = L"deskfeed-tools-fixture-host.exe";
    return context;
}

int g_gate_serial = 0;

std::wstring make_gate_name() {
    return L"Local\\DeskFeedFixtureGate-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(++g_gate_serial);
}

// 只读地看那份**命名记录**（与 actor 持锁写的同一块跨进程内存）。读端可能撞上写端的快照，
// 所以每次读都用 `guard_classify` 的 checksum 验一遍，撞上撕裂就重读。
struct GuardRecordWatch {
    HANDLE mapping = nullptr;
    void* view = nullptr;

    bool open(deskfeed::inject::GuardTarget const& target) {
        namespace payload = deskfeed::payload;
        wchar_t name[payload::kGuardNameCapacity]{};
        if (!payload::guard_format_object_name(name, std::size(name), payload::kGuardSuffixRecord,
                                               target.process_id, target.creation_time)) {
            return false;
        }
        mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (mapping == nullptr) {
            return false;
        }
        view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, payload::kGuardRecordSize);
        return view != nullptr;
    }

    void close() {
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    bool read(deskfeed::payload::GuardRecord& out) const {
        if (view == nullptr) {
            return false;
        }
        std::memcpy(&out, view, sizeof(out));
        return true;
    }

    bool wait_for(deskfeed::payload::GuardSnapshot wanted, int timeout_ms,
                  deskfeed::payload::GuardRecord* last = nullptr) {
        for (int waited = 0; waited <= timeout_ms; waited += 10) {
            deskfeed::payload::GuardRecord record{};
            if (read(record)) {
                if (last != nullptr) {
                    *last = record;
                }
                if (deskfeed::payload::guard_classify(record) == wanted) {
                    return true;
                }
            }
            ::Sleep(10);
        }
        return false;
    }
};

void run_leased_action_cases() {
    using namespace deskfeed::inject;
    namespace payload = deskfeed::payload;

    // ---- 受租约保护的一次调用：记录真的走到 PendingTid，再随真实 signal 回到 Idle --------
    if (wanted("leasecall/record-and-park")) {
        FixtureHost host;
        std::wstring const gate_name = make_gate_name();
        HANDLE const gate = ::CreateEventW(nullptr, TRUE, FALSE, gate_name.c_str());
        if (gate == nullptr || !spawn_fixture_host(host, {{L"DESKFEED_FIXTURE_STATUS_GATE", gate_name}},
                                                   /*load_fixture=*/true)) {
            check(false, "leasecall/record-and-park could not set up the fixture host");
            if (gate != nullptr) {
                ::CloseHandle(gate);
            }
        } else {
            TargetProcess target;
            std::wstring error;
            check(open_target_process(host.pid, host.creation_time, target, error),
                  "leasecall/record-and-park captured the target process");
            GuardTarget const guard_target{target.process_id, target.creation_time};
            GuardRecordWatch watch;

            RemoteBuffer buffer;
            std::wstring buffer_error;
            check(remote_alloc_on(target.process, sizeof(payload::PayloadStatusPod), buffer,
                                  buffer_error),
                  "leasecall/record-and-park allocated the POD buffer in the target");
            // 调用方先写好 ABI 头（真实负载也按这条契约判断自己认不认这份布局）。
            payload::PayloadStatusPod initial{};
            initial.size = sizeof(initial);
            initial.abi = payload::kPayloadStatusAbi;
            initial.width = payload::kPayloadStatusWidth;
            check(remote_write_on(target.process, buffer.address, &initial, sizeof(initial),
                                  buffer_error),
                  "leasecall/record-and-park wrote the POD header into the target");

            LeaseCall call;
            GuardAcquire acquired = GuardAcquire::Error;
            bool concluded = false;
            std::atomic<bool> acquired_flag{false};
            std::thread actor([&]() {
                GuardLease lease;
                std::wstring acquire_error;
                acquired = lease.acquire(guard_target, acquire_error);
                if (!guard_acquire_allows_action(acquired)) {
                    acquired_flag.store(true);
                    return;
                }
                acquired_flag.store(true);
                // 真实的"超时未决"：闸门还没放行，200 ms 的等待必然超时，线程句柄被保留下来。
                call = run_leased_call(lease, target, 50, [&]() -> RemoteCall {
                    return call_remote_export_on(target.process, host.dll_base,
                                                 host.status_export.rva,
                                                 reinterpret_cast<void*>(buffer.address), 200);
                });
                lease.note_action_conclusive(true);
                std::wstring conclude_error;
                concluded = lease.conclude_clean(conclude_error);
            });

            while (!acquired_flag.load()) {
                ::Sleep(1);
            }
            check(acquired == GuardAcquire::Acquired,
                  "leasecall/record-and-park the actor acquired the lease");
            check(watch.open(guard_target),
                  "leasecall/record-and-park opened the named record the actor created");

            payload::GuardRecord pending_record{};
            check(watch.wait_for(payload::GuardSnapshot::PendingTid, 4000, &pending_record),
                  "leasecall/record-and-park the named record is PendingTid while the callee is "
                  "still inside the target");
            check(pending_record.tid != 0,
                  "leasecall/record-and-park the record carries the real entry tid");
            check(guard_probe_thread(host.pid, pending_record.tid) == GuardTidProbe::Present,
                  "leasecall/record-and-park that tid is really alive inside the target");

            // 放行：夹具写完 POD 再返回 ⇒ park 必须等到**真实** signal。
            ::SetEvent(gate);
            actor.join();

            check(call.outcome == LeaseCallOutcome::Completed && call.call.completed_late,
                  "leasecall/record-and-park parking observed the real (late) completion");
            check(call.call.exit_code == 1,
                  "leasecall/record-and-park the real exit code was read after the timeout");
            check(concluded, "leasecall/record-and-park the actor concluded clean");
            check(watch.wait_for(payload::GuardSnapshot::Idle, 3000),
                  "leasecall/record-and-park the record is back to Idle");
            check(!guard_event_signaled(guard_target),
                  "leasecall/record-and-park no sticky uncertainty was left behind");

            payload::PayloadStatusPod pod{};
            check(remote_read_on(target.process, buffer.address, &pod, sizeof(pod), buffer_error),
                  "leasecall/record-and-park the caller's buffer is still there");
            check(pod.size == sizeof(pod) && pod.abi == payload::kPayloadStatusAbi,
                  "leasecall/record-and-park the late 144-byte write landed intact");

            remote_free_on(target.process, buffer);
            watch.close();
            close_target_process(target);
            ::CloseHandle(gate);
            host.kill();
        }
    }

    // ---- 完整动作路径：stop 旧的（真实 FreeLibrary）→ 再 LoadLibraryW → Start ------------------
    if (wanted("action/load-stop-start")) {
        FixtureHost host;
        std::vector<std::pair<std::wstring, std::wstring>> const env = {
            {L"DESKFEED_FIXTURE_STATUS_STARTED", L"0"},
            {L"DESKFEED_FIXTURE_STATUS_UNLOAD_SAFE", L"1"},
            {L"DESKFEED_FIXTURE_STOP_CODE", L"1"},
            {L"DESKFEED_FIXTURE_START_CODE", L"1"},
        };
        if (!spawn_fixture_host(host, env, /*load_fixture=*/true)) {
            check(false, "action/load-stop-start could not set up the fixture host");
        } else {
            ActionContext const context = fixture_context(host);
            Options options;
            options.action = L"load";
            options.payload_path = fixture_payload_path();
            options.scope = L"fixture-" + std::to_wstring(::GetCurrentProcessId());
            int const code = do_load_in(context, options);
            check_eq(code, 0,
                     "action/load-stop-start the load succeeds through the real action path");

            ModuleEntry module;
            std::wstring find_error;
            check(lookup_module_by_full_path(host.pid, fixture_payload_path(), module, find_error) ==
                      ModuleLookup::Found,
                  "action/load-stop-start the payload is mapped in the target after the load");

            GuardTarget const guard_target{host.pid, host.creation_time};
            check(!guard_event_signaled(guard_target),
                  "action/load-stop-start the lease was released cleanly (no sticky uncertainty)");
            host.kill();
        }
    }

    // ---- 未退休（Stop 报 0）绝不 FreeLibrary：报告 retained，模块保持映射 ----------------------
    if (wanted("action/stop-retained-no-eject")) {
        FixtureHost host;
        std::vector<std::pair<std::wstring, std::wstring>> const env = {
            {L"DESKFEED_FIXTURE_STATUS_STARTED", L"1"},
            {L"DESKFEED_FIXTURE_STATUS_THREAD_ALIVE", L"1"},
            {L"DESKFEED_FIXTURE_STATUS_UNLOAD_SAFE", L"1"},
            {L"DESKFEED_FIXTURE_STOP_CODE", L"0"},  // "我没退休"
        };
        if (!spawn_fixture_host(host, env, /*load_fixture=*/true)) {
            check(false, "action/stop-retained-no-eject could not set up the fixture host");
        } else {
            ActionContext const context = fixture_context(host);
            Options options;
            options.action = L"stop";
            options.payload_path = fixture_payload_path();
            options.scope = L"fixture-" + std::to_wstring(::GetCurrentProcessId());
            int const code = do_stop_in(context, options);
            check_eq(code, 1,
                     "action/stop-retained-no-eject an unretired payload is reported as a failure");

            ModuleEntry module;
            std::wstring find_error;
            check(lookup_module_by_full_path(host.pid, fixture_payload_path(), module, find_error) ==
                      ModuleLookup::Found,
                  "action/stop-retained-no-eject the DLL is still mapped (no FreeLibrary on an "
                  "unretired payload)");
            host.kill();
        }
    }

    // ---- 租约门禁真的在动作里：Busy 不给结论、同线程递归不抹掉粘性未知 -------------------------
    if (wanted("action/status-lease")) {
        FixtureHost host;
        if (!spawn_fixture_host(host, {{L"DESKFEED_FIXTURE_STATUS_STARTED", L"0"}},
                                /*load_fixture=*/true)) {
            check(false, "action/status-lease could not set up the fixture host");
        } else {
            ActionContext const context = fixture_context(host);
            Options options;
            options.action = L"status";
            options.payload_path = fixture_payload_path();
            options.scope = L"fixture-" + std::to_wstring(::GetCurrentProcessId());

            GuardTarget const guard_target{host.pid, host.creation_time};
            GuardLease holder;
            std::wstring error;
            check(holder.acquire(guard_target, error) == GuardAcquire::Acquired,
                  "action/status-lease the test acquired the lease");

            // 另一位 actor 在**另一条线程**上问状态：拿不到租约 ⇒ 只报告"问不出来"，绝不动作。
            int busy_code = 0;
            std::thread other([&]() { busy_code = do_status_in(context, options); });
            other.join();
            check_eq(busy_code, 2,
                     "action/status-lease a busy lease makes status say 'cannot tell'");

            std::wstring conclude_error;
            holder.note_action_conclusive(true);
            check(holder.conclude_clean(conclude_error),
                  "action/status-lease the test released its lease cleanly");
            check_eq(do_status_in(context, options), 1,
                     "action/status-lease status answers once the lease is free");

            // 同一条线程再次 acquire 拿到的是 WAIT_OBJECT_0（递归）而不是 WAIT_ABANDONED：
            // 这条路径绝不能把上一位留下的粘性未知当成良性残留。
            GuardLease recursive;
            check(recursive.acquire(guard_target, error) == GuardAcquire::Acquired,
                  "action/status-lease the same thread re-acquired (a recursive Win32 mutex)");
            recursive.abandon_unknown();
            check_eq(do_status_in(context, options), 2,
                     "action/status-lease a same-thread re-acquire must not erase the sticky unknown");
            host.kill();
        }
    }

    // ---- 上一位 actor 崩了（真 abandoned）：Busy 之后由下一位解除 ------------------------------
    if (wanted("action/abandon-recovery")) {
        FixtureHost host;
        if (!spawn_fixture_host(host, {{L"DESKFEED_FIXTURE_STATUS_STARTED", L"0"}},
                                /*load_fixture=*/true)) {
            check(false, "action/abandon-recovery could not set up the fixture host");
        } else {
            ActionContext const context = fixture_context(host);
            Options options;
            options.action = L"status";
            options.payload_path = fixture_payload_path();
            options.scope = L"fixture-" + std::to_wstring(::GetCurrentProcessId());
            GuardTarget const guard_target{host.pid, host.creation_time};

            // 一个注定不存在的 TID：起一条线程、记住它的 TID、join 掉。
            unsigned long absent_tid = 0;
            {
                std::thread temp([]() {});
                absent_tid = ::GetThreadId(temp.native_handle());
                temp.join();
            }
            check(absent_tid != 0, "action/abandon-recovery captured a real (now gone) tid");

            HANDLE const release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<bool> holding{false};
            std::thread crasher([&]() {
                GuardLease lease;
                std::wstring acquire_error;
                if (lease.acquire(guard_target, acquire_error) == GuardAcquire::Acquired) {
                    std::wstring write_error;
                    lease.note_remote_call_started(absent_tid, write_error);
                }
                holding.store(true);
                ::WaitForSingleObject(release, 5000);
                // 不结算、不释放：线程退出 ⇒ 内核把 mutex 标成 abandoned。
            });
            while (!holding.load()) {
                ::Sleep(1);
            }
            check_eq(do_status_in(context, options), 2,
                     "action/abandon-recovery a live holder makes the next actor Busy");
            ::SetEvent(release);
            crasher.join();
            check_eq(do_status_in(context, options), 1,
                     "action/abandon-recovery the next actor recovers once the owner thread really "
                     "ended and the recorded tid is gone");
            check(!guard_event_signaled(guard_target),
                  "action/abandon-recovery recovery cleared the sticky event");
            ::CloseHandle(release);
            host.kill();
        }
    }

    // ---- 上一位 actor 在 PendingNoTid 之后崩了：本会话永久未知，下一位绝不清 -------------------
    if (wanted("action/abandon-no-tid-unknown")) {
        FixtureHost host;
        if (!spawn_fixture_host(host, {{L"DESKFEED_FIXTURE_STATUS_STARTED", L"0"}},
                                /*load_fixture=*/true)) {
            check(false, "action/abandon-no-tid-unknown could not set up the fixture host");
        } else {
            ActionContext const context = fixture_context(host);
            Options options;
            options.action = L"status";
            options.payload_path = fixture_payload_path();
            options.scope = L"fixture-" + std::to_wstring(::GetCurrentProcessId());
            GuardTarget const guard_target{host.pid, host.creation_time};

            std::thread crasher([&]() {
                GuardLease lease;
                std::wstring acquire_error;
                if (lease.acquire(guard_target, acquire_error) == GuardAcquire::Acquired) {
                    std::wstring write_error;
                    lease.note_remote_call_pending(write_error);  // 有入口在飞、但还不知道 TID
                }
                // 线程到此结束：abandoned，记录停在 PendingNoTid。
            });
            crasher.join();

            check_eq(do_status_in(context, options), 2,
                     "action/abandon-no-tid-unknown a crash after PendingNoTid keeps the target "
                     "uncertain");
            GuardRecordWatch watch;
            check(watch.open(guard_target), "action/abandon-no-tid-unknown opened the named record");
            payload::GuardRecord record{};
            check(watch.read(record), "action/abandon-no-tid-unknown read the named record");
            check(record.state == payload::kGuardStatePendingNoTid && record.tid == 0,
                  "action/abandon-no-tid-unknown the pending record is left exactly as it was");
            check(guard_event_signaled(guard_target),
                  "action/abandon-no-tid-unknown the sticky uncertainty is set");
            watch.close();
            host.kill();
        }
    }
}

DWORD WINAPI trivial_thread_entry(void*) { return 0; }

void run_host_safety_cases() {
    using namespace deskfeed::probe;
    // ---- 卸载门禁：任何一项事实不成立都**不**允许归还模块引用 --------------------------------
    //
    // 这是宿主收尾那**唯一**一处 FreeLibrary 循环（`report_module_references`）前面的判据。
    // 断言的形状就是"缺什么都不能卸"：工作线程没真退出、停止入口没跑过/报了未退休、有悬挂、
    // feed 模式下状态 POD 不可信 —— 逐条都必须 false。
    if (wanted("hostgate/retirement-no-unload")) {
        UnloadGate perfect;
        perfect.worker_exited = true;
        perfect.stop_ran = true;
        perfect.dangling_clear = true;
        check(unload_allowed(perfect),
              "hostgate/retirement-no-unload a fully proven retirement allows returning references");

        UnloadGate strong = perfect;
        strong.feed_mode = true;
        strong.status_clear = true;
        check(unload_allowed(strong), "hostgate/retirement-no-unload feed with a clear POD allows it");

        // 逐项反例。每一项都对应一个真实缺陷形状：
        UnloadGate worker_alive = perfect;
        worker_alive.worker_exited = false;  // "已经 post 过 WM_CLOSE"不等于线程退出了
        check(!unload_allowed(worker_alive),
              "hostgate/retirement-no-unload a worker that has not really exited never unloads");

        UnloadGate stop_zero = perfect;
        stop_zero.stop_ran = false;  // DeskFeedStop 报 0 = 未退休
        check(!unload_allowed(stop_zero),
              "hostgate/retirement-no-unload a 'not retired' stop never unloads");

        UnloadGate dangling = perfect;
        dangling.dangling_clear = false;  // 还有未证明安全的悬挂
        check(!unload_allowed(dangling),
              "hostgate/retirement-no-unload a dangling hook never unloads");

        UnloadGate feed_untrusted = perfect;
        feed_untrusted.feed_mode = true;
        feed_untrusted.status_clear = false;  // POD 不可信 / thread_alive!=0 / unload_safe!=1
        check(!unload_allowed(feed_untrusted),
              "hostgate/retirement-no-unload feed with an unclear status POD never unloads");

        UnloadGate default_gate;
        check(!unload_allowed(default_gate),
              "hostgate/retirement-no-unload the default (all-unknown) gate is fail closed");
    }

    // ---- 收尾等待：只有**真实** signal 才算退出，超时/活着一律 false ---------------------------
    if (wanted("hostgate/worker-join")) {
        std::atomic<bool> release{false};
        std::thread lingering([&]() {
            while (!release.load()) {
                ::Sleep(5);
            }
        });
        HANDLE const handle = lingering.native_handle();
        check(!wait_thread_exit_pumping(handle, 150),
              "hostgate/worker-join a thread that is still running is not reported as exited");
        release.store(true);
        // 先观察"真的退出了"，再 join（MSVC 的 std::thread::join 会关掉底层句柄）。
        check(wait_thread_exit_pumping(handle, 3000),
              "hostgate/worker-join a really exited thread is observed as exited");
        lingering.join();
        check(!wait_thread_exit_pumping(nullptr, 10),
              "hostgate/worker-join a null handle is never 'exited'");

        // 一条早就跑完、句柄仍然有效的线程：必须立刻观察到。
        HANDLE const finished = ::CreateThread(nullptr, 0, trivial_thread_entry, nullptr, 0, nullptr);
        if (finished == nullptr) {
            check(false, "hostgate/worker-join could not create the trivial thread");
        } else {
            ::WaitForSingleObject(finished, 5000);
            check(wait_thread_exit_pumping(finished, 1000),
                  "hostgate/worker-join an already finished thread is observed immediately");
            ::CloseHandle(finished);
        }
    }
}

// ---------------------------------------------------------------------------
// 探针的 scope 规则与显示邮箱占用
//
// `--scope` 会被带进内核对象名与共享内存名，所以自测宿主必须非空 + 过公共白名单；而"同 scope
// 再来一个生产者"必须被**拒绝**：`DisplayFrameProducer::create` 对已存在的同名 section 照样成功，
// 并且会把帧头 memset 成零（覆写别人的邮箱）。这两条判据与探针运行时用的是同一份实现
// （`probe_scope_ok` 在 probe/host.h，`ProbeScopeClaim` 在 probe/scope_claim.h，后者抢的是
// **主进程同一个**实例仲裁名字 `scoped_name(L"Local\\DeskFeed-master", scope)`）。
// ---------------------------------------------------------------------------
void run_probe_scope_cases() {
    if (wanted("probe/scope-whitelist")) {
        std::wstring error;
        check(!deskfeed::probe::probe_scope_ok(L"", error),
              "probe/scope-whitelist the empty (production) scope is refused");
        check(deskfeed::probe::probe_scope_ok(L"probe-1_x", error),
              "probe/scope-whitelist a 1..32 [A-Za-z0-9_-] name is accepted");
        check(!deskfeed::probe::probe_scope_ok(std::wstring(33, L'a'), error),
              "probe/scope-whitelist a 33-character name is refused");
        check(!deskfeed::probe::probe_scope_ok(L"../evil", error),
              "probe/scope-whitelist path traversal is refused");
        check(!deskfeed::probe::probe_scope_ok(L"a\\b", error),
              "probe/scope-whitelist a separator is refused");
        check(!deskfeed::probe::probe_scope_ok(L"has space", error),
              "probe/scope-whitelist whitespace is refused");
    }

    if (wanted("probe/scope-occupied-master")) {
        // **真实仲裁对象**：主进程的实例锁就是 `CreateMutexW(scoped_name(L"Local\\DeskFeed-master",
        // scope))` + `ERROR_ALREADY_EXISTS`。这里直接建出那个名字（不必 spawn 一个 master），探针
        // 必须看到"已被占用"并拒绝；对象消失之后同一个 scope 又能被探针正常占用。
        std::wstring const scope = L"toolstest-master-" + std::to_wstring(::GetCurrentProcessId());
        std::wstring const name = deskfeed::scoped_name(L"Local\\DeskFeed-master", scope);
        HANDLE const master = ::CreateMutexW(nullptr, FALSE, name.c_str());
        check(master != nullptr, "probe/scope-occupied-master the master arbitration object exists");
        deskfeed::probe::ProbeScopeClaim claim;
        std::string error;
        check(!claim.acquire(scope, error),
              "probe/scope-occupied-master a scope whose master arbitration exists is refused");
        check(!claim.held(), "probe/scope-occupied-master a refused claim holds nothing");
        if (master != nullptr) {
            ::CloseHandle(master);
        }
        check(claim.acquire(scope, error),
              "probe/scope-occupied-master the scope frees when the arbitration object goes away");
        claim.release();
    }

    if (wanted("probe/scope-claim-unique")) {
        // 两个探针抢同一个 scope：恰好一个赢（claim 就是那个"存在即占用"的内核对象）。
        std::wstring const scope = L"toolstest-unique-" + std::to_wstring(::GetCurrentProcessId());
        deskfeed::probe::ProbeScopeClaim first;
        deskfeed::probe::ProbeScopeClaim second;
        std::string error;
        check(first.acquire(scope, error), "probe/scope-claim-unique the first claim wins");
        check(!second.acquire(scope, error), "probe/scope-claim-unique the second claim is refused");
        check(!second.held(), "probe/scope-claim-unique the loser holds nothing");
        first.release();
        check(second.acquire(scope, error), "probe/scope-claim-unique the scope frees after release");
        second.release();
    }

    if (wanted("probe/scope-occupied-live")) {
        // 已有活邮箱：探针必须拒绝，而且**绝不 touch** 那块邮箱——帧头不被 memset、内容照旧。
        // `MockProducer::create` 的第一句就是同一个 claim（见 probe/mock_producer.cpp），所以
        // "拒绝"必然发生在任何 `DisplayFrameProducer::create` 之前。
        std::wstring const scope = L"toolstest-live-" + std::to_wstring(::GetCurrentProcessId());
        deskfeed::DisplayFrameProducer existing;
        std::string error;
        if (!existing.create(scope, nullptr, error)) {
            check(false, "probe/scope-occupied-live could not create the live mailbox");
        } else {
            deskfeed::probe::ProbeScopeClaim claim;
            check(!claim.acquire(scope, error),
                  "probe/scope-occupied-live refuses a scope that already has a live mailbox");
            check(!claim.held(), "probe/scope-occupied-live a refused claim holds nothing");
            // 拒绝必须**真的**保护了那块邮箱：它还是原来那个生产者建的，内容照常发得出去、
            // 读得回来（没有被第二个人的 create memset 掉）。
            deskfeed::Frame frame;
            frame.kind = deskfeed::ViewKind::Content;
            deskfeed::FrameItem item;
            item.id = "cpu";
            item.label = "CPU";
            item.value = "54C";
            frame.items.push_back(item);
            check(existing.publish(frame, 1000),
                  "probe/scope-occupied-live the live mailbox still publishes");
            deskfeed::DisplayFrameConsumer consumer;
            bool const attached = consumer.attach(scope, error);
            check(attached, "probe/scope-occupied-live the live mailbox is still attachable");
            if (attached) {
                deskfeed::Frame read{};
                check(consumer.read(read) && read.items.size() == 1,
                      "probe/scope-occupied-live the live mailbox still carries its own frame");
                consumer.detach();
            }
            existing.close();
        }
    }
}

// ---------------------------------------------------------------------------
// G-G：退休门禁的**真实**证据（retained fixture 的 ordinary-detach 标记）
//
// 夹具模块只在普通 detach（`lpReserved == nullptr`，= 真的有人 `FreeLibrary`）时写标记文件；
// 进程退出（`lpReserved != nullptr`）绝不写。于是：
//   Stop == 0 ⇒ 子进程必须不卸载、exit 1、标记 ABSENT ——"进程结束了"本身不是 Free 的证据；
//   Stop == 1 ⇒ 子进程卸载、refs == 1、exit 0、标记 PRESENT。
// 卸载决策用**同一份** `probe/host_gate.h::unload_allowed`：把它变异成"无视未退休"之后，Stop=0
// 那一轮会真的卸载（标记出现 + exit 0），用例必红。
// ---------------------------------------------------------------------------

std::wstring retained_fixture_path() {
    return fixture_dir() + L"\\deskfeed-test-retained-fixture.dll";
}

// 子进程模式：LoadLibrary 夹具 → 生产 ABI 各调一次 → 用真实的卸载门禁决定要不要 FreeLibrary。
int run_retained_fixture_child(std::wstring const& dll_path) {
    using namespace deskfeed::probe;
    HMODULE const module = ::LoadLibraryW(dll_path.c_str());
    if (module == nullptr) {
        std::printf("retained-child: LoadLibrary failed (%lu)\n", ::GetLastError());
        return 3;
    }
    auto start = reinterpret_cast<DWORD(WINAPI*)(void*)>(::GetProcAddress(module, "DeskFeedStart"));
    auto stop = reinterpret_cast<DWORD(WINAPI*)(void*)>(::GetProcAddress(module, "DeskFeedStop"));
    auto status =
        reinterpret_cast<DWORD(WINAPI*)(void*)>(::GetProcAddress(module, "DeskFeedGetStatus"));
    if (start == nullptr || stop == nullptr || status == nullptr) {
        std::printf("retained-child: the fixture does not export the production ABI\n");
        return 4;
    }
    std::wstring scope = L"retained-fixture";
    DWORD const started = start(&scope);
    deskfeed::payload::PayloadStatusPod pod{};
    pod.size = sizeof(pod);
    pod.abi = deskfeed::payload::kPayloadStatusAbi;
    pod.width = deskfeed::payload::kPayloadStatusWidth;
    DWORD const status_ok = status(&pod);
    DWORD const stopped = stop(reinterpret_cast<void*>(static_cast<std::uintptr_t>(10000)));

    UnloadGate gate;
    gate.worker_exited = true;  // 本子进程的"工作线程"就是它自己，已经走到这里
    gate.stop_ran = stopped == 1;
    gate.dangling_clear = true;
    gate.feed_mode = true;
    gate.status_clear =
        status_ok == 1 && pod.thread_alive == 0 && pod.unload_safe == 1 && pod.dangling == 0;
    std::printf("retained-child: start=%lu status=%lu stop=%lu\n", started, status_ok, stopped);
    if (!unload_allowed(gate)) {
        std::printf("retained-child: the gate refuses to unload; leaving the module mapped\n");
        return 1;  // 绝不 FreeLibrary：进程退出的 DETACH(lpReserved != nullptr) 不写标记
    }
    int references = 0;
    while (::FreeLibrary(module)) {
        ++references;
    }
    std::printf("retained-child: freed, references=%d\n", references);
    return references == 1 ? 0 : 5;
}

// 只终止**本用例自己** CreateProcessW 出来的那个 PID；绝不按镜像名杀进程。
bool run_retained_fixture_case(bool stop_ok, std::wstring const& marker, DWORD& exit_code) {
    std::wstring const dll = retained_fixture_path();
    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    ::DeleteFileW(marker.c_str());
    std::wstring const command = L"\"" + std::wstring(deskfeed::module_self_path()) +
                                 L"\" --retained-fixture-child \"" + dll + L"\"";
    std::wstring const block = build_environment({
        {L"DESKFEED_FIXTURE_STOP_CODE", stop_ok ? L"1" : L"0"},
        {L"DESKFEED_FIXTURE_DETACH_MARKER", marker},
    });
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, const_cast<wchar_t*>(command.c_str()), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                         const_cast<wchar_t*>(block.c_str()), nullptr, &startup, &process) == FALSE) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, 30000);
    exit_code = 0;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

void run_retained_fixture_cases() {
    if (!wanted("hostgate/retained-fixture")) {
        return;
    }
    std::wstring const dir = temp_dir();
    std::wstring const marker_bad = dir + L"\\retained-stop0.marker";
    DWORD exit_code = 0;
    if (!run_retained_fixture_case(/*stop_ok=*/false, marker_bad, exit_code)) {
        check(false, "hostgate/retained-fixture the fixture child could not be launched");
        return;
    }
    check_eq(exit_code, static_cast<DWORD>(1),
             "hostgate/retained-fixture Stop=0 makes the child exit 1 without unloading");
    check(::GetFileAttributesW(marker_bad.c_str()) == INVALID_FILE_ATTRIBUTES,
          "hostgate/retained-fixture Stop=0 never ran FreeLibrary (no ordinary-detach marker)");

    std::wstring const marker_ok = dir + L"\\retained-stop1.marker";
    if (!run_retained_fixture_case(/*stop_ok=*/true, marker_ok, exit_code)) {
        check(false, "hostgate/retained-fixture the healthy fixture child could not be launched");
        return;
    }
    check_eq(exit_code, static_cast<DWORD>(0),
             "hostgate/retained-fixture a healthy Stop=1 child exits 0 with references==1");
    check(::GetFileAttributesW(marker_ok.c_str()) != INVALID_FILE_ATTRIBUTES,
          "hostgate/retained-fixture a healthy Stop=1 really ran FreeLibrary (marker present)");
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            g_filter = argv[++index];
        } else if (std::strcmp(argv[index], "--mailbox-owner") == 0 && index + 1 < argc) {
            // 子进程模式：只当"显示邮箱的所有者"，给父用例制造"attach 之后所有者死亡"。
            std::string const narrow(argv[++index]);
            std::wstring scope;
            scope.reserve(narrow.size());
            for (char const character : narrow) {
                scope.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
            }
            return run_mailbox_owner_child(scope);
        } else if (std::strcmp(argv[index], "--remote-writer") == 0) {
            // 子进程模式：只装载延迟写 DLL 并停在那里，给父用例制造真实的远程调用超时。
            return run_remote_writer_child();
        } else if (std::strcmp(argv[index], "--retained-fixture-child") == 0 && index + 1 < argc) {
            // 子进程模式：装载 retained fixture，走一遍生产 ABI，再用真实的卸载门禁决定要不要
            // FreeLibrary；夹具的 ordinary-detach 标记是"到底有没有卸"的机械证据。
            std::string const narrow(argv[++index]);
            return run_retained_fixture_child(widen(narrow.c_str()));
        }
    }

    std::wstring const dir = temp_dir();
    run_pe_cases(dir);
    run_identity_cases(dir);
    run_decision_cases();
    run_reading_cases();
    run_mailbox_case();
    run_mailbox_latest_cases();
    run_mailbox_owner_death_case();
    run_remote_late_write_case();
    run_park_cases();
    run_guard_cases();
    run_leased_action_cases();
    run_host_safety_cases();
    run_probe_scope_cases();
    run_retained_fixture_cases();
    run_state_gate_cases();
    run_frame_cases();
    run_gate_cases();

    // 清理：合成文件与目录都删掉，别在 %TEMP% 里留东西。
    std::wstring pattern = dir + L"\\*.dll";
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            ::DeleteFileW((dir + L"\\" + found.cFileName).c_str());
        } while (::FindNextFileW(search, &found));
        ::FindClose(search);
    }
    ::RemoveDirectoryW(dir.c_str());

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
