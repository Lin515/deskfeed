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
#include "inject/pe_export.h"
#include "inject/remote.h"
#include "inject/target.h"
#include "platform/paths.h"
#include "probe/host_gate.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <sstream>
#include <string>
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
