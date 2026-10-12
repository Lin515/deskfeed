// deskfeed-tools-tests.exe —— 注入器与探针工具的**离线**用例。
//
// 覆盖的都是"不需要 explorer、不需要负载 DLL、不需要 XAML"的那部分：
//   * 严格 PE 解析：合成的坏文件 fixtures 必须被逐项拒绝（0 入口、越出 image、导出的是数据、
//     目录越界、PE32/x86、转发导出、名字不落在本节里的）。
//   * 卸载/装载门禁的纯决策表（超时不释放远程缓冲、线程没退出就不算装载成功）。
//   * 完整路径匹配：同名不同目录**不算**命中。
//   * 实例名白名单。
//   * 显示量具的纯排版口径（与窗口/任务栏同一份 display/reading）。
//   * 显示邮箱的收发一轮（隔离 scope，自己起生产者与消费者）。
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

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    put32(nt + 8, 0);                                  // TimeDateStamp
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

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            g_filter = argv[++index];
        }
    }

    std::wstring const dir = temp_dir();
    run_pe_cases(dir);
    run_decision_cases();
    run_reading_cases();
    run_mailbox_case();

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
